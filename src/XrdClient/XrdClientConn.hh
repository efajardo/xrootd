//////////////////////////////////////////////////////////////////////////
//                                                                      //
// XrdClientConn                                                        //
//                                                                      //
// Author: Fabrizio Furano (INFN Padova, 2004)                          //
// Adapted from TXNetFile (root.cern.ch) originally done by             //
//  Alvise Dorigo, Fabrizio Furano                                      //
//          INFN Padova, 2003                                           //
//                                                                      //
// High level handler of connections to xrootd.                         //
//                                                                      //
//////////////////////////////////////////////////////////////////////////

//       $Id$

#ifndef XRD_CONN_H
#define XRD_CONN_H


#include "XrdClient/XrdClientConst.hh"

#include "time.h"
#include "XrdClient/XrdClientConnMgr.hh"
#include "XrdClient/XrdClientMessage.hh"
#include "XrdClient/XrdClientUrlInfo.hh"
#include "XrdClient/XrdClientReadCache.hh"
#include "XrdOuc/XrdOucHash.hh"

#define ConnectionManager XrdClientConn::GetConnectionMgr()

class XrdClientAbs;
class XrdSecProtocol;

class XrdClientConn {

public:

    enum ESrvErrorHandlerRetval {
	kSEHRReturnMsgToCaller   = 0,
	kSEHRBreakLoop           = 1,
	kSEHRContinue            = 2,
	kSEHRReturnNoMsgToCaller = 3,
	kSEHRRedirLimitReached   = 4
    };
    enum EThreeStateReadHandler {
	kTSRHReturnMex     = 0,
	kTSRHReturnNullMex = 1,
	kTSRHContinue      = 2
    };

    // To keep info about an open session
    struct                     SessionIDInfo {
	char id[16];
    };

    int                        fLastDataBytesRecv;
    int                        fLastDataBytesSent;
    XErrorCode                 fOpenError;	

    XrdOucString               fRedirOpaque;        // Opaque info returned by the server when

    // redirecting. To be used in the next opens
    XrdClientConn();
    virtual ~XrdClientConn();

    inline bool                CacheWillFit(long long bytes) {
	if (!fMainReadCache)
	    return FALSE;
	return fMainReadCache->WillFit(bytes);
    }

    bool                       CheckHostDomain(XrdOucString hostToCheck);
    short                      Connect(XrdClientUrlInfo Host2Conn,
				       XrdClientAbsUnsolMsgHandler *unsolhandler);
    void                       Disconnect(bool ForcePhysicalDisc);
    virtual bool               GetAccessToSrv();
    XReqErrorType              GoBackToRedirector();

    XrdOucString               GetClientHostDomain() { return fgClientHostDomain; }


    static XrdClientPhyConnection     *GetPhyConn(int LogConnID);

    long                       GetDataFromCache(const void *buffer,
						long long begin_offs,
						long long end_offs,
						bool PerfCalc,
						XrdClientIntvList &missingblks,
						long &outstandingblks );

    bool                       SubmitDataToCache(XrdClientMessage *xmsg,
						 long long begin_offs,
						 long long end_offs);

    bool                       SubmitRawDataToCache(const void *buffer,
						 long long begin_offs,
						 long long end_offs);

    void                       SubmitPlaceholderToCache(long long begin_offs,
							long long end_offs) {
	if (fMainReadCache)
	    fMainReadCache->PutPlaceholder(begin_offs, end_offs);
    }

  
    void                       RemoveAllDataFromCache() {
        if (fMainReadCache)
            fMainReadCache->RemoveItems();
    }

    void                       RemoveDataFromCache(long long begin_offs,
                                                   long long end_offs) {
        if (fMainReadCache)
            fMainReadCache->RemoveItems(begin_offs, end_offs);
    }

    void                       RemovePlaceholdersFromCache() {
        if (fMainReadCache)
            fMainReadCache->RemovePlaceholders();
    }

    void                       PrintCache() {
        if (fMainReadCache)
            fMainReadCache->PrintCache();
    }


    int                        GetLogConnID() const { return fLogConnID; }

    ERemoteServerType          GetServerType() const { return fServerType; }

    kXR_unt16                  GetStreamID() const { return fPrimaryStreamid; }

