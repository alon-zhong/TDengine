/*
 * Copyright (c) 2019 TAOS Data, Inc. <jhtao@taosdata.com>
 * * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the GNU Affero General Public License, version 3
 * or later ("AGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifdef USE_UV

#include "transComm.h"

static TdThreadOnce transModuleInit = PTHREAD_ONCE_INIT;

static char* notify = "a";

typedef struct {
  int       notifyCount;  //
  int       init;         // init or not
  STransMsg msg;
} SSvrRegArg;

typedef struct SSvrConn {
  T_REF_DECLARE()
  uv_tcp_t*  pTcp;
  queue      wreqQueue;
  uv_timer_t pTimer;

  queue       queue;
  SConnBuffer readBuf;  // read buf,
  int         inType;
  void*       pTransInst;  // rpc init
  void*       ahandle;     //
  void*       hostThrd;
  STransQueue srvMsgs;

  SSvrRegArg regArg;
  bool       broken;  // conn broken;

  ConnStatus status;

  uint32_t clientIp;
  uint16_t port;

  char src[32];
  char dst[32];

  int64_t refId;
  int     spi;
  char    info[64];
  char    user[TSDB_UNI_LEN];  // user ID for the link
  char    secret[TSDB_PASSWORD_LEN];
  char    ckey[TSDB_PASSWORD_LEN];  // ciphering key
} SSvrConn;

typedef struct SSvrMsg {
  SSvrConn*     pConn;
  STransMsg     msg;
  queue         q;
  STransMsgType type;
} SSvrMsg;

typedef struct SWorkThrd {
  TdThread      thread;
  uv_connect_t  connect_req;
  uv_pipe_t*    pipe;
  uv_os_fd_t    fd;
  uv_loop_t*    loop;
  SAsyncPool*   asyncPool;
  uv_prepare_t* prepare;
  queue         msg;

  queue conn;
  void* pTransInst;
  bool  quit;
} SWorkThrd;

typedef struct SServerObj {
  TdThread   thread;
  uv_tcp_t   server;
  uv_loop_t* loop;

  // work thread info
  int         workerIdx;
  int         numOfThreads;
  int         numOfWorkerReady;
  SWorkThrd** pThreadObj;

  uv_pipe_t   pipeListen;
  uv_pipe_t** pipe;
  uint32_t    ip;
  uint32_t    port;
  uv_async_t* pAcceptAsync;  // just to quit from from accept thread

  bool inited;
} SServerObj;

static void uvAllocConnBufferCb(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf);
static void uvAllocRecvBufferCb(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf);
static void uvOnRecvCb(uv_stream_t* cli, ssize_t nread, const uv_buf_t* buf);
static void uvOnTimeoutCb(uv_timer_t* handle);
static void uvOnSendCb(uv_write_t* req, int status);
static void uvOnPipeWriteCb(uv_write_t* req, int status);
static void uvOnAcceptCb(uv_stream_t* stream, int status);
static void uvOnConnectionCb(uv_stream_t* q, ssize_t nread, const uv_buf_t* buf);
static void uvWorkerAsyncCb(uv_async_t* handle);
static void uvAcceptAsyncCb(uv_async_t* handle);
static void uvShutDownCb(uv_shutdown_t* req, int status);
static void uvPrepareCb(uv_prepare_t* handle);

/*
 * time-consuming task throwed into BG work thread
 */
static void uvWorkDoTask(uv_work_t* req);
static void uvWorkAfterTask(uv_work_t* req, int status);

static void uvWalkCb(uv_handle_t* handle, void* arg);
static void uvFreeCb(uv_handle_t* handle);

static void uvStartSendRespInternal(SSvrMsg* smsg);
static void uvPrepareSendData(SSvrMsg* msg, uv_buf_t* wb);
static void uvStartSendResp(SSvrMsg* msg);

static void uvNotifyLinkBrokenToApp(SSvrConn* conn);

static void destroySmsg(SSvrMsg* smsg);
// check whether already read complete packet
static SSvrConn* createConn(void* hThrd);
static void      destroyConn(SSvrConn* conn, bool clear /*clear handle or not*/);
static void      destroyConnRegArg(SSvrConn* conn);

static int reallocConnRef(SSvrConn* conn);

static void uvHandleQuit(SSvrMsg* msg, SWorkThrd* thrd);
static void uvHandleRelease(SSvrMsg* msg, SWorkThrd* thrd);
static void uvHandleResp(SSvrMsg* msg, SWorkThrd* thrd);
static void uvHandleRegister(SSvrMsg* msg, SWorkThrd* thrd);
static void (*transAsyncHandle[])(SSvrMsg* msg, SWorkThrd* thrd) = {uvHandleResp, uvHandleQuit, uvHandleRelease,
                                                                    uvHandleRegister, NULL};

static void uvDestroyConn(uv_handle_t* handle);

// server and worker thread
static void* transWorkerThread(void* arg);
static void* transAcceptThread(void* arg);

// add handle loop
static bool addHandleToWorkloop(SWorkThrd* pThrd, char* pipeName);
static bool addHandleToAcceptloop(void* arg);

