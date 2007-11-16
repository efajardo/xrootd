/******************************************************************************/
/*                                                                            */
/*                      X r d C m s C l u s t e r . c c                       */
/*                                                                            */
/* (c) 2007 by the Board of Trustees of the Leland Stanford, Jr., University  */
/*                            All Rights Reserved                             */
/*   Produced by Andrew Hanushevsky for Stanford University under contract    */
/*              DE-AC02-76-SFO0515 with the Department of Energy              */
/******************************************************************************/

//         $Id$

// Original Version: 1.38 2007/07/26 15:18:24 ganis

const char *XrdCmsClusterCVSID = "$Id$";

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/types.h>

#include "XProtocol/YProtocol.hh"
  
#include "Xrd/XrdJob.hh"
#include "Xrd/XrdLink.hh"
#include "Xrd/XrdScheduler.hh"

#include "XrdCms/XrdCmsCache.hh"
#include "XrdCms/XrdCmsConfig.hh"
#include "XrdCms/XrdCmsCluster.hh"
#include "XrdCms/XrdCmsManager.hh"
#include "XrdCms/XrdCmsNode.hh"
#include "XrdCms/XrdCmsState.hh"
#include "XrdCms/XrdCmsSelect.hh"
#include "XrdCms/XrdCmsTrace.hh"
#include "XrdCms/XrdCmsTypes.hh"

#include "XrdNet/XrdNetDNS.hh"

#include "XrdOuc/XrdOucPup.hh"

#include "XrdSys/XrdSysPlatform.hh"
#include "XrdSys/XrdSysPthread.hh"
#include "XrdSys/XrdSysTimer.hh"

using namespace XrdCms;

/******************************************************************************/
/*                        G l o b a l   O b j e c t s                         */
/******************************************************************************/

       XrdCmsCluster   XrdCms::Cluster;

/******************************************************************************/
/*                      L o c a l   S t r u c t u r e s                       */
/******************************************************************************/
  
class XrdCmsDrop : XrdJob
{
public:

     void DoIt() {Cluster.STMutex.Lock();
                  Cluster.Drop(nodeEnt, nodeInst, this);
                  Cluster.STMutex.UnLock();
                 }

          XrdCmsDrop(int nid, int inst) : XrdJob("drop node")
                    {nodeEnt  = nid;
                     nodeInst = inst;
                     Sched->Schedule((XrdJob *)this, time(0)+Config.DRPDelay);
                    }
         ~XrdCmsDrop() {}

int  nodeEnt;
int  nodeInst;
};
  
/******************************************************************************/
/*                           C o n s t r u c t o r                            */
/******************************************************************************/

XrdCmsCluster::XrdCmsCluster()
{
     memset((void *)NodeTab, 0, sizeof(NodeTab));
     memset((void *)NodeBat, 0, sizeof(NodeBat));
     memset((void *)AltMans, (int)' ', sizeof(AltMans));
     AltMend = AltMans;
     AltMent = -1;
     NodeCnt =  0;
     STHi    = -1;
     XWait   = 0;
     XnoStage= 0;
     SelAcnt = 0;
     SelRcnt = 0;
     doReset = 0;
     resetMask = 0;
     peerHost  = 0;
     peerMask  = 0; peerMask = ~peerMask;
}
  
/******************************************************************************/
/*                                   A d d                                    */
/******************************************************************************/
  
XrdCmsNode *XrdCmsCluster::Add(const char *Role, XrdLink *lp,
                               int port,  int Status,
                               int sport, const char *theNID)
{
   EPNAME("Add")
    sockaddr InetAddr;
    const char *act = "Added ";
    const char *hnp = lp->Name(&InetAddr);
    unsigned int ipaddr = XrdNetDNS::IPAddr(&InetAddr);
    XrdCmsNode *nP = 0;
    int Slot, Free = -1, Bump1 = -1, Bump2 = -1, Bump3 = -1;
    int tmp, Special = (Status & (CMS_isMan|CMS_isPeer));
    XrdSysMutexHelper STMHelper(STMutex);

// Find available slot for this node. Here are the priorities:
// Slot  = Reconnecting node
// Free  = Available slot           ( 1st in table)
// Bump1 = Disconnected server      (last in table)
// Bump2 = Connected    server      (last in table)
// Bump3 = Disconnected managr/peer ( 1st in table) if new one is managr/peer
//
   for (Slot = 0; Slot < STMax; Slot++)
       if (NodeBat[Slot])
          {if (NodeBat[Slot]->isNode(ipaddr, port, theNID)) break;
//Conn//   if (NodeTab[Slot])
              {if (!NodeBat[Slot]->isPerm)   Bump2 = Slot; // Last conn Server
//Disc//      } else {
               if ( NodeBat[Slot]->isPerm)
                  {if (Bump3 < 0 && Special) Bump3 = Slot;}//  1st disc Man/Pr
                  else                       Bump1 = Slot; // Last disc Server
              }
          } else if (Free < 0)               Free  = Slot; //  1st free slot

// Check if node is already logged in or is a relogin
//
   if (Slot < STMax)
      if (NodeTab[Slot] && NodeTab[Slot]->isBound)
         {Say.Emsg("Cluster", Role, hnp, "already logged in.");
          return 0;
         } else { // Rehook node to previous unconnected entry
          nP = NodeBat[Slot];
          nP->Link      = lp;
          nP->isOffline = 0;
          nP->isConn    = 1;
          nP->Instance++;
          nP->setName(Role, lp, port);  // Just in case it changed
          NodeTab[Slot] = nP;
          act = "Re-added ";
         }

// Reuse an old ID if we must or redirect the incomming node
//
   if (!nP) 
      {if (Free >= 0) Slot = Free;
          else {if (Bump1 >= 0) Slot = Bump1;
                   else Slot = (Bump2 >= 0 ? Bump2 : Bump3);
                if (Slot < 0)
                   {if (Status & CMS_isPeer) Say.Emsg("Cluster", "Add peer", hnp,
                                                "failed; too many subscribers.");
                       else {sendAList(lp);
                             DEBUG(hnp <<" redirected; too many subscribers.");
                            }
                    return 0;
                   }

                if (NodeTab[Slot] && !(Status & CMS_isPeer)) sendAList(lp);

                DEBUG(hnp << " bumps " << NodeBat[Slot]->Ident <<" #" <<Slot);
                NodeBat[Slot]->Lock();
                Remove("redirected", NodeBat[Slot], -1);
                act = "Shoved ";
               }
       nP = new XrdCmsNode(Role, lp, port, theNID, 0, Slot);
       NodeTab[Slot] = NodeBat[Slot] = nP;
      }

// Indicate whether this snode can be redirected
//
   nP->isPerm = (Status & (CMS_isMan | CMS_isPeer)) ? CMS_Perm : 0;

// Assign new server
//
   if (Status & CMS_isMan) setAltMan(Slot, ipaddr, sport);
   if (Slot > STHi) STHi = Slot;
   nP->isBound   = 1;
   nP->isConn    = 1;
   nP->isNoStage = (Status & CMS_noStage);
   nP->isSuspend = (Status & CMS_Suspend);
   nP->isMan     = (Status & CMS_isMan);
   nP->isPeer    = (Status & CMS_isPeer);
   nP->isDisable = 1;
   NodeCnt++;
   if (Config.SUPLevel
   && (tmp = NodeCnt*Config.SUPLevel/100) > Config.SUPCount)
      Config.SUPCount=tmp;

// Compute new peer mask, as needed
//
   if (nP->isPeer) peerHost |=  nP->NodeMask;
      else         peerHost &= ~nP->NodeMask;
   peerMask = ~peerHost;

// Document login
//
   DEBUG(act <<nP->Ident <<" to cluster; id=" <<Slot <<'.' <<nP->Instance
         <<"; num=" <<NodeCnt <<"; min=" <<Config.SUPCount);

// Compute new state of all nodes if we are a reporting manager.
//
   if (Config.asManager()) CmsState.Calc(1, nP->isNoStage, nP->isSuspend);

// All done
//
   return nP;
}