    inline XrdClientUrlInfo    *GetLBSUrl() { return fLBSUrl; }
    inline XrdClientUrlInfo    GetCurrentUrl() { return fUrl; }
    inline XrdClientUrlInfo    GetRedirUrl() { return fREQUrl; }

    XErrorCode                 GetOpenError() const { return fOpenError; }
    virtual XReqErrorType      GoToAnotherServer(XrdClientUrlInfo newdest);
    bool                       IsConnected() const { return fConnected; }
    bool                       IsPhyConnConnected();

    struct ServerResponseHeader
    LastServerResp;

    struct ServerResponseBody_Error
    LastServerError;

    UnsolRespProcResult        ProcessAsynResp(XrdClientMessage *unsolmsg);

    virtual bool               SendGenCommand(ClientRequest *req, 
					      const void *reqMoreData,       
					      void **answMoreDataAllocated,
					      void *answMoreData, bool HasToAlloc,
					      char *CmdName, int substreamid = 0);

    int                        GetOpenSockFD() const { return fOpenSockFD; }

    void                       SetClientHostDomain(const char *src) { fgClientHostDomain = src; }
    void                       SetConnected(bool conn) { fConnected = conn; }

    void                       SetOpenError(XErrorCode err) { fOpenError = err; }

    // Gets a parallel stream id to use to set the return path for a re
    int                        GetParallelStreamToUse(int reqsperstream);
    int                        GetParallelStreamCount();     // Returns the total number of connected streams

    void                       SetRedirHandler(XrdClientAbs *rh) { fRedirHandler = rh; }

    void                       SetRequestedDestHost(char *newh, kXR_int32 port) {
	fREQUrl = fUrl;
	fREQUrl.Host = newh;
	fREQUrl.Port = port;
	fREQUrl.SetAddrFromHost();
    }

    // Puts this instance in pause state for wsec seconds.
    // A value <= 0 revokes immediately the pause state
    void                       SetREQPauseState(kXR_int32 wsec) {
	// Lock mutex
	fREQWait->Lock();

	if (wsec > 0)
	    fREQWaitTimeLimit = time(0) + wsec;
	else {
	    fREQWaitTimeLimit = 0;
	    fREQWait->Broadcast();
	}

	// UnLock mutex
	fREQWait->UnLock();
    }

    // Puts this instance in connect-pause state for wsec seconds.
    // Any future connection attempt will not happen before wsec
    //  and the first one will be towards the given host
    void                       SetREQDelayedConnectState(kXR_int32 wsec) {
	// Lock mutex
	fREQConnectWait->Lock();

	if (wsec > 0)
	    fREQConnectWaitTimeLimit = time(0) + wsec;
	else {
	    fREQConnectWaitTimeLimit = 0;
	    fREQConnectWait->Broadcast();
	}

	// UnLock mutex
	fREQConnectWait->UnLock();
    }

    void                       SetSID(kXR_char *sid);
    inline void                SetUrl(XrdClientUrlInfo thisUrl) { fUrl = thisUrl; }

    // Sends the request to the server, through logconn with ID LogConnID
    // The request is sent with a streamid 'child' of the current one, then marked as pending
    // Its answer will be caught asynchronously
    XReqErrorType              WriteToServer_Async(ClientRequest *req, 
						   const void* reqMoreData,
						   int substreamid = 0);

    static XrdClientConnectionMgr *GetConnectionMgr()
    { return fgConnectionMgr;} //Instance of the conn manager

    void GetSessionID(SessionIDInfo &sess) {
      XrdOucString sessname;
      char buf[20];
      
      snprintf(buf, 20, "%d", fUrl.Port);

      sessname = fUrl.HostAddr;
      if (sessname.length() <= 0)
	sessname = fUrl.Host;

      sessname += ":";
      sessname += buf;

      sess = *( fSessionIDRepo.Find(sessname.c_str()) );
    }

    long                       GetServerProtocol() { return fServerProto; }

    short                      GetMaxRedirCnt() const { return fMaxGlobalRedirCnt; }
    void                       SetMaxRedirCnt(short mx) {fMaxGlobalRedirCnt = mx; }