#define CONN_SHOULD_RELEASE(conn, head)                                                                   \
  do {                                                                                                    \
    if ((head)->release == 1 && (head->msgLen) == sizeof(*head)) {                                        \
      reallocConnRef(conn);                                                                               \
      tTrace("conn %p received release request", conn);                                                   \
                                                                                                          \
      STraceId traceId = head->traceId;                                                                   \
      conn->status = ConnRelease;                                                                         \
      transClearBuffer(&conn->readBuf);                                                                   \
      transFreeMsg(transContFromHead((char*)head));                                                       \
                                                                                                          \
      STransMsg tmsg = {                                                                                  \
          .code = 0, .info.handle = (void*)conn, .info.traceId = traceId, .info.ahandle = (void*)0x9527}; \
      SSvrMsg* srvMsg = taosMemoryCalloc(1, sizeof(SSvrMsg));                                             \
      srvMsg->msg = tmsg;                                                                                 \
      srvMsg->type = Release;                                                                             \
      srvMsg->pConn = conn;                                                                               \
      if (!transQueuePush(&conn->srvMsgs, srvMsg)) {                                                      \
        return;                                                                                           \
      }                                                                                                   \
      if (conn->regArg.init) {                                                                            \
        tTrace("conn %p release, notify server app", conn);                                               \
        STrans* pTransInst = conn->pTransInst;                                                            \
        (*pTransInst->cfp)(pTransInst->parent, &(conn->regArg.msg), NULL);                                \
        memset(&conn->regArg, 0, sizeof(conn->regArg));                                                   \
      }                                                                                                   \
      uvStartSendRespInternal(srvMsg);                                                                    \
      return;                                                                                             \
    }                                                                                                     \
  } while (0)

#define SRV_RELEASE_UV(loop)       \
  do {                             \
    uv_walk(loop, uvWalkCb, NULL); \
    uv_run(loop, UV_RUN_DEFAULT);  \
    uv_loop_close(loop);           \
  } while (0);

#define ASYNC_ERR_JRET(thrd)                            \
  do {                                                  \
    if (thrd->quit) {                                   \
      tTrace("worker thread already quit, ignore msg"); \
      goto _return1;                                    \
    }                                                   \
  } while (0)

void uvAllocRecvBufferCb(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf) {
  SSvrConn*    conn = handle->data;
  SConnBuffer* pBuf = &conn->readBuf;
  transAllocBuffer(pBuf, buf);
}

// refers specifically to query or insert timeout
static void uvHandleActivityTimeout(uv_timer_t* handle) {
  SSvrConn* conn = handle->data;
  tDebug("%p timeout since no activity", conn);
}

static void uvHandleReq(SSvrConn* pConn) {
  STransMsgHead* msg = NULL;
  int            msgLen = 0;

  msgLen = transDumpFromBuffer(&pConn->readBuf, (char**)&msg);

  STransMsgHead* pHead = (STransMsgHead*)msg;
  pHead->code = htonl(pHead->code);
  pHead->msgLen = htonl(pHead->msgLen);
  memcpy(pConn->user, pHead->user, strlen(pHead->user));

  // TODO(dengyihao): time-consuming task throwed into BG Thread
  //  uv_work_t* wreq = taosMemoryMalloc(sizeof(uv_work_t));
  //  wreq->data = pConn;
  //  uv_read_stop((uv_stream_t*)pConn->pTcp);
  //  transRefSrvHandle(pConn);
  //  uv_queue_work(((SWorkThrd*)pConn->hostThrd)->loop, wreq, uvWorkDoTask, uvWorkAfterTask);

  CONN_SHOULD_RELEASE(pConn, pHead);

  STransMsg transMsg;
  memset(&transMsg, 0, sizeof(transMsg));
  transMsg.contLen = transContLenFromMsg(pHead->msgLen);
  transMsg.pCont = pHead->content;
  transMsg.msgType = pHead->msgType;
  transMsg.code = pHead->code;

  pConn->inType = pHead->msgType;
  if (pConn->status == ConnNormal) {
    if (pHead->persist == 1) {
      pConn->status = ConnAcquire;
      transRefSrvHandle(pConn);
      tDebug("conn %p acquired by server app", pConn);
    }
  }
  STrans*   pTransInst = pConn->pTransInst;
  STraceId* trace = &pHead->traceId;
  if (pConn->status == ConnNormal && pHead->noResp == 0) {
    transRefSrvHandle(pConn);

    tGDebug("%s conn %p %s received from %s, local info:%s, len:%d", transLabel(pTransInst), pConn,
            TMSG_INFO(transMsg.msgType), pConn->dst, pConn->src, transMsg.contLen);
  } else {
    tGDebug("%s conn %p %s received from %s, local info:%s, len:%d, resp:%d, code:%d", transLabel(pTransInst), pConn,
            TMSG_INFO(transMsg.msgType), pConn->dst, pConn->src, transMsg.contLen, pHead->noResp, transMsg.code);
  }

  // pHead->noResp = 1,
  // 1. server application should not send resp on handle
  // 2. once send out data, cli conn released to conn pool immediately
  // 3. not mixed with persist
  transMsg.info.ahandle = (void*)pHead->ahandle;
  transMsg.info.handle = (void*)transAcquireExHandle(transGetRefMgt(), pConn->refId);
  transMsg.info.refId = pConn->refId;
  transMsg.info.traceId = pHead->traceId;

  tGTrace("%s handle %p conn:%p translated to app, refId:%" PRIu64, transLabel(pTransInst), transMsg.info.handle, pConn,
          pConn->refId);
  assert(transMsg.info.handle != NULL);

  if (pHead->noResp == 1) {
    transMsg.info.refId = -1;
  }

  // set up conn info
  SRpcConnInfo* pConnInfo = &(transMsg.info.conn);
  pConnInfo->clientIp = pConn->clientIp;
  pConnInfo->clientPort = pConn->port;
  tstrncpy(pConnInfo->user, pConn->user, sizeof(pConnInfo->user));

  transReleaseExHandle(transGetRefMgt(), pConn->refId);

  (*pTransInst->cfp)(pTransInst->parent, &transMsg, NULL);
}