/******************************************************************************/
/*                             B r o a d c a s t                              */
/******************************************************************************/

void XrdCmsCluster::Broadcast(SMask_t smask, const struct iovec *iod, 
                              int iovcnt, int iotot)
{
   int i;
   XrdCmsNode *nP;
   SMask_t bmask;

// Obtain a lock on the table and screen out peer nodes
//
   STMutex.Lock();
   bmask = smask & peerMask;

// Run through the table looking for nodes to send messages to
//
   for (i = 0; i <= STHi; i++)
       {if ((nP = NodeTab[i]) && nP->isNode(bmask) && !nP->isOffline)
           nP->Lock();
           else continue;
        STMutex.UnLock();
        nP->Send(iod, iovcnt, iotot);
        nP->UnLock();
        STMutex.Lock();
       }
   STMutex.UnLock();
}

/******************************************************************************/

void XrdCmsCluster::Broadcast(SMask_t smask, XrdCms::CmsRRHdr &Hdr,
                              char *Data,    int Dlen)
{
   struct iovec ioV[3], *iovP = &ioV[1];
   unsigned short Temp;
   int Blen;

// Construct packed data for the character argument. If data is a string then
// Dlen must include the null byte if it is specified at all.
//
   Blen  = XrdOucPup::Pack(&iovP, Data, Temp, (Dlen ? strlen(Data)+1 : Dlen));
   Hdr.datalen = htons(static_cast<unsigned short>(Blen));

// Complete the iovec and send off the data
//
   ioV[0].iov_base = (char *)&Hdr; ioV[0].iov_len = sizeof(Hdr);
   Broadcast(smask, ioV, 3, Blen+sizeof(Hdr));
}

/******************************************************************************/

void XrdCmsCluster::Broadcast(SMask_t smask, XrdCms::CmsRRHdr &Hdr,
                              void *Data,    int Dlen)
{
   struct iovec ioV[2] = {{(char *)&Hdr, sizeof(Hdr)}, {(char *)Data, Dlen}};

// Send of the data as eveything was constructed properly
//
   Broadcast(smask, ioV, 2, Dlen+sizeof(Hdr));
}

/******************************************************************************/
/*                               g e t M a s k                                */
/******************************************************************************/

SMask_t XrdCmsCluster::getMask(unsigned int IPv4adr)
{
   int i;
   XrdCmsNode *nP;
   SMask_t smask = 0;

// Obtain a lock on the table
//
   STMutex.Lock();

// Run through the table looking for a node with matching IP address
//
   for (i = 0; i <= STHi; i++)
       if ((nP = NodeTab[i]) && nP->isNode(IPv4adr))
          {smask = nP->NodeMask; break;}

// All done
//
   STMutex.UnLock();
   return smask;
}

/******************************************************************************/
/*                                  L i s t                                   */
/******************************************************************************/
  