    static XrdOucString        GetKey(XrdClientUrlInfo uu);

protected:
    void                       SetLogConnID(int cid) { fLogConnID = cid; }
    void                       SetStreamID(kXR_unt16 sid) { fPrimaryStreamid = sid; }

    // The handler which first tried to connect somewhere
    XrdClientAbsUnsolMsgHandler *fUnsolMsgHandler;

    XrdClientUrlInfo           fUrl;                // The current URL
    XrdClientUrlInfo           *fLBSUrl;            // Needed to save the load balancer url
    XrdClientUrlInfo           fREQUrl;             // For explicitly requested redirs

    short                      fGlobalRedirCnt;    // Number of redirections

private:

    static XrdOucString        fgClientHostDomain; // Save the client's domain name

    bool                       fGettingAccessToSrv; // To avoid recursion in desperate situations

    bool                       fConnected;
    time_t                     fGlobalRedirLastUpdateTimestamp; // Timestamp of last redirection

    int                        fLogConnID;        // Logical connection ID used
    kXR_unt16                  fPrimaryStreamid;  // Streamid used for normal communication
    // NB it's a copy of the one contained in
    // the logconn

    short                      fMaxGlobalRedirCnt;
    XrdClientReadCache         *fMainReadCache;

    XrdClientAbs               *fRedirHandler;      // Pointer to a class inheriting from
    // XrdClientAbs providing methods
    // to handle a redir at higher level

    XrdOucString               fRedirInternalToken; // Token returned by the server when
    // redirecting. To be used in the next logins

    XrdSysCondVar              *fREQWaitResp;           // For explicitly requested delayed async responses
    ServerResponseBody_Attn_asynresp *
                               fREQWaitRespData;        // For explicitly requested delayed async responses

    time_t                     fREQWaitTimeLimit;   // For explicitly requested pause state
    XrdSysCondVar              *fREQWait;           // For explicitly requested pause state
    time_t                     fREQConnectWaitTimeLimit;   // For explicitly requested delayed reconnect
    XrdSysCondVar              *fREQConnectWait;           // For explicitly requested delayed reconnect

    long                       fServerProto;        // The server protocol
    ERemoteServerType          fServerType;         // Server type as returned by doHandShake() 

    static XrdOucHash<SessionIDInfo>
    fSessionIDRepo;      // The repository of session IDs, shared.
    // Association between
    // <hostname>:<port> and a SessionIDInfo struct

    int                        fOpenSockFD;         // Descriptor of the underlying socket
    static XrdClientConnectionMgr *fgConnectionMgr; //Instance of the Connection Manager

    bool                       CheckErrorStatus(XrdClientMessage *, short &, char *);
    void                       CheckPort(int &port);
    void                       CheckREQPauseState();
    void                       CheckREQConnectWaitState();
    bool                       CheckResp(struct ServerResponseHeader *resp, const char *method);
    XrdClientMessage           *ClientServerCmd(ClientRequest *req,
						const void *reqMoreData,
						void **answMoreDataAllocated,
						void *answMoreData,
						bool HasToAlloc,
						int substreamid = 0);
    XrdSecProtocol            *DoAuthentication(char *plist, int plsiz);

    ERemoteServerType          DoHandShake(short log);

    bool                       DoLogin();
    bool                       DomainMatcher(XrdOucString dom, XrdOucString domlist);

    XrdOucString               GetDomainToMatch(XrdOucString hostname);

    ESrvErrorHandlerRetval     HandleServerError(XReqErrorType &, XrdClientMessage *,
						 ClientRequest *);
    bool                       MatchStreamid(struct ServerResponseHeader *ServerResponse);

    // Sends a close request, without waiting for an answer
    // useful (?) to be sent just before closing a badly working stream
    bool                       PanicClose();

    XrdOucString               ParseDomainFromHostname(XrdOucString hostname);

    XrdClientMessage           *ReadPartialAnswer(XReqErrorType &, size_t &, 
						  ClientRequest *, bool, void**,
						  EThreeStateReadHandler &);

    void                       ClearSessionID();

    XReqErrorType              WriteToServer(ClientRequest *req, 
					     const void* reqMoreData,
					     short LogConnID,
					     int substreamid = 0);

    bool                       WaitResp(int secsmax);
};



#endif