void uvOnRecvCb(uv_stream_t* cli, ssize_t nread, const uv_buf_t* buf) {
  // opt
  SSvrConn*    conn = cli->data;
  SConnBuffer* pBuf = &conn->readBuf;
  STrans*      pTransInst = conn->pTransInst;
  if (nread > 0) {
    pBuf->len += nread;
    tTrace("%s conn %p total read:%d, current read:%d", transLabel(pTransInst), conn, pBuf->len, (int)nread);
    while (transReadComplete(pBuf)) {
      tTrace("%s conn %p alread read complete packet", transLabel(pTransInst), conn);
      uvHandleReq(conn);
    }
    return;
  }
  if (nread == 0) {
    return;
  }

  tWarn("%s conn %p read error:%s", transLabel(pTransInst), conn, uv_err_name(nread));
  if (nread < 0) {
    conn->broken = true;
    if (conn->status == ConnAcquire) {
      if (conn->regArg.init) {
        tTrace("%s conn %p broken, notify server app", transLabel(pTransInst), conn);
        STrans* pTransInst = conn->pTransInst;
        (*pTransInst->cfp)(pTransInst->parent, &(conn->regArg.msg), NULL);
        memset(&conn->regArg, 0, sizeof(conn->regArg));
      }
    }
    destroyConn(conn, true);
  }
}
void uvAllocConnBufferCb(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf) {
  buf->len = 2;
  buf->base = taosMemoryCalloc(1, sizeof(char) * buf->len);
}

void uvOnTimeoutCb(uv_timer_t* handle) {
  // opt
  SSvrConn* pConn = handle->data;
  tError("conn %p time out", pConn);
}

void uvOnSendCb(uv_write_t* req, int status) {
  SSvrConn* conn = transReqQueueRemove(req);
  if (conn == NULL) return;

  if (status == 0) {
    tTrace("conn %p data already was written on stream", conn);
    if (!transQueueEmpty(&conn->srvMsgs)) {
      SSvrMsg* msg = transQueuePop(&conn->srvMsgs);
      destroySmsg(msg);
      // send cached data
      if (!transQueueEmpty(&conn->srvMsgs)) {
        msg = (SSvrMsg*)transQueueGet(&conn->srvMsgs, 0);
        if (msg->type == Register && conn->status == ConnAcquire) {
          conn->regArg.notifyCount = 0;
          conn->regArg.init = 1;
          conn->regArg.msg = msg->msg;
          if (conn->broken) {
            STrans* pTransInst = conn->pTransInst;
            (pTransInst->cfp)(pTransInst->parent, &(conn->regArg.msg), NULL);
            memset(&conn->regArg, 0, sizeof(conn->regArg));
          }
          transQueuePop(&conn->srvMsgs);
          taosMemoryFree(msg);

          msg = (SSvrMsg*)transQueueGet(&conn->srvMsgs, 0);
          if (msg != NULL) {
            uvStartSendRespInternal(msg);
          }
        } else {
          uvStartSendRespInternal(msg);
        }
      }
    }
    transUnrefSrvHandle(conn);
  } else {
    tError("conn %p failed to write data, %s", conn, uv_err_name(status));
    conn->broken = true;
    transUnrefSrvHandle(conn);
  }
}
static void uvOnPipeWriteCb(uv_write_t* req, int status) {
  if (status == 0) {
    tTrace("success to dispatch conn to work thread");
  } else {
    tError("fail to dispatch conn to work thread");
  }
  uv_close((uv_handle_t*)req->data, uvFreeCb);
  taosMemoryFree(req);
}

static void uvPrepareSendData(SSvrMsg* smsg, uv_buf_t* wb) {
  SSvrConn*  pConn = smsg->pConn;
  STransMsg* pMsg = &smsg->msg;
  if (pMsg->pCont == 0) {
    pMsg->pCont = (void*)rpcMallocCont(0);
    pMsg->contLen = 0;
  }
  STransMsgHead* pHead = transHeadFromCont(pMsg->pCont);
  pHead->ahandle = (uint64_t)pMsg->info.ahandle;
  pHead->traceId = pMsg->info.traceId;
  pHead->hasEpSet = pMsg->info.hasEpSet;

  if (pConn->status == ConnNormal) {
    pHead->msgType = (0 == pMsg->msgType ? pConn->inType + 1 : pMsg->msgType);
    if (smsg->type == Release) pHead->msgType = 0;
  } else {
    if (smsg->type == Release) {
      pHead->msgType = 0;
      pConn->status = ConnNormal;
      destroyConnRegArg(pConn);
      transUnrefSrvHandle(pConn);
    } else {
      // set up resp msg type
      pHead->msgType = (0 == pMsg->msgType ? pConn->inType + 1 : pMsg->msgType);
    }
  }

  pHead->release = smsg->type == Release ? 1 : 0;
  pHead->code = htonl(pMsg->code);

  char*   msg = (char*)pHead;
  int32_t len = transMsgLenFromCont(pMsg->contLen);

  STrans*   pTransInst = pConn->pTransInst;
  STraceId* trace = &pMsg->info.traceId;
  tGDebug("%s conn %p %s is sent to %s, local info:%s, len:%d", transLabel(pTransInst), pConn,
          TMSG_INFO(pHead->msgType), pConn->dst, pConn->src, pMsg->contLen);
  pHead->msgLen = htonl(len);

  wb->base = msg;
  wb->len = len;
}

static void uvStartSendRespInternal(SSvrMsg* smsg) {
  SSvrConn* pConn = smsg->pConn;
  if (pConn->broken) {
    return;
  }

  uv_buf_t wb;
  uvPrepareSendData(smsg, &wb);

  transRefSrvHandle(pConn);
  uv_write_t* req = transReqQueuePush(&pConn->wreqQueue);
  uv_write(req, (uv_stream_t*)pConn->pTcp, &wb, 1, uvOnSendCb);
}
static void uvStartSendResp(SSvrMsg* smsg) {
  // impl
  SSvrConn* pConn = smsg->pConn;
  if (pConn->broken == true) {
    // persist by
    transFreeMsg(smsg->msg.pCont);
    taosMemoryFree(smsg);
    transUnrefSrvHandle(pConn);
    return;
  }
  if (pConn->status == ConnNormal) {
    transUnrefSrvHandle(pConn);
  }

  if (!transQueuePush(&pConn->srvMsgs, smsg)) {
    return;
  }
  uvStartSendRespInternal(smsg);
  return;
}