XrdCmsSelected *XrdCmsCluster::List(SMask_t mask, CmsLSOpts opts)
{
    const char *reason;
    int i, iend, nump, delay, lsall = opts & LS_All;
    XrdCmsNode     *nP;
    XrdCmsSelected *sipp = 0, *sip;

// If only one wanted, the select appropriately
//
   STMutex.Lock();
   iend = (opts & LS_Best ? 0 : STHi);
   for (i = 0; i <= iend; i++)
       {if (opts & LS_Best)
            nP = (Config.sched_RR
                 ? SelbyRef( mask, nump, delay, &reason, 0)
                 : SelbyLoad(mask, nump, delay, &reason, 0));
           else if (((nP = NodeTab[i]) || (nP = NodeBat[i]))
                &&  !lsall && !(nP->NodeMask & mask)) nP = 0;
        if (nP)
           {sip = new XrdCmsSelected((opts & LS_IPO) ? 0 : nP->Name(), sipp);
            if (opts & LS_IPV6)
               {sip->IPV6Len = nP->IPV6Len;
                strcpy(sip->IPV6, nP->IPV6);
               }
            sip->Mask    = nP->NodeMask;
            sip->Id      = nP->NodeID;
            sip->IPAddr  = nP->IPAddr;
            sip->Port    = nP->Port;
            sip->Load    = nP->myLoad;
            sip->Util    = nP->DiskUtil;
            sip->RefTotA = nP->RefTotA + nP->RefA;
            sip->RefTotR = nP->RefTotR + nP->RefR;
            if (nP->isOffline) sip->Status  = XrdCmsSelected::Offline;
               else sip->Status  = 0;
            if (nP->isDisable) sip->Status |= XrdCmsSelected::Disable;
            if (nP->isNoStage) sip->Status |= XrdCmsSelected::NoStage;
            if (nP->isSuspend) sip->Status |= XrdCmsSelected::Suspend;
            if (nP->isRW     ) sip->Status |= XrdCmsSelected::isRW;
            if (nP->isMan    ) sip->Status |= XrdCmsSelected::isMangr;
            if (nP->isPeer   ) sip->Status |= XrdCmsSelected::isPeer;
            if (nP->isProxy  ) sip->Status |= XrdCmsSelected::isProxy;
            nP->UnLock();
            sipp = sip;
           }
       }
   STMutex.UnLock();

// Return result
//
   return sipp;
}
  
/******************************************************************************/
/*                                L o c a t e                                 */
/******************************************************************************/

int XrdCmsCluster::Locate(XrdCmsSelect &Sel)
{
   XrdCmsPInfo   pinfo;
   XrdCmsCInfo   cinfo;
   SMask_t       qfVec = 0;

// Find out who serves this path
//
   if (!Cache.Paths.Find(Sel.Path, pinfo) || !pinfo.rovec)
      {Sel.Vec.hf = Sel.Vec.pf = Sel.Vec.wf = 0;
       return -1;
      }

// Complete the request info object if we have one
//
   if (Sel.InfoP)
      {Sel.InfoP->rwVec = pinfo.rwvec;
       Sel.InfoP->isLU  = 1;
      }

// First check if we have seen this file before. If so, get nodes that have it.
// A Refresh request kills this because it's as if we hadn't seen it before.
// If the file was found but either a query is in progress or we have a server
// bounce; the client must wait.
//
   if (Sel.Opts & XrdCmsSelect::Refresh 
   ||  !Cache.GetFile(Sel.Path, cinfo, 0, Sel.InfoP))
      {Cache.AddFile(Sel.Path, 0, Sel.Opts, Config.LUPDelay, Sel.InfoP);
       qfVec = pinfo.rovec; Sel.Vec.hf=Sel.Vec.pf=Sel.Vec.wf=0;
      } else if (cinfo.sbvec != 0)
                {Cache.DelFile(Sel.Path, cinfo.sbvec, Config.LUPDelay);
                 qfVec = cinfo.sbvec; Sel.Vec.hf=Sel.Vec.pf=Sel.Vec.wf=0;
                }

// Check if we have to ask any nodes if they have the file
//
   if (qfVec)
      {CmsStateRequest QReq = {{0, kYR_state, 0, 0}};
       if (Sel.Opts & XrdCmsSelect::Refresh)
          QReq.Hdr.modifier = CmsStateRequest::kYR_refresh;
       Cluster.Broadcast(qfVec, QReq.Hdr, Sel.Path, Sel.PLen);
       return (Sel.InfoP && Sel.InfoP->Key ? -2 : Config.LUPDelay);
      }

// Return a delay if we have some information but a query is in progress
//
   if (cinfo.deadline)
      return (Sel.InfoP && Sel.InfoP->Key ? -2 : Config.LUPDelay);

// Return to the client who has what
//
   Sel.Vec.wf = pinfo.rwvec;
   Sel.Vec.hf = cinfo.hfvec;
   Sel.Vec.pf = cinfo.pfvec;
   return 0;
}
  
/******************************************************************************/
/*                               M o n P e r f                                */
/******************************************************************************/
  
void *XrdCmsCluster::MonPerf()
{
   CmsPingRequest   Ping  = {{0, kYR_ping,  0, 0}};
   CmsUsageRequest  Usage = {{0, kYR_usage, 0, 0}};
   CmsRRHdr *Req;
   XrdCmsNode *nP;
   int i, doping = 0;

// Sleep for the indicated amount of time, then maintain load on each server
//
   while(Config.AskPing)
        {XrdSysTimer::Snooze(Config.AskPing);
         if (--doping < 0) doping = Config.AskPerf;
         STMutex.Lock();
         for (i = 0; i <= STHi; i++)
             if ((nP = NodeTab[i]) && nP->isBound)
                {nP->Lock();
                 STMutex.UnLock();
                 if (nP->PingPong <= 0) Remove("not responding", nP);
                    else {Req = (doping || !Config.AskPerf)
                              ? (CmsRRHdr *)&Ping.Hdr:(CmsRRHdr *)&Usage.Hdr;
                          nP->Send((char *)Req, sizeof(CmsRRHdr));
                         }
                 nP->PingPong--;
                 nP->UnLock();
                 STMutex.Lock();
                }
         STMutex.UnLock();
        }
   return (void *)0;
}
  
/******************************************************************************/
/*                               M o n R e f s                                */
/******************************************************************************/
  