static void destroySmsg(SSvrMsg* smsg) {
  if (smsg == NULL) {
    return;
  }
  transFreeMsg(smsg->msg.pCont);
  taosMemoryFree(smsg);
}
static void destroyAllConn(SWorkThrd* pThrd) {
  tTrace("thread %p destroy all conn ", pThrd);
  while (!QUEUE_IS_EMPTY(&pThrd->conn)) {
    queue* h = QUEUE_HEAD(&pThrd->conn);
    QUEUE_REMOVE(h);
    QUEUE_INIT(h);

    SSvrConn* c = QUEUE_DATA(h, SSvrConn, queue);
    while (T_REF_VAL_GET(c) >= 2) {
      transUnrefSrvHandle(c);
    }
    transUnrefSrvHandle(c);
  }
}
void uvWorkerAsyncCb(uv_async_t* handle) {
  SAsyncItem* item = handle->data;
  SWorkThrd*  pThrd = item->pThrd;
  SSvrConn*   conn = NULL;
  queue       wq;

  // batch process to avoid to lock/unlock frequently
  taosThreadMutexLock(&item->mtx);
  QUEUE_MOVE(&item->qmsg, &wq);
  taosThreadMutexUnlock(&item->mtx);

  while (!QUEUE_IS_EMPTY(&wq)) {
    queue* head = QUEUE_HEAD(&wq);
    QUEUE_REMOVE(head);

    SSvrMsg* msg = QUEUE_DATA(head, SSvrMsg, q);
    if (msg == NULL) {
      tError("unexcept occurred, continue");
      continue;
    }

    // release handle to rpc init
    if (msg->type == Quit) {
      (*transAsyncHandle[msg->type])(msg, pThrd);
      continue;
    } else {
      STransMsg transMsg = msg->msg;

      SExHandle* exh1 = transMsg.info.handle;
      int64_t    refId = transMsg.info.refId;
      SExHandle* exh2 = transAcquireExHandle(transGetRefMgt(), refId);
      if (exh2 == NULL || exh1 != exh2) {
        tTrace("handle except msg %p, ignore it", exh1);
        transReleaseExHandle(transGetRefMgt(), refId);
        destroySmsg(msg);
        continue;
      }
      msg->pConn = exh1->handle;
      transReleaseExHandle(transGetRefMgt(), refId);
      (*transAsyncHandle[msg->type])(msg, pThrd);
    }
  }
}
static void uvWalkCb(uv_handle_t* handle, void* arg) {
  if (!uv_is_closing(handle)) {
    uv_close(handle, NULL);
  }
}
static void uvFreeCb(uv_handle_t* handle) {
  //
  taosMemoryFree(handle);
}

static void uvAcceptAsyncCb(uv_async_t* async) {
  SServerObj* srv = async->data;
  tDebug("close server port %d", srv->port);
  uv_walk(srv->loop, uvWalkCb, NULL);
}

static void uvShutDownCb(uv_shutdown_t* req, int status) {
  if (status != 0) {
    tDebug("conn failed to shut down:%s", uv_err_name(status));
  }
  uv_close((uv_handle_t*)req->handle, uvDestroyConn);
  taosMemoryFree(req);
}
static void uvPrepareCb(uv_prepare_t* handle) {
  // prepare callback
  SWorkThrd*  pThrd = handle->data;
  SAsyncPool* pool = pThrd->asyncPool;

  for (int i = 0; i < pool->nAsync; i++) {
    uv_async_t* async = &(pool->asyncs[i]);
    SAsyncItem* item = async->data;

    queue wq;
    taosThreadMutexLock(&item->mtx);
    QUEUE_MOVE(&item->qmsg, &wq);
    taosThreadMutexUnlock(&item->mtx);

    while (!QUEUE_IS_EMPTY(&wq)) {
      queue* head = QUEUE_HEAD(&wq);
      QUEUE_REMOVE(head);

      SSvrMsg* msg = QUEUE_DATA(head, SSvrMsg, q);
      if (msg == NULL) {
        tError("unexcept occurred, continue");
        continue;
      }
      // release handle to rpc init
      if (msg->type == Quit) {
        (*transAsyncHandle[msg->type])(msg, pThrd);
        continue;
      } else {
        STransMsg transMsg = msg->msg;

        SExHandle* exh1 = transMsg.info.handle;
        int64_t    refId = transMsg.info.refId;
        SExHandle* exh2 = transAcquireExHandle(transGetRefMgt(), refId);
        if (exh2 == NULL || exh1 != exh2) {
          tTrace("handle except msg %p, ignore it", exh1);
          transReleaseExHandle(transGetRefMgt(), refId);
          destroySmsg(msg);
          continue;
        }
        msg->pConn = exh1->handle;
        transReleaseExHandle(transGetRefMgt(), refId);
        (*transAsyncHandle[msg->type])(msg, pThrd);
      }
    }
  }
}

static void uvWorkDoTask(uv_work_t* req) {
  // doing time-consumeing task
  // only auth conn currently, add more func later
  tTrace("conn %p start to be processed in BG Thread", req->data);
  return;
}

static void uvWorkAfterTask(uv_work_t* req, int status) {
  if (status != 0) {
    tTrace("conn %p failed to processed ", req->data);
  }
  // Done time-consumeing task
  // add more func later
  // this func called in main loop
  tTrace("conn %p already processed ", req->data);
  taosMemoryFree(req);
}