void *XrdCmsCluster::MonRefs()
{
   XrdCmsNode *nP;
   int  i, snooze_interval = 10*60, loopmax, loopcnt = 0;
   int resetA, resetR, resetAR;

// Compute snooze interval
//
   if ((loopmax = Config.RefReset / snooze_interval) <= 1)
      if (!Config.RefReset) loopmax = 0;
         else {loopmax = 1; snooze_interval = Config.RefReset;}

// Sleep for the snooze interval. If a reset was requested then do a selective
// reset unless we reached our snooze maximum and enough selections have gone
// by; in which case, do a global reset.
//
   do {XrdSysTimer::Snooze(snooze_interval);
       loopcnt++;
       STMutex.Lock();
       resetA  = (SelAcnt >= Config.RefTurn);
       resetR  = (SelRcnt >= Config.RefTurn);
       resetAR = (loopmax && loopcnt >= loopmax && (resetA || resetR));
       if (doReset || resetAR)
           {for (i = 0; i <= STHi; i++)
                if ((nP = NodeTab[i])
                &&  (resetAR || (doReset && nP->isNode(resetMask))) )
                    {nP->Lock();
                     if (resetA || doReset) {nP->RefTotA += nP->RefA;nP->RefA=0;}
                     if (resetR || doReset) {nP->RefTotR += nP->RefR;nP->RefR=0;}
                     nP->UnLock();
                    }
            if (resetAR)
               {if (resetA) SelAcnt = 0;
                if (resetR) SelRcnt = 0;
                loopcnt = 0;
               }
            if (doReset) {doReset = 0; resetMask = 0;}
           }
       STMutex.UnLock();
      } while(1);
   return (void *)0;
}

/******************************************************************************/
/*                                R e m o v e                                 */
/******************************************************************************/

// Warning! The node object must be locked upon entry. The lock is released
//          prior to returning to the caller. This entry obtains the node
//          table lock. When immed != 0 then the node is immediately dropped.
//          When immed if < 0 then the caller already holds the STMutex and it 
//          is not released upon exit.

void XrdCmsCluster::Remove(const char *reason, XrdCmsNode *theNode, int immed)
{
   EPNAME("Remove_Node")
   struct theLocks
          {XrdSysMutex *myMutex;
           XrdCmsNode  *myNode;
           int          myImmed;

                       theLocks(XrdSysMutex *mtx, XrdCmsNode *node, int immed)
                               {myImmed = immed; myNode = node; myMutex = mtx;
                                if (myImmed >= 0) myMutex->Lock();
                               }
                      ~theLocks()
                               {if (myImmed >= 0) myMutex->UnLock();
                                if (myNode) myNode->UnLock();
                               }
          } LockHandler(&STMutex, theNode, immed);

   int Inst, NodeID = theNode->ID(Inst);

// Handle Locks Obtained a lock on the node table. Mark node as being offline
//
   theNode->isOffline = 1;

// If the node is connected the simply close the connection. This will cause
// the connection handler to re-initiate the node removal. The LockHandler
// destructor will release the node table and node object locks as needed.
//
   if (theNode->isConn)
      {theNode->Disc(reason, 0);
       return;
      }

// If the node is part of the cluster, do not count it anymore
//
   if (theNode->isBound) {theNode->isBound = 0; NodeCnt--;}

// Compute new state of all nodes if we are a reporting manager
//
   if (Config.asManager()) 
      CmsState.Calc(-1, theNode->isNoStage, theNode->isSuspend);

// If this is an immediate drop request, do so now. Drop() will delete
// the node object and remove the node lock. Som tell LockHandler that.
//
   if (immed || !Config.DRPDelay) 
      {Drop(NodeID, Inst);
       LockHandler.myNode = 0;
       return;
      }

// If a drop job is already scheduled, update the instance field. Otherwise,
// Schedule a node drop at a future time.
//
   theNode->DropTime = time(0)+Config.DRPDelay;
   if (theNode->DropJob) theNode->DropJob->nodeInst = Inst;
      else theNode->DropJob = new XrdCmsDrop(NodeID, Inst);

// Document removal
//
   if (reason) 
      Say.Emsg("Manager", theNode->Ident, "scheduled for removal;", reason);
      else DEBUG("Will remove " <<theNode->Ident <<" node "
                 <<NodeID <<'.' <<Inst);
}

/******************************************************************************/
/*                                R e s u m e                                 */
/******************************************************************************/

void XrdCmsCluster::Resume()
{
     static CmsStatusRequest myState = {{0, kYR_status, 
                                            CmsStatusRequest::kYR_Resume, 0}};
     static const int        szReqst = sizeof(CmsStatusRequest);

// If the suspend file is still present, ignore this resume request
//
   if (Config.inSuspend())
      Say.Emsg("Manager","Resume request ignored; suspend file present.");
      else {XWait = 0;
            Manager.Inform("resume", (char *)&myState, szReqst);
           }
}

/******************************************************************************/
/*                              R e s e t R e f                               */
/******************************************************************************/
  
void XrdCmsCluster::ResetRef(SMask_t smask)
{

// Obtain a lock on the table
//
   STMutex.Lock();

// Inform the reset thread that we need a reset
//
   doReset = 1;
   resetMask |= smask;

// Unlock table and exit
//
   STMutex.UnLock();
}

/******************************************************************************/
/*                                S e l e c t                                 */
/******************************************************************************/
  
int XrdCmsCluster::Select(XrdCmsSelect &Sel)
{
   XrdCmsPInfo pinfo;
   XrdCmsCInfo cinfo;
   const char *Amode;
   int dowt = 0, retc, isRW;
   SMask_t amask, smask, pmask;

// Establish some local options
//
   if (Sel.Opts & XrdCmsSelect::Write) {isRW = 1; Amode = "write";}
      else                             {isRW = 0; Amode = "read";}

// Find out who serves this path
//
   if (!Cache.Paths.Find(Sel.Path, pinfo)
   || (amask = ((isRW ? pinfo.rwvec : pinfo.rovec) & Sel.nmask)) == 0)
      {Sel.Resp.DLen = snprintf(Sel.Resp.Data, sizeof(Sel.Resp.Data)-1,
                       "No servers have %s access to the file", Amode)+1;
       return -1;
      }

// First check if we have seen this file before. If so, get primary selections.
//
   if (Sel.Opts & XrdCmsSelect::Refresh) {retc = 0; pmask = 0;}
      else if (!(retc = Cache.GetFile(Sel.Path,cinfo,isRW,Sel.InfoP))) pmask=0;
              else pmask = (isRW ? cinfo.hfvec & pinfo.rwvec
                                 : cinfo.hfvec & Sel.nmask);

// We didn't find the file or a refresh is wanted (easy case) or the file was
// found but either a query is in progress. In either case, the client must
// either wait a for full period or a short period if its in the pending queue.
// If we have a server bounce the client waits if no alternative is available. 
//
   if (!retc)
      {Cache.AddFile(Sel.Path, 0, Sel.Opts, Config.LUPDelay, Sel.InfoP);
       cinfo.sbvec = pinfo.rovec;
       dowt = (Sel.InfoP && Sel.InfoP->Key ? -1 : 1);// Placed in pending state
      } else {
       if (cinfo.sbvec != 0) Cache.DelFile(Sel.Path,cinfo.sbvec,Config.LUPDelay);
       if (cinfo.deadline) dowt = (Sel.InfoP && Sel.InfoP->Key ? -1 : 1);
          else             dowt = 0;
      }

// Check if we have to ask any nodes if they have the file
//
   if (cinfo.sbvec != 0)
      {CmsStateRequest QReq = {{0, kYR_state, 0, 0}};
       if (Sel.Opts & XrdCmsSelect::Refresh)
          QReq.Hdr.modifier = CmsStateRequest::kYR_refresh;
       Cluster.Broadcast(cinfo.sbvec, QReq.Hdr, Sel.Path, Sel.PLen);
      }

// If the client has to wait now, delay the client and return
//
   if (dowt) return (dowt > 0 ? Config.LUPDelay : 0);

// Compute the primary and secondary selections:
// Primary:   Servers who already have the file (computed above)
// Secondary: Servers who don't have the file but can get it
//
   if (Sel.Opts & XrdCmsSelect::Online) smask = 0;
      else smask = (Sel.Opts & XrdCmsSelect::NewFile ?
                    Sel.nmask & pinfo.rwvec: amask & pinfo.ssvec); // Alternates

// Select a node
//
   if ((!pmask && !smask) || (retc = SelNode(Sel, pmask, smask)) < 0)
      {Sel.Resp.DLen = snprintf(Sel.Resp.Data, sizeof(Sel.Resp.Data)-1,
                       "No servers are available to %s the file.", Amode)+1;
       return -1;
      }

// All done
//
   return retc;
}

/******************************************************************************/
  
int XrdCmsCluster::Select(int isrw, SMask_t pmask,
                          int &port, char *hbuff, int &hlen)
{
   static const SMask_t smLow = 255;
   XrdCmsNode *nP = 0;
   SMask_t tmask;
   int Snum = 0;

// Compute the a single node number that is contained in the mask
//
   if (!pmask) return 0;
   do {if (!(tmask = pmask & smLow)) Snum += 8;
         else {while((tmask = tmask>>1)) Snum++; break;}
      } while((pmask = pmask >> 8));

// See if the node passes muster
//
   STMutex.Lock();
   if ((nP = NodeTab[Snum]))
      {if (nP->isOffline || nP->isSuspend || nP->isDisable)      nP = 0;
          else if (!Config.sched_RR
               && (nP->myLoad > Config.MaxLoad))                 nP = 0;
       if (nP)
          if (isrw)
             if (nP->isNoStage || nP->DiskFree < Config.DiskMin) nP = 0;
                else {SelAcnt++; nP->Lock();}
            else     {SelRcnt++; nP->Lock();}
      }
   STMutex.UnLock();

// At this point either we have a node or we do not
//
   if (nP)
      {strcpy(hbuff, nP->Name(hlen, port));
       nP->RefR++;
       nP->UnLock();
       return 1;
      }
   return 0;
}

/******************************************************************************/
/*                                 S p a c e                                  */
/******************************************************************************/
  
void XrdCmsCluster::Space(int none, int doinform)
{
     static const int         szReqst = sizeof(CmsStatusRequest);
     static CmsStatusRequest  okSpace = {{0, kYR_status, 
                                             CmsStatusRequest::kYR_Stage,   0}};
     static CmsStatusRequest  noSpace = {{0, kYR_status, 
                                             CmsStatusRequest::kYR_noStage, 0}};
            CmsStatusRequest *qSpace;
            const char       *What;
            int               PStage;

     if (Config.asSolo()) return;

     if (none) {qSpace = &noSpace; What = "nostage";}
        else   {qSpace = &okSpace; What = "stage";}

     XXMutex.Lock();
     PStage = XnoStage;
     if (none) {XnoStage |=  CMS_noSpace; PStage = !PStage;}
        else    XnoStage &= ~CMS_noSpace;
     if (doinform && PStage) Manager.Inform(What, (char *)qSpace, szReqst);
     XXMutex.UnLock();
}

/******************************************************************************/
/*                                 S t a g e                                  */
/******************************************************************************/
  
void XrdCmsCluster::Stage(int ison, int doinform)
{
     static const int         szReqst = sizeof(CmsStatusRequest);
     static CmsStatusRequest  okStage = {{0, kYR_status,
                                             CmsStatusRequest::kYR_Stage,   0}};
     static CmsStatusRequest  noStage = {{0, kYR_status,
                                             CmsStatusRequest::kYR_noStage, 0}};
            CmsStatusRequest *qStage = (ison ? &okStage : &noStage);
            const char       *What;
            int               PStage;

     if (ison) {qStage = &noStage; What = "nostage";}
        else   {qStage = &okStage; What = "stage";}

     XXMutex.Lock();
     PStage = XnoStage;
     if (ison)  XnoStage &= ~CMS_noStage;
        else   {XnoStage |=  CMS_noStage; PStage = !PStage;}
     if (doinform && PStage) Manager.Inform(What, (char *)qStage, szReqst);
     XXMutex.UnLock();
}