void uvOnAcceptCb(uv_stream_t* stream, int status) {
  if (status == -1) {
    return;
  }
  SServerObj* pObj = container_of(stream, SServerObj, server);

  uv_tcp_t* cli = (uv_tcp_t*)taosMemoryMalloc(sizeof(uv_tcp_t));
  uv_tcp_init(pObj->loop, cli);

  if (uv_accept(stream, (uv_stream_t*)cli) == 0) {
    if (pObj->numOfWorkerReady < pObj->numOfThreads) {
      tError("worker-threads are not ready for all, need %d instead of %d.", pObj->numOfThreads,
             pObj->numOfWorkerReady);
      uv_close((uv_handle_t*)cli, NULL);
      return;
    }

    uv_write_t* wr = (uv_write_t*)taosMemoryMalloc(sizeof(uv_write_t));
    wr->data = cli;
    uv_buf_t buf = uv_buf_init((char*)notify, strlen(notify));

    pObj->workerIdx = (pObj->workerIdx + 1) % pObj->numOfThreads;

    tTrace("new conntion accepted by main server, dispatch to %dth worker-thread", pObj->workerIdx);

    uv_write2(wr, (uv_stream_t*)&(pObj->pipe[pObj->workerIdx][0]), &buf, 1, (uv_stream_t*)cli, uvOnPipeWriteCb);
  } else {
    uv_close((uv_handle_t*)cli, NULL);
  }
}
void uvOnConnectionCb(uv_stream_t* q, ssize_t nread, const uv_buf_t* buf) {
  tTrace("connection coming");
  if (nread < 0) {
    if (nread != UV_EOF) {
      tError("read error %s", uv_err_name(nread));
    }
    // TODO(log other failure reason)
    tWarn("failed to create connect:%p", q);
    taosMemoryFree(buf->base);
    uv_close((uv_handle_t*)q, NULL);
    // taosMemoryFree(q);
    return;
  }
  // free memory allocated by
  assert(nread == strlen(notify));
  assert(buf->base[0] == notify[0]);
  taosMemoryFree(buf->base);

  SWorkThrd* pThrd = q->data;

  uv_pipe_t* pipe = (uv_pipe_t*)q;
  if (!uv_pipe_pending_count(pipe)) {
    tError("No pending count");
    return;
  }

  uv_handle_type pending = uv_pipe_pending_type(pipe);
  assert(pending == UV_TCP);

  SSvrConn* pConn = createConn(pThrd);

  pConn->pTransInst = pThrd->pTransInst;
  /* init conn timer*/
  // uv_timer_init(pThrd->loop, &pConn->pTimer);
  // pConn->pTimer.data = pConn;

  pConn->hostThrd = pThrd;

  // init client handle
  pConn->pTcp = (uv_tcp_t*)taosMemoryMalloc(sizeof(uv_tcp_t));
  uv_tcp_init(pThrd->loop, pConn->pTcp);
  pConn->pTcp->data = pConn;

  transSetConnOption((uv_tcp_t*)pConn->pTcp);

  if (uv_accept(q, (uv_stream_t*)(pConn->pTcp)) == 0) {
    uv_os_fd_t fd;
    uv_fileno((const uv_handle_t*)pConn->pTcp, &fd);
    tTrace("conn %p created, fd:%d", pConn, fd);

    struct sockaddr peername, sockname;
    int             addrlen = sizeof(peername);
    if (0 != uv_tcp_getpeername(pConn->pTcp, (struct sockaddr*)&peername, &addrlen)) {
      tError("conn %p failed to get peer info", pConn);
      transUnrefSrvHandle(pConn);
      return;
    }
    transGetSockDebugInfo(&peername, pConn->dst);

    addrlen = sizeof(sockname);
    if (0 != uv_tcp_getsockname(pConn->pTcp, (struct sockaddr*)&sockname, &addrlen)) {
      tError("conn %p failed to get local info", pConn);
      transUnrefSrvHandle(pConn);
      return;
    }
    transGetSockDebugInfo(&sockname, pConn->src);
    struct sockaddr_in addr = *(struct sockaddr_in*)&sockname;

    pConn->clientIp = addr.sin_addr.s_addr;
    pConn->port = ntohs(addr.sin_port);
    uv_read_start((uv_stream_t*)(pConn->pTcp), uvAllocRecvBufferCb, uvOnRecvCb);

  } else {
    tDebug("failed to create new connection");
    transUnrefSrvHandle(pConn);
  }
}

void* transAcceptThread(void* arg) {
  // opt
  setThreadName("trans-accept");
  SServerObj* srv = (SServerObj*)arg;
  uv_run(srv->loop, UV_RUN_DEFAULT);

  return NULL;
}
void uvOnPipeConnectionCb(uv_connect_t* connect, int status) {
  if (status != 0) {
    return;
  }
  SWorkThrd* pThrd = container_of(connect, SWorkThrd, connect_req);
  uv_read_start((uv_stream_t*)pThrd->pipe, uvAllocConnBufferCb, uvOnConnectionCb);
}
static bool addHandleToWorkloop(SWorkThrd* pThrd, char* pipeName) {
  pThrd->loop = (uv_loop_t*)taosMemoryMalloc(sizeof(uv_loop_t));
  if (0 != uv_loop_init(pThrd->loop)) {
    return false;
  }

  uv_pipe_init(pThrd->loop, pThrd->pipe, 1);

  pThrd->pipe->data = pThrd;

  QUEUE_INIT(&pThrd->msg);

  pThrd->prepare = taosMemoryCalloc(1, sizeof(uv_prepare_t));
  uv_prepare_init(pThrd->loop, pThrd->prepare);
  uv_prepare_start(pThrd->prepare, uvPrepareCb);
  pThrd->prepare->data = pThrd;

  // conn set
  QUEUE_INIT(&pThrd->conn);

  pThrd->asyncPool = transAsyncPoolCreate(pThrd->loop, 1, pThrd, uvWorkerAsyncCb);
  uv_pipe_connect(&pThrd->connect_req, pThrd->pipe, pipeName, uvOnPipeConnectionCb);
  // uv_read_start((uv_stream_t*)pThrd->pipe, uvAllocConnBufferCb, uvOnConnectionCb);
  return true;
}