/******************************************************************************/
/*                                 S t a t s                                  */
/******************************************************************************/
  
int XrdCmsCluster::Stats(char *bfr, int bln)
{
   static const char statfmt1[] = "<stats id=\"cms\"><name>%s</name>";
   static const char statfmt2[] = "<subscriber><name>%s</name>"
          "<status>%s</status><load>%d</load><diskfree>%d</diskfree>"
          "<refa>%d</refa><refr>%d</refr></subscriber>";
   static const char statfmt3[] = "</stats>\n";
   XrdCmsSelected *sp;
   int mlen, tlen = sizeof(statfmt3);
   char stat[6], *stp;

   class spmngr {
         public: XrdCmsSelected *sp;

                 spmngr() {sp = 0;}
                ~spmngr() {XrdCmsSelected *xsp;
                           while((xsp = sp)) {sp = sp->next; delete xsp;}
                          }
                } mngrsp;

// Check if actual length wanted
//
   if (!bfr) return  sizeof(statfmt1) + 256  +
                    (sizeof(statfmt2) + 20*4 + 256) * STMax +
                     sizeof(statfmt3) + 1;

// Get the statistics
//
   mngrsp.sp = sp = List(FULLMASK, LS_All);

// Format the statistics
//
   mlen = snprintf(bfr, bln, statfmt1, Config.myName);
   if ((bln -= mlen) <= 0) return 0;
   tlen += mlen;

   while(sp && bln)
        {stp = stat;
         if (sp->Status)
            {if (sp->Status & XrdCmsSelected::Offline) *stp++ = 'o';
             if (sp->Status & XrdCmsSelected::Suspend) *stp++ = 's';
             if (sp->Status & XrdCmsSelected::NoStage) *stp++ = 'n';
             if (sp->Status & XrdCmsSelected::Disable) *stp++ = 'd';
            } else *stp++ = 'a';
         bfr += mlen;
         mlen = snprintf(bfr, bln, statfmt2, sp->Name, stat,
                sp->Load, sp->Free, sp->RefTotA, sp->RefTotR);
         bln  -= mlen;
         tlen += mlen;
         sp = sp->next;
        }

// See if we overflowed. otherwise finish up
//
   if (sp || bln < (int)sizeof(statfmt1)) return 0;
   bfr += mlen;
   strcpy(bfr, statfmt3);
   return tlen;
}
  
/******************************************************************************/
/*                               S u s p e n d                                */
/******************************************************************************/

void XrdCmsCluster::Suspend(int doinform)
{
     static CmsStatusRequest myState = {{0, kYR_status, 
                                            CmsStatusRequest::kYR_Suspend, 0}};
     static const int szReqst = sizeof(CmsStatusRequest);

     XWait = 1;
     if (doinform) Manager.Inform("suspend", (char *)&myState, szReqst);
}
  
/******************************************************************************/
/*                       P r i v a t e   M e t h o d s                        */
/******************************************************************************/
/******************************************************************************/
/*                             c a l c D e l a y                              */
/******************************************************************************/
  
XrdCmsNode *XrdCmsCluster::calcDelay(int nump, int numd, int numf, int numo,
                                     int nums, int &delay, const char **reason)
{
        if (!nump) {delay = 0;
                    *reason = "no eligible servers for";
                   }
   else if (numf)  {delay = Config.DiskWT;
                    *reason = "no eligible servers have space for";
                   }
   else if (numo)  {delay = Config.MaxDelay;
                    *reason = "eligible servers overloaded for";
                   }
   else if (nums)  {delay = Config.SUSDelay;
                    *reason = "eligible servers suspended for";
                   }
   else if (numd)  {delay = Config.SUPDelay;
                    *reason = "eligible servers offline for";
                   }
   else            {delay = Config.SUPDelay;
                    *reason = "server selection error for";
                   }
   return (XrdCmsNode *)0;
}

/******************************************************************************/
/*                                  D r o p                                   */
/******************************************************************************/
  
// Warning: STMutex must be locked upon entry; the caller must release it.
//          This method may only be called via Remove() either directly or via
//          a defered job scheduled by that method. This method actually
//          deletes the node object.

int XrdCmsCluster::Drop(int sent, int sinst, XrdCmsDrop *djp)
{
   EPNAME("Drop_Node")
   XrdCmsNode *nP;
   char hname[512];

// Make sure this node is the right one
//
   if (!(nP = NodeTab[sent]) || nP->Inst() != sinst)
      {if (djp == nP->DropJob) {nP->DropJob = 0; nP->DropTime = 0;}
       DEBUG("Drop node " <<sent <<'.' <<sinst <<" cancelled.");
       return 0;
      }

// Check if the drop has been rescheduled
//
   if (djp && time(0) < nP->DropTime)
      {Sched->Schedule((XrdJob *)djp, nP->DropTime);
       return 1;
      }

// Save the node name (don't want to hold a lock across a message)
//
   strlcpy(hname, nP->Ident, sizeof(hname));

// Remove node from the manager table
//
   NodeTab[sent] = 0;
   NodeBat[sent] = 0;
   nP->isOffline = 1;
   nP->DropTime  = 0;
   nP->DropJob   = 0;
   nP->isBound   = 0;

// Remove node from the peer list (if it is one)
//
   if (nP->isPeer) {peerHost &= nP->NodeMask; peerMask = ~peerHost;}

// Remove node entry from the alternate list and readjust the end pointer.
//
   if (nP->isMan)
      {memset((void *)&AltMans[sent*AltSize], (int)' ', AltSize);
       if (sent == AltMent)
          {AltMent--;
           while(AltMent >= 0 &&  NodeTab[AltMent]
                              && !NodeTab[AltMent]->isMan) AltMent--;
           if (AltMent < 0) AltMend = AltMans;
              else AltMend = AltMans + ((AltMent+1)*AltSize);
          }
      }

// Readjust STHi
//
   if (sent == STHi) while(STHi >= 0 && !NodeTab[STHi]) STHi--;

// Invalidate any cached entries for this node
//
   if (nP->NodeMask)
      {Cache.Paths.Remove(nP->NodeMask);
       Cache.Reset(nP->NodeID);
      }

// Document the drop
//
   DEBUG("Node " <<hname <<' ' <<sent <<'.' <<sinst <<" dropped.");
   Say.Emsg("Drop_Node", hname, "dropped.");

// Delete the node object
//
   delete nP;
   return 0;
}

/******************************************************************************/
/*                                R e c o r d                                 */
/******************************************************************************/
  
void XrdCmsCluster::Record(char *path, const char *reason)
{
   EPNAME("Record")
   static int msgcnt = 256;
   static XrdSysMutex mcMutex;
   int mcnt;

   DEBUG(reason <<path);
   mcMutex.Lock();
   msgcnt++; mcnt = msgcnt;
   mcMutex.UnLock();

   if (mcnt > 255)
      {Say.Emsg("client defered;", reason, path);
       mcnt = 1;
      }
}
 
/******************************************************************************/
/*                               S e l N o d e                                */
/******************************************************************************/
  
int XrdCmsCluster::SelNode(XrdCmsSelect &Sel, SMask_t pmask, SMask_t amask)
{
    EPNAME("SelNode")
    const char *reason, *reason2;
    int delay = 0, delay2 = 0, nump, isalt = 0, pass = 2;
    int needrw = Sel.Opts & XrdCmsSelect::Write;
    SMask_t mask;
    XrdCmsNode *nP = 0;

// Scan for a primary and alternate node (alternates do staging). At this
// point we omit all peer nodes as they are our last resort.
//
   STMutex.Lock();
   mask = pmask & peerMask;
   while(pass--)
        {if (mask)
            {nP = (Config.sched_RR
                   ? SelbyRef( mask, nump, delay, &reason, isalt||needrw)
                   : SelbyLoad(mask, nump, delay, &reason, isalt||needrw));
             if (nP || (nump && delay) || NodeCnt < Config.SUPCount) break;
            }
         mask = amask & peerMask; isalt = 1;
        }
   STMutex.UnLock();

// Update info
//
   if (nP)
      {strcpy(Sel.Resp.Data, nP->Name(Sel.Resp.DLen, Sel.Resp.Port));
       Sel.Resp.DLen++;
       if (isalt || (Sel.Opts & XrdCmsSelect::NewFile) || Sel.iovN)
          {if (isalt) 
              Cache.AddFile(Sel.Path,nP->NodeMask,Sel.Opts|XrdCmsSelect::Pending);
           if (Sel.iovN && Sel.iovP) nP->Send(Sel.iovP, Sel.iovN);
                  TRACE(Stage, Sel.Resp.Data <<" staging " <<Sel.Path);
          } else {TRACE(Stage, Sel.Resp.Data <<" serving " <<Sel.Path);}
       nP->UnLock();
       return 0;
      } else if (!delay && NodeCnt < Config.SUPCount)
                {reason = "insufficient number of nodes";
                 delay = Config.SUPDelay;
                }

// Return delay if selection failure is recoverable
//
   if (delay && delay < Config.PSDelay)
      {Record(Sel.Path, reason);
       return delay;
      }

// At this point, we attempt a peer node selection (choice of last resort)
//
   if (Sel.Opts & XrdCmsSelect::Peers)
      {STMutex.Lock();
       if ((mask = (pmask | amask) & peerHost))
          nP = SelbyCost(mask, nump, delay2, &reason2, needrw);
       STMutex.UnLock();
       if (nP)
          {strcpy(Sel.Resp.Data, nP->Name(Sel.Resp.DLen, Sel.Resp.Port));
           Sel.Resp.DLen++;
           if (Sel.iovN && Sel.iovP) nP->Send(Sel.iovP, Sel.iovN);
           nP->UnLock();
           TRACE(Stage, "Peer " <<Sel.Resp.Data <<" handling " <<Sel.Path);
           return 0;
          }
       if (!delay) {delay = delay2; reason = reason2;}
      }

// At this point we either don't have enough nodes or simply can't handle this
//
   if (delay)
      {TRACE(Defer, "client defered; " <<reason <<" for " <<Sel.Path);
       return delay;
      }
   return -1;
}

/******************************************************************************/
/*                             S e l b y C o s t                              */
/******************************************************************************/

// Cost selection is used only for peer node selection as peers do not
// report a load and handle their own scheduling.

XrdCmsNode *XrdCmsCluster::SelbyCost(SMask_t mask, int &nump, int &delay,
                                     const char **reason, int needspace)
{
    int i, numd, numf, nums;
    XrdCmsNode *np, *sp = 0;

// Scan for a node (sp points to the selected one)
//
   nump = nums = numf = numd = 0; // possible, suspended, full, and dead
   for (i = 0; i <= STHi; i++)
       if ((np = NodeTab[i]) && (np->NodeMask & mask))
          {nump++;
           if (np->isOffline)                   {numd++; continue;}
           if (np->isSuspend || np->isDisable)  {nums++; continue;}
           if (needspace &&     np->isNoStage)  {numf++; continue;}
           if (!sp) sp = np;
              else if (abs(sp->myCost - np->myCost)
                          <= Config.P_fuzz)
                      {if (needspace)
                          {if (sp->RefA > (np->RefA+Config.DiskLinger))
                               sp=np;
                           } 
                           else if (sp->RefR > np->RefR) sp=np;
                       }
                       else if (sp->myCost > np->myCost) sp=np;
          }

// Check for overloaded node and return result
//
   if (!sp) return calcDelay(nump, numd, numf, 0, nums, delay, reason);
   sp->Lock();
   if (needspace) {SelAcnt++; sp->RefA++;}  // Protected by STMutex
      else        {SelRcnt++; sp->RefR++;}
   delay = 0;
   return sp;
}
  