static bool addHandleToAcceptloop(void* arg) {
  // impl later
  SServerObj* srv = arg;

  int err = 0;
  if ((err = uv_tcp_init(srv->loop, &srv->server)) != 0) {
    tError("failed to init accept server:%s", uv_err_name(err));
    return false;
  }

  // register an async here to quit server gracefully
  srv->pAcceptAsync = taosMemoryCalloc(1, sizeof(uv_async_t));
  uv_async_init(srv->loop, srv->pAcceptAsync, uvAcceptAsyncCb);
  srv->pAcceptAsync->data = srv;

  struct sockaddr_in bind_addr;
  uv_ip4_addr("0.0.0.0", srv->port, &bind_addr);
  if ((err = uv_tcp_bind(&srv->server, (const struct sockaddr*)&bind_addr, 0)) != 0) {
    tError("failed to bind:%s", uv_err_name(err));
    return false;
  }
  if ((err = uv_listen((uv_stream_t*)&srv->server, 512, uvOnAcceptCb)) != 0) {
    tError("failed to listen:%s", uv_err_name(err));
    terrno = TSDB_CODE_RPC_PORT_EADDRINUSE;
    return false;
  }
  return true;
}
void* transWorkerThread(void* arg) {
  setThreadName("trans-worker");
  SWorkThrd* pThrd = (SWorkThrd*)arg;
  uv_run(pThrd->loop, UV_RUN_DEFAULT);

  return NULL;
}

static SSvrConn* createConn(void* hThrd) {
  SWorkThrd* pThrd = hThrd;

  SSvrConn* pConn = (SSvrConn*)taosMemoryCalloc(1, sizeof(SSvrConn));

  transReqQueueInit(&pConn->wreqQueue);
  QUEUE_INIT(&pConn->queue);

  QUEUE_PUSH(&pThrd->conn, &pConn->queue);

  transQueueInit(&pConn->srvMsgs, NULL);

  memset(&pConn->regArg, 0, sizeof(pConn->regArg));
  pConn->broken = false;
  pConn->status = ConnNormal;
  transInitBuffer(&pConn->readBuf);

  SExHandle* exh = taosMemoryMalloc(sizeof(SExHandle));
  exh->handle = pConn;
  exh->pThrd = pThrd;
  exh->refId = transAddExHandle(transGetRefMgt(), exh);
  transAcquireExHandle(transGetRefMgt(), exh->refId);

  STrans* pTransInst = pThrd->pTransInst;
  pConn->refId = exh->refId;
  transRefSrvHandle(pConn);
  tTrace("%s handle %p, conn %p created, refId:%" PRId64, transLabel(pTransInst), exh, pConn, pConn->refId);
  return pConn;
}

static void destroyConn(SSvrConn* conn, bool clear) {
  if (conn == NULL) {
    return;
  }

  if (clear) {
    if (!uv_is_closing((uv_handle_t*)conn->pTcp)) {
      tTrace("conn %p to be destroyed", conn);
      uv_close((uv_handle_t*)conn->pTcp, uvDestroyConn);
    }
  }
}
static void destroyConnRegArg(SSvrConn* conn) {
  if (conn->regArg.init == 1) {
    transFreeMsg(conn->regArg.msg.pCont);
    conn->regArg.init = 0;
  }
}
static int reallocConnRef(SSvrConn* conn) {
  transReleaseExHandle(transGetRefMgt(), conn->refId);
  transRemoveExHandle(transGetRefMgt(), conn->refId);
  // avoid app continue to send msg on invalid handle
  SExHandle* exh = taosMemoryMalloc(sizeof(SExHandle));
  exh->handle = conn;
  exh->pThrd = conn->hostThrd;
  exh->refId = transAddExHandle(transGetRefMgt(), exh);
  transAcquireExHandle(transGetRefMgt(), exh->refId);
  conn->refId = exh->refId;

  return 0;
}
static void uvDestroyConn(uv_handle_t* handle) {
  SSvrConn* conn = handle->data;
  if (conn == NULL) {
    return;
  }
  SWorkThrd* thrd = conn->hostThrd;

  transReleaseExHandle(transGetRefMgt(), conn->refId);
  transRemoveExHandle(transGetRefMgt(), conn->refId);

  STrans* pTransInst = thrd->pTransInst;
  tDebug("%s conn %p destroy", transLabel(pTransInst), conn);

  for (int i = 0; i < transQueueSize(&conn->srvMsgs); i++) {
    SSvrMsg* msg = transQueueGet(&conn->srvMsgs, i);
    destroySmsg(msg);
  }

  transReqQueueClear(&conn->wreqQueue);
  transQueueDestroy(&conn->srvMsgs);

  QUEUE_REMOVE(&conn->queue);
  taosMemoryFree(conn->pTcp);
  destroyConnRegArg(conn);
  transDestroyBuffer(&conn->readBuf);
  taosMemoryFree(conn);

  if (thrd->quit && QUEUE_IS_EMPTY(&thrd->conn)) {
    tTrace("work thread quit");
    uv_walk(thrd->loop, uvWalkCb, NULL);
  }
}
static void uvPipeListenCb(uv_stream_t* handle, int status) {
  ASSERT(status == 0);

  SServerObj* srv = container_of(handle, SServerObj, pipeListen);
  uv_pipe_t*  pipe = &(srv->pipe[srv->numOfWorkerReady][0]);
  ASSERT(0 == uv_pipe_init(srv->loop, pipe, 1));
  ASSERT(0 == uv_accept((uv_stream_t*)&srv->pipeListen, (uv_stream_t*)pipe));

  ASSERT(1 == uv_is_readable((uv_stream_t*)pipe));
  ASSERT(1 == uv_is_writable((uv_stream_t*)pipe));
  ASSERT(0 == uv_is_closing((uv_handle_t*)pipe));

  srv->numOfWorkerReady++;

  // ASSERT(0 == uv_listen((uv_stream_t*)&ctx.send.tcp, 512, uvOnAcceptCb));

  // r = uv_read_start((uv_stream_t*)&ctx.channel, alloc_cb, read_cb);
  // ASSERT(r == 0);
}