/******************************************************************************/
/*                             S e l b y L o a d                              */
/******************************************************************************/
  
XrdCmsNode *XrdCmsCluster::SelbyLoad(SMask_t mask, int &nump, int &delay,
                                     const char **reason, int needspace)
{
    int i, numd, numf, numo, nums;
    XrdCmsNode *np, *sp = 0;

// Scan for a node (preset possible, suspended, overloaded, full, and dead)
//
   nump = nums = numo = numf = numd = 0; 
   for (i = 0; i <= STHi; i++)
       if ((np = NodeTab[i]) && (np->NodeMask & mask))
          {nump++;
           if (np->isOffline)                     {numd++; continue;}
           if (np->isSuspend || np->isDisable)    {nums++; continue;}
           if (np->myLoad > Config.MaxLoad) {numo++; continue;}
           if (needspace && (   np->isNoStage
                             || np->DiskFree < Config.DiskMin))
              {numf++; continue;}
           if (!sp) sp = np;
              else if (abs(sp->myLoad - np->myLoad)
                          <= Config.P_fuzz)
                      {if (needspace)
                          {if (sp->RefA > (np->RefA+Config.DiskLinger))
                               sp=np;
                           } 
                           else if (sp->RefR > np->RefR) sp=np;
                       }
                       else if (sp->myLoad > np->myLoad) sp=np;
          }

// Check for overloaded node and return result
//
   if (!sp) return calcDelay(nump, numd, numf, numo, nums, delay, reason);
   sp->Lock();
   if (needspace) {SelAcnt++; sp->RefA++;}  // Protected by STMutex
      else        {SelRcnt++; sp->RefR++;}
   delay = 0;
   return sp;
}
/******************************************************************************/
/*                              S e l b y R e f                               */
/******************************************************************************/

XrdCmsNode *XrdCmsCluster::SelbyRef(SMask_t mask, int &nump, int &delay,
                                    const char **reason, int needspace)
{
    int i, numd, numf, nums;
    XrdCmsNode *np, *sp = 0;

// Scan for a node (sp points to the selected one)
//
   nump = nums = numf = numd = 0; // possible, suspended, full, and dead
   for (i = 0; i <= STHi; i++)
       if ((np = NodeTab[i]) && (np->NodeMask & mask))
          {nump++;
           if (np->isOffline)                   {numd++; continue;}
           if (np->isSuspend || np->isDisable)  {nums++; continue;}
           if (needspace && (   np->isNoStage
                             || np->DiskFree < Config.DiskMin))
              {numf++; continue;}
           if (!sp) sp = np;
              else if (needspace)
                      {if (sp->RefA > (np->RefA+Config.DiskLinger)) sp=np;}
                      else if (sp->RefR > np->RefR) sp=np;
          }

// Check for overloaded node and return result
//
   if (!sp) return calcDelay(nump, numd, numf, 0, nums, delay, reason);
   sp->Lock();
   if (needspace) {SelAcnt++; sp->RefA++;}  // Protected by STMutex
      else        {SelRcnt++; sp->RefR++;}
   delay = 0;
   return sp;
}
 
/******************************************************************************/
/*                             s e n d A L i s t                              */
/******************************************************************************/
  
// Single entry at a time, protected by STMutex!

void XrdCmsCluster::sendAList(XrdLink *lp)
{
   static CmsTryRequest Req = {{0, kYR_try, 0, 0}, 0};
   static char *AltNext = AltMans;
   static struct iovec iov[4] = {{(caddr_t)&Req, sizeof(Req)},
                                 {0, 0},
                                 {AltMans, 0},
                                 {(caddr_t)"\0", 1}};
   int dlen;

// Calculate what to send
//
   AltNext = AltNext + AltSize;
   if (AltNext >= AltMend)
      {AltNext = AltMans;
       iov[1].iov_len = 0;
       iov[2].iov_len = dlen = AltMend - AltMans;
      } else {
        iov[1].iov_base = (caddr_t)AltNext;
        iov[1].iov_len  = AltMend - AltNext;
        iov[2].iov_len  = AltNext - AltMans;
        dlen = iov[1].iov_len + iov[2].iov_len;
      }

// Complete the request
//
   Req.Hdr.datalen = htons(static_cast<unsigned short>(dlen+sizeof(Req.sLen)));
   Req.sLen = htons(static_cast<unsigned short>(dlen));

// Send the list of alternates (rotated once)
//
   lp->Send(iov, 4, dlen+sizeof(Req));
}

/******************************************************************************/
/*                             s e t A l t M a n                              */
/******************************************************************************/
  
// Single entry at a time, protected by STMutex!
  
void XrdCmsCluster::setAltMan(int snum, unsigned int ipaddr, int port)
{
   char *ap = &AltMans[snum*AltSize];
   int i;

// Preset the buffer and pre-screen the port number
//
   if (!port || (port > 0x0000ffff)) port = Config.PortTCP;
   memset(ap, int(' '), AltSize);

// Insert the ip address of this node into the list of nodes
//
   i = XrdNetDNS::IP2String(ipaddr, port, ap, AltSize);
   ap[i] = ' ';

// Compute new fence
//
   if (ap >= AltMend) {AltMend = ap + AltSize; AltMent = snum;}
}