void* transInitServer(uint32_t ip, uint32_t port, char* label, int numOfThreads, void* fp, void* shandle) {
  SServerObj* srv = taosMemoryCalloc(1, sizeof(SServerObj));
  srv->loop = (uv_loop_t*)taosMemoryMalloc(sizeof(uv_loop_t));
  srv->numOfThreads = numOfThreads;
  srv->workerIdx = 0;
  srv->numOfWorkerReady = 0;
  srv->pThreadObj = (SWorkThrd**)taosMemoryCalloc(srv->numOfThreads, sizeof(SWorkThrd*));
  srv->pipe = (uv_pipe_t**)taosMemoryCalloc(srv->numOfThreads, sizeof(uv_pipe_t*));
  srv->ip = ip;
  srv->port = port;
  uv_loop_init(srv->loop);

  assert(0 == uv_pipe_init(srv->loop, &srv->pipeListen, 0));
#ifdef WINDOWS
  char pipeName[64];
  snprintf(pipeName, sizeof(pipeName), "\\\\?\\pipe\\trans.rpc.%p-" PRIu64, taosSafeRand(), GetCurrentProcessId());
#else
  char pipeName[PATH_MAX] = {0};
  snprintf(pipeName, sizeof(pipeName), "%s%spipe.trans.rpc.%08X-" PRIu64, tsTempDir, TD_DIRSEP, taosSafeRand(),
           taosGetSelfPthreadId());
#endif
  assert(0 == uv_pipe_bind(&srv->pipeListen, pipeName));
  assert(0 == uv_listen((uv_stream_t*)&srv->pipeListen, SOMAXCONN, uvPipeListenCb));

  for (int i = 0; i < srv->numOfThreads; i++) {
    SWorkThrd* thrd = (SWorkThrd*)taosMemoryCalloc(1, sizeof(SWorkThrd));
    thrd->pTransInst = shandle;
    thrd->quit = false;
    srv->pThreadObj[i] = thrd;
    thrd->pTransInst = shandle;

    srv->pipe[i] = (uv_pipe_t*)taosMemoryCalloc(2, sizeof(uv_pipe_t));
    thrd->pipe = &(srv->pipe[i][1]);  // init read

    if (false == addHandleToWorkloop(thrd, pipeName)) {
      goto End;
    }
    int err = taosThreadCreate(&(thrd->thread), NULL, transWorkerThread, (void*)(thrd));
    if (err == 0) {
      tDebug("success to create worker-thread:%d", i);
    } else {
      // TODO: clear all other resource later
      tError("failed to create worker-thread:%d", i);
      goto End;
    }
  }
  if (false == taosValidIpAndPort(srv->ip, srv->port)) {
    terrno = TAOS_SYSTEM_ERROR(errno);
    tError("invalid ip/port, %d:%d, reason:%s", srv->ip, srv->port, terrstr());
    goto End;
  }
  if (false == addHandleToAcceptloop(srv)) {
    goto End;
  }
  int err = taosThreadCreate(&srv->thread, NULL, transAcceptThread, (void*)srv);
  if (err == 0) {
    tDebug("success to create accept-thread");
  } else {
    tError("failed  to create accept-thread");
    goto End;
    // clear all resource later
  }
  srv->inited = true;
  return srv;
End:
  transCloseServer(srv);
  return NULL;
}

void uvHandleQuit(SSvrMsg* msg, SWorkThrd* thrd) {
  thrd->quit = true;
  if (QUEUE_IS_EMPTY(&thrd->conn)) {
    uv_walk(thrd->loop, uvWalkCb, NULL);
  } else {
    destroyAllConn(thrd);
  }
  taosMemoryFree(msg);
}
void uvHandleRelease(SSvrMsg* msg, SWorkThrd* thrd) {
  SSvrConn* conn = msg->pConn;
  if (conn->status == ConnAcquire) {
    reallocConnRef(conn);
    if (!transQueuePush(&conn->srvMsgs, msg)) {
      return;
    }
    uvStartSendRespInternal(msg);
    return;
  } else if (conn->status == ConnRelease || conn->status == ConnNormal) {
    tDebug("%s conn %p already released, ignore release-msg", transLabel(thrd->pTransInst), conn);
  }
  destroySmsg(msg);
}
void uvHandleResp(SSvrMsg* msg, SWorkThrd* thrd) {
  // send msg to client
  tDebug("%s conn %p start to send resp (2/2)", transLabel(thrd->pTransInst), msg->pConn);
  uvStartSendResp(msg);
}
void uvHandleRegister(SSvrMsg* msg, SWorkThrd* thrd) {
  SSvrConn* conn = msg->pConn;
  tDebug("%s conn %p register brokenlink callback", transLabel(thrd->pTransInst), conn);
  if (conn->status == ConnAcquire) {
    if (!transQueuePush(&conn->srvMsgs, msg)) {
      return;
    }
    transQueuePop(&conn->srvMsgs);
    conn->regArg.notifyCount = 0;
    conn->regArg.init = 1;
    conn->regArg.msg = msg->msg;
    tDebug("conn %p register brokenlink callback succ", conn);

    if (conn->broken) {
      STrans* pTransInst = conn->pTransInst;
      (*pTransInst->cfp)(pTransInst->parent, &(conn->regArg.msg), NULL);
      memset(&conn->regArg, 0, sizeof(conn->regArg));
    }
    taosMemoryFree(msg);
  }
}
void destroyWorkThrd(SWorkThrd* pThrd) {
  if (pThrd == NULL) {
    return;
  }
  taosThreadJoin(pThrd->thread, NULL);
  SRV_RELEASE_UV(pThrd->loop);
  TRANS_DESTROY_ASYNC_POOL_MSG(pThrd->asyncPool, SSvrMsg, destroySmsg);
  transAsyncPoolDestroy(pThrd->asyncPool);
  taosMemoryFree(pThrd->prepare);
  taosMemoryFree(pThrd->loop);
  taosMemoryFree(pThrd);
}
void sendQuitToWorkThrd(SWorkThrd* pThrd) {
  SSvrMsg* msg = taosMemoryCalloc(1, sizeof(SSvrMsg));
  msg->type = Quit;
  tDebug("server send quit msg to work thread");
  transAsyncSend(pThrd->asyncPool, &msg->q);
}

void transCloseServer(void* arg) {
  // impl later
  SServerObj* srv = arg;

  tDebug("send quit msg to accept thread");
  if (srv->inited) {
    uv_async_send(srv->pAcceptAsync);
    taosThreadJoin(srv->thread, NULL);
  }
  SRV_RELEASE_UV(srv->loop);

  for (int i = 0; i < srv->numOfThreads; i++) {
    sendQuitToWorkThrd(srv->pThreadObj[i]);
    destroyWorkThrd(srv->pThreadObj[i]);
  }

  taosMemoryFree(srv->pThreadObj);
  taosMemoryFree(srv->pAcceptAsync);
  taosMemoryFree(srv->loop);

  for (int i = 0; i < srv->numOfThreads; i++) {
    taosMemoryFree(srv->pipe[i]);
  }
  taosMemoryFree(srv->pipe);

  taosMemoryFree(srv);
}

void transRefSrvHandle(void* handle) {
  if (handle == NULL) {
    return;
  }
  int ref = T_REF_INC((SSvrConn*)handle);
  tTrace("conn %p ref count:%d", handle, ref);
}

void transUnrefSrvHandle(void* handle) {
  if (handle == NULL) {
    return;
  }
  int ref = T_REF_DEC((SSvrConn*)handle);
  tTrace("conn %p ref count:%d", handle, ref);
  if (ref == 0) {
    destroyConn((SSvrConn*)handle, true);
  }
}

int transReleaseSrvHandle(void* handle) {
  SRpcHandleInfo* info = handle;
  SExHandle*      exh = info->handle;
  int64_t         refId = info->refId;

  ASYNC_CHECK_HANDLE(exh, refId);

  SWorkThrd* pThrd = exh->pThrd;
  ASYNC_ERR_JRET(pThrd);

  STransMsg tmsg = {.code = 0, .info.handle = exh, .info.ahandle = NULL, .info.refId = refId};

  SSvrMsg* m = taosMemoryCalloc(1, sizeof(SSvrMsg));
  m->msg = tmsg;
  m->type = Release;

  tTrace("%s conn %p start to release", transLabel(pThrd->pTransInst), exh->handle);
  transAsyncSend(pThrd->asyncPool, &m->q);
  transReleaseExHandle(transGetRefMgt(), refId);
  return 0;
_return1:
  tTrace("handle %p failed to send to release handle", exh);
  transReleaseExHandle(transGetRefMgt(), refId);
  return -1;
_return2:
  tTrace("handle %p failed to send to release handle", exh);
  return -1;
}
int transSendResponse(const STransMsg* msg) {
  SExHandle* exh = msg->info.handle;
  int64_t    refId = msg->info.refId;
  ASYNC_CHECK_HANDLE(exh, refId);
  assert(refId != 0);

  STransMsg tmsg = *msg;
  tmsg.info.refId = refId;

  SWorkThrd* pThrd = exh->pThrd;
  ASYNC_ERR_JRET(pThrd);

  SSvrMsg* m = taosMemoryCalloc(1, sizeof(SSvrMsg));
  m->msg = tmsg;
  m->type = Normal;

  STraceId* trace = (STraceId*)&msg->info.traceId;
  tGTrace("conn %p start to send resp (1/2)", exh->handle);
  transAsyncSend(pThrd->asyncPool, &m->q);
  transReleaseExHandle(transGetRefMgt(), refId);
  return 0;
_return1:
  tTrace("handle %p failed to send resp", exh);
  rpcFreeCont(msg->pCont);
  transReleaseExHandle(transGetRefMgt(), refId);
  return -1;
_return2:
  tTrace("handle %p failed to send resp", exh);
  rpcFreeCont(msg->pCont);
  return -1;
}
int transRegisterMsg(const STransMsg* msg) {
  SExHandle* exh = msg->info.handle;
  int64_t    refId = msg->info.refId;
  ASYNC_CHECK_HANDLE(exh, refId);

  STransMsg tmsg = *msg;
  tmsg.info.refId = refId;

  SWorkThrd* pThrd = exh->pThrd;
  ASYNC_ERR_JRET(pThrd);

  SSvrMsg* m = taosMemoryCalloc(1, sizeof(SSvrMsg));
  m->msg = tmsg;
  m->type = Register;

  STrans* pTransInst = pThrd->pTransInst;
  tTrace("%s conn %p start to register brokenlink callback", transLabel(pTransInst), exh->handle);
  transAsyncSend(pThrd->asyncPool, &m->q);
  transReleaseExHandle(transGetRefMgt(), refId);
  return 0;

_return1:
  tTrace("handle %p failed to register brokenlink", exh);
  rpcFreeCont(msg->pCont);
  transReleaseExHandle(transGetRefMgt(), refId);
  return -1;
_return2:
  tTrace("handle %p failed to register brokenlink", exh);
  rpcFreeCont(msg->pCont);
  return -1;
}

int transGetConnInfo(void* thandle, STransHandleInfo* pConnInfo) { return -1; }

#endif
