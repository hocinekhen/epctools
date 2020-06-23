/*
* Copyright (c) 2020 Sprint
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*    http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/

#include "epfcp.h"

namespace PFCP
{
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

UShort Configuration::port_         = 8805;
Int Configuration::bufsize_         = 2097152;
LongLong Configuration::t1_         = 3000;
LongLong Configuration::hbt1_       = 5000;
Int Configuration::n1_              = 2;
Int Configuration::hbn1_            = 3;
ELogger *Configuration::logger_     = nullptr;
size_t Configuration::naw_          = 10;
Long Configuration::law_            = 6000; // 6 seconds
Int Configuration::trb_             = 0;
Bool Configuration::atr_            = False;
Translator *Configuration::xlator_  = nullptr;

ApplicationThread *ApplicationThread::this_ = nullptr;
TranslationThread *TranslationThread::this_ = nullptr;
CommunicationThread *CommunicationThread::this_ = nullptr;

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

Void Initialize()
{
   static EString __method__ = __METHOD_NAME__;

   Configuration::logger().startup("{} - initializing the timer pool", __method__);
   ETimerPool::Instance().init();
   Configuration::logger().startup("{} - initializing the communication thread", __method__);
   CommunicationThread::Instance().init(1, 101, NULL, 100000);
   Configuration::logger().startup("{} - initializing the translation thread", __method__);
   TranslationThread::Instance().init(1, 102, NULL, 100000);
}

Void Uninitialize()
{
   static EString __method__ = __METHOD_NAME__;

   Configuration::logger().startup("{} - releasing local nodes");
   CommunicationThread::Instance().releaseLocalNodes();

   Configuration::logger().startup("{} - stopping the translation thread");
   TranslationThread::Instance().quit();
   TranslationThread::Instance().join();
   TranslationThread::cleanup();

   Configuration::logger().startup("{} - stopping the communication thread");
   CommunicationThread::Instance().quit();
   CommunicationThread::Instance().join();
   CommunicationThread::cleanup();
}
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

TeidRangeManager::TeidRangeManager(Int rangeBits)
   : bits_(rangeBits)
{
   static EString __method__ = __METHOD_NAME__;

   if (bits_ < 0 || bits_ > 7)
      throw TeidRangeManager_InvalidRangeBits();
   
   Int range = 1 << rangeBits;
   for (Int i=0; i<range; i++)
      free_.push_back(i);
}

Bool TeidRangeManager::assign(RemoteNodeSPtr &n)
{
   static EString __method__ = __METHOD_NAME__;

   if (free_.size() > 0)
   {
      Int trv = free_.front();
      free_.pop_front();
      n->setTeidRangeValue(trv);
      used_[trv] = n;
      return True;
   }
   return False;
}

Void TeidRangeManager::release(RemoteNodeSPtr &n)
{
   static EString __method__ = __METHOD_NAME__;

   Int trv = n->teidRangeValue();
   if (used_.erase(trv) == 1)
      free_.push_back(trv);
   n->setTeidRangeValue(-1);
}

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

NodeSocket::NodeSocket()
   : ESocket::UdpPrivate(CommunicationThread::Instance(), Configuration::socketBufferSize())
{
   static EString __method__ = __METHOD_NAME__;
}

NodeSocket::~NodeSocket()
{
   static EString __method__ = __METHOD_NAME__;
}

Void NodeSocket::onReceive(const ESocket::Address &src, const ESocket::Address &dst, cpUChar msg, Int len)
{
   static EString __method__ = __METHOD_NAME__;

   ln_->onReceive(ln_, src, dst, msg, len);
}

Void NodeSocket::onError()
{
   static EString __method__ = __METHOD_NAME__;
}

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

RemoteNode::RemoteNode()
   : trv_(-1),
     awndcnt_(0),
     aw_(0)
{
   static EString __method__ = __METHOD_NAME__;
}

RemoteNode::~RemoteNode()
{
   static EString __method__ = __METHOD_NAME__;
}

Bool RemoteNode::addRcvdReq(ULong sn)
{
   static EString __method__ = __METHOD_NAME__;

   if (rcvdReqExists(sn))
      return False;

   RcvdReq rr(sn);

   auto result = rrumap_.insert(std::make_pair(rr.seqNbr(), rr));

   return result.second;
}

Bool RemoteNode::setRcvdReqRspWnd(ULong sn, Int wnd)
{
   static EString __method__ = __METHOD_NAME__;

   auto it = rrumap_.find(sn);
   if (it == rrumap_.end())
      return False;
   it->second.setRspWnd(wnd);
   return True;
}

Void RemoteNode::removeRcvdRqstEntries(Int wnd)
{
   static EString __method__ = __METHOD_NAME__;

   auto it = rrumap_.begin();
   while (it != rrumap_.end())
   {
      if (it->second.rspWnd() == wnd)
         it = rrumap_.erase(it);
      else
         it++;
   }
}

RemoteNode &RemoteNode::setNbrActivityWnds(size_t nbr)
{
   static EString __method__ = __METHOD_NAME__;

   awnds_.clear();
   for (size_t idx=0; idx<nbr; idx++)
      awnds_.push_back(0);
   awnds_.shrink_to_fit();
   awndcnt_ = 0;
   return *this;
}

Void RemoteNode::nextActivityWnd(Int wnd)
{
   static EString __method__ = __METHOD_NAME__;

   aw_ = wnd;
   if (awndcnt_ < awnds_.size())
      awndcnt_++;
   awnds_[aw_] = 0;  // clear the previous activity in the new wnd
}

Bool RemoteNode::checkActivity()
{
    static EString __method__ = __METHOD_NAME__;

  if (awndcnt_ < awnds_.size())
   {
      // return True to indicate that there has been activity since
      // a full set of wnds has not been processed yet
      return True;
   }

   for (auto &val : awnds_)
   {
      if (val > 0)
         return True;
   }
   
   return False;
}

Void RemoteNode::removeOldReqs(Int rw)
{
   static EString __method__ = __METHOD_NAME__;

   auto entry = rrumap_.begin();
   while (entry != rrumap_.end())
   {
      if (entry->second.rspWnd() == rw)
         entry = rrumap_.erase(entry);
      else
         entry++;
   }
}

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

LocalNode::LocalNode()
{
   static EString __method__ = __METHOD_NAME__;
}

LocalNode::~LocalNode()
{
   static EString __method__ = __METHOD_NAME__;

   auto entry = roumap_.begin();
   while (entry != roumap_.end())
   {
      delete entry->second;
      entry = roumap_.erase(entry);
   }
}

Seid LocalNode::allocSeid()
{
   static EString __method__ = __METHOD_NAME__;

   return seidmgr_.alloc();
}

Void LocalNode::freeSeid(Seid seid)
{
   static EString __method__ = __METHOD_NAME__;

   seidmgr_.free(seid);
}

ULong LocalNode::allocSeqNbr()
{
   static EString __method__ = __METHOD_NAME__;

   return seqmgr_.alloc();
}

Void LocalNode::freeSeqNbr(ULong sn)
{
   static EString __method__ = __METHOD_NAME__;

   seqmgr_.free(sn);
}

Bool LocalNode::rqstOutExists(ULong seqnbr) const
{
   static EString __method__ = __METHOD_NAME__;

   auto it = roumap_.find(seqnbr);
   return it != roumap_.end();
}

Bool LocalNode::addRqstOut(ReqOut *ro)
{
   static EString __method__ = __METHOD_NAME__;

   if (rqstOutExists(ro->seqNbr()))
      return False;
   
   auto result = roumap_.insert(std::make_pair(ro->seqNbr(), ro));

   return result.second;
}

Bool LocalNode::setRqstOutRespWnd(ULong seqnbr, Int wnd)
{
   static EString __method__ = __METHOD_NAME__;

   auto it = roumap_.find(seqnbr);
   if (it == roumap_.end())
      return False;
   it->second->setRspWnd(wnd);
   return True;
}

Void LocalNode::removeRqstOutEntries(Int wnd)
{
   static EString __method__ = __METHOD_NAME__;

   auto it = roumap_.begin();
   while (it != roumap_.end())
   {
      if (it->second->rspWnd() == wnd)
      {
         ReqOut *ro = it->second;
         it = roumap_.erase(it);
         delete ro;
      }
      else
      {
         it++;
      }
   }
}

Void LocalNode::setNbrActivityWnds(size_t nbr)
{
   static EString __method__ = __METHOD_NAME__;

   for (auto &kv : rns_)
      kv.second->setNbrActivityWnds(nbr);
}

Void LocalNode::nextActivityWnd(Int wnd)
{
   static EString __method__ = __METHOD_NAME__;

   for (auto &kv : rns_)
      kv.second->nextActivityWnd(wnd);
}

Void LocalNode::checkActivity(LocalNodeSPtr &ln)
{
   static EString __method__ = __METHOD_NAME__;

   // check for activity from all of the RemoteNode's
   for (auto &kv : rns_)
   {
      if (!kv.second->checkActivity())
      {
         Configuration::logger().debug("{} - remote {} is inactive", __method__, kv.second->ipAddress().address());
         SndHeartbeatReqDataPtr shb = new SndHeartbeatReqData(ln, kv.second);
         SEND_TO_TRANSLATION(SndHeartbeatReq, shb);

         // increment the activity for the RemoteNode to prevent a heartbeat from going out 
         // until there is no activity in all windows
         kv.second->incrementActivity();
      }
   }
}

RemoteNodeSPtr LocalNode::createRemoteNode(EIpAddress &address, UShort port)
{
   static EString __method__ = __METHOD_NAME__;

   try
   {
      // create the RemoteNode shared pointer
      RemoteNodeSPtr rn = std::make_shared<RemoteNode>();

      // set the IP address for the RemoteNode
      ESocket::Address rnaddr;
      if (address.family() == AF_INET)
         rn->setAddress(ESocket::Address(address.ipv4Address(), port));
      else
         rn->setAddress(ESocket::Address(address.ipv6Address(), port));
      rn->setIpAddress(address);

      // assign the TEID range value for the RemotNode
      if (!CommunicationThread::Instance().assignTeidRangeValue(rn))
         throw RemoteNodeException_UnableToAssignTeidRangeValue();

      // set the number of activity wnds for the RemoteNode
      rn->setNbrActivityWnds(Configuration::nbrActivityWnds());

      // set the activity wnd
      rn->nextActivityWnd(CommunicationThread::Instance().currentActivityWnd());

      // add the RemoteNode to the RemodeNode collection for this LocalNode
      auto result = rns_.insert(std::make_pair(rn->ipAddress(), rn));

      if (!result.second)
         throw LocalNodeException_RemoteNodeUMapInsertFailed();
      
      RemoteNodeSPtr *rnp = new RemoteNodeSPtr();
      (*rnp) = rn;
      SEND_TO_APPLICATION(RemoteNodeAdded, rnp);

      return result.first->second;
   }
   catch (const std::exception &e)
   {
      Configuration::logger().minor("{} - address={} exception - {}",
         __method__, address.address(), e.what());
      throw LocalNodeException_UnableToCreateRemoteNode();
   }
}

Void LocalNode::onReceive(LocalNodeSPtr &ln, const ESocket::Address &src, const ESocket::Address &dst, cpUChar msg, Int len)
{
   static EString __method__ = __METHOD_NAME__;
   TranslatorMsgInfo tmi;
   RemoteNodeSPtr rn;

   try
   {
      // get the msg header info
      Configuration::translator().getMsgInfo(tmi, msg, len);

      // lookup the remote node and create it if it does not exist
      EIpAddress remoteIpAddress(src.getSockAddrStorage());
      RemoteNodeUMap::iterator rnit = rns_.find(remoteIpAddress);
      Configuration::logger().debug("{} - looking up remoteNode {}",
         __method__, remoteIpAddress.address());
      rn = (rnit != rns_.end()) ?
         rnit->second :
         createRemoteNode(remoteIpAddress, Configuration::port());

      // increment the activity for the RemoteNode
      rn->incrementActivity();

      if (tmi.isReq())
      {
         // check to see if this is a duplicate req
         if (!rn->rcvdReqExists(tmi.seqNbr()))
         {
            // create and populate ReqIn
            ReqInPtr ri = new ReqIn();
            ri->setLocalNode(ln);
            ri->setRemoteNode(rn);
            ri->setSeid(tmi.seid());
            ri->setSeqNbr(tmi.seqNbr());
            ri->setMsgType(tmi.msgType());
            ri->setIsReq(tmi.isReq());
            ri->setVersion(tmi.version());
            ri->assign(msg, len);

            // add RcvdReqeust
            if (rn->addRcvdReq(ri->seqNbr()))
            {
               // snd ReqIn to TranslationThread
               SEND_TO_TRANSLATION(RcvdReq, ri);
            }
            else
            {
               Configuration::logger().debug(
                  "{} - unable to insert RcvdReq in the RemoteNode,"
                  " discarding req local={} remote={} seid={} msgType={} seqNbr={} version={} msgLen={}",
                  __method__, ln->ipAddress().address(), rn->ipAddress().address(), tmi.seid(),
                  tmi.msgType(), tmi.seqNbr(), tmi.version(), len);
               delete ri;
            }
         }
         else
         {
            // duplicate msg, so discard it
            Configuration::logger().debug(
               "{} - discarding duplicate req local={} remote={} seid={} msgType={} seqNbr={} version={} msgLen={}",
               __method__, ln->ipAddress().address(), rn->ipAddress().address(), tmi.seid(),
               tmi.msgType(), tmi.seqNbr(), tmi.version(), len);
         }
      }
      else
      {
         // locate the corresponding ReqOut entry
         auto roit = roumap_.find(tmi.seqNbr());
         if (roit != roumap_.end())
         {
            // ReqOut entry found, set the rsp wnd for the req
            roit->second->setRspWnd(CommunicationThread::Instance().currentRspWnd());

            // stop the retransmit timer
            roit->second->stopT1();

            // create and poulate RspIn
            RspInPtr ri = new RspIn();
            ri->setReq(roit->second->appMsg());
            ri->setLocalNode(ln);
            ri->setRemoteNode(rn);
            ri->setSeid(tmi.seid());
            ri->setSeqNbr(tmi.seqNbr());
            ri->setMsgType(tmi.msgType());
            ri->setIsReq(tmi.isReq());
            ri->setVersion(tmi.version());
            ri->assign(msg, len);

            // snd RspIn to TranslationThread
            SEND_TO_TRANSLATION(RcvdRsp, ri);
         }
         else
         {
            // ReqOut entry NOT found, discard the rsp msg
            Configuration::logger().debug(
               "{} - corresponding ReqOut entry not found,"
               " discarding rsp local={} remote={} seid={} msgType={} seqNbr={} version={} msgLen={}",
               __method__, ln->ipAddress().address(), rn->ipAddress().address(), tmi.seid(),
               tmi.msgType(), tmi.seqNbr(), tmi.version(), len);
         }
      }
   }
   catch (const LocalNodeException_UnableToCreateRemoteNode &e)
   {
      Configuration::logger().minor(
         "{} - {} - discarding msg local={} remote={} seid={} msgType={} seqNbr={} version={} msgLen={}",
         __method__, e.what(), dst.getAddress(), src.getAddress(), tmi.seid(),
         tmi.msgType(), tmi.seqNbr(), tmi.version(), len);
   }
}

Bool LocalNode::onReqOutTimeout(ReqOutPtr ro)
{
   static EString __method__ = __METHOD_NAME__;
   if (ro == nullptr)
   {
      Configuration::logger().minor("{} - the ReqOutPtr is NULL", __method__);
      return False;
   }

   // lookup the ReqOut entry
   auto entry = roumap_.find(ro->seqNbr());
   if (entry != roumap_.end()) // found the entry
   {
      if (sndReq(ro))
         return True;
      roumap_.erase(entry);
   }
   else
   {
      // ReqOut entry NOT found, do not surface the timeout error
      Configuration::logger().debug(
         "{} - corresponding ReqOut entry not found,"
         " discarding tiemout local={} remote={}",
         __method__, ipAddress().address(), ro->remoteNode()->address().getAddress());
   }

   return False;
}

Void LocalNode::removeOldReqs(Int rw)
{
   static EString __method__ = __METHOD_NAME__;

   // remove the old ReqOut entries
   auto entry = roumap_.begin();
   while (entry != roumap_.end())
   {
      if (entry->second->rspWnd() == rw)
      {
         ReqOutPtr ro = entry->second;
         entry = roumap_.erase(entry);
         delete ro;
      }
      else
      {
         entry++;
      }
   }

   // remove the old RcvdReq entries
   for (auto &kv : rns_)
      kv.second->removeOldReqs(rw);
}

Void LocalNode::sndInitialReq(ReqOutPtr ro)
{
   static EString __method__ = __METHOD_NAME__;

   // check to see if the ReqOut entry already exists
   auto roit = roumap_.find(ro->seqNbr());
   if (roit == roumap_.end())
   {
      // add the ReqOut entry to retransmit collection
      roumap_.insert(std::make_pair(ro->seqNbr(), ro));
      sndReq(ro);
   }
   else
   {
      // log the error
      Configuration::logger().major(
         "{} - seqNbr {} already exists in retransmission collection "
         "local={} remote={} seid={} msgType={} seqNbr={} version={} msgLen={}",
         __method__, ipAddress().address(), ro->remoteNode()->ipAddress().address(),
         ro->seid(), ro->msgType(), ro->seqNbr(), ro->version(), ro->len());
      // snd the error to the application thread
      SndReqExceptionData *exdata = new SndReqExceptionData();
      exdata->req = ro->appMsg();
      SEND_TO_APPLICATION(SndReqError, exdata);
      // delete the ReqOut object
      delete ro;
   }
}

Bool LocalNode::sndReq(ReqOutPtr ro)
{
   static EString __method__ = __METHOD_NAME__;

   if (ro->okToSnd()) // implicitly decrements the retransmission count
   {
      // snd the data
      socket_.write(ro->remoteNode()->address(), ro->data(), ro->len());
      ro->startT1();
      return True;
   }

   if (ro->msgType() == PfcpHeartbeatReq)
   {
      // send remote node failure notice to the ApplicationThread
      RemoteNodeSPtr *rn = new RemoteNodeSPtr();
      (*rn) = ro->remoteNode();
      SEND_TO_APPLICATION(RemoteNodeFailure, rn);

      Configuration::logger().major(
         "{} - remote node is non-responsive local={} remote={}",
         __method__, address().getAddress(), ro->remoteNode()->address().getAddress());

      // delete the AppMsgReq object since the communication thread is responsible for it
      delete ro->appMsg();
   }
   else
   {
      // send the timeout notice for the AppMsgReq to the ApplicationThread
      SEND_TO_APPLICATION(ReqTimeout, ro->appMsg());
   }
   // clear the AppMsgReqPtr (doesn't do anything since the ReqOut destructor doesn't try to delete it)
   ro->setAppMsg(nullptr);

   return False;
}

Void LocalNode::sndRsp(RspOutPtr ro)
{
   static EString __method__ = __METHOD_NAME__;

   if (ro->remoteNode()->setRcvdReqRspWnd(ro->seqNbr()))
   {
      // snd the data
      socket_.write(ro->remoteNode()->address(), ro->data(), ro->len());
   }
   else
   {
      // the corresponding req does not exist, so don't snd the rsp
      SEND_TO_APPLICATION(SndRspError, ro->rsp());
      ro->setRsp(nullptr);
   }
   // delete the RspOut object
   delete ro;
}

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

Translator::Translator()
{
}

Translator::~Translator()
{
}

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

BEGIN_MESSAGE_MAP(ApplicationThread, EThreadPrivate)
   ON_MESSAGE(static_cast<UInt>(ApplicationThread::Events::RcvdReq), ApplicationThread::onRcvdReq)
   ON_MESSAGE(static_cast<UInt>(ApplicationThread::Events::RcvdRsp), ApplicationThread::onRcvdRsp)
   ON_MESSAGE(static_cast<UInt>(ApplicationThread::Events::ReqTimeout), ApplicationThread::onReqTimeout)
   ON_MESSAGE(static_cast<UInt>(ApplicationThread::Events::RemoteNodeAdded), ApplicationThread::onRemoteNodeAdded)
   ON_MESSAGE(static_cast<UInt>(ApplicationThread::Events::RemoteNodeFailure), ApplicationThread::onRemoteNodeFailure)
   ON_MESSAGE(static_cast<UInt>(ApplicationThread::Events::RemoteNodeRestart), ApplicationThread::onRemoteNodeRestart)
   ON_MESSAGE(static_cast<UInt>(ApplicationThread::Events::RemoteNodeRemoved), ApplicationThread::onRemoteNodeRemoved)
   ON_MESSAGE(static_cast<UInt>(ApplicationThread::Events::SndReqError), ApplicationThread::onSndReqError)
   ON_MESSAGE(static_cast<UInt>(ApplicationThread::Events::SndRspError), ApplicationThread::onSndRspError)
END_MESSAGE_MAP()

ApplicationThread::ApplicationThread()
{
   static EString __method__ = __METHOD_NAME__;

   this_ = this;
}

ApplicationThread::~ApplicationThread()
{
   static EString __method__ = __METHOD_NAME__;
}

Void ApplicationThread::onInit()
{
   static EString __method__ = __METHOD_NAME__;

   EThreadPrivate::onInit();
}

Void ApplicationThread::onQuit()
{
   static EString __method__ = __METHOD_NAME__;

   EThreadPrivate::onQuit();
}

Void ApplicationThread::onRcvdReq(AppMsgReqPtr req)
{
   static EString __method__ = __METHOD_NAME__;
   Configuration::logger().debug(
      "ApplicationThread::onReqRcvd()"
      " seid={}"
      " seqNbr={}"
      " msgType={}"
      " isReq={}",
      __method__, req->seid(), req->seqNbr(), req->msgType(),(req->isReq()?"True":"False"));
}

Void ApplicationThread::onRcvdRsp(AppMsgRspPtr rsp)
{
   static EString __method__ = __METHOD_NAME__;
   Configuration::logger().debug(
      "{}"
      " seid={}"
      " seqNbr={}"
      " msgType={}"
      " isReq={}",
      __method__, rsp->seid(), rsp->seqNbr(), rsp->msgType(),(rsp->isReq()?"True":"False"));
}

Void ApplicationThread::onReqTimeout(AppMsgReqPtr req)
{
   static EString __method__ = __METHOD_NAME__;
   Configuration::logger().debug(
      "{}"
      " seid={}"
      " seqNbr={}"
      " msgType={}"
      " isReq={}",
      __method__, req->seid(), req->seqNbr(), req->msgType(),(req->isReq()?"True":"False"));
}

Void ApplicationThread::onRemoteNodeAdded(RemoteNodeSPtr &rmtNode)
{
   static EString __method__ = __METHOD_NAME__;
   Configuration::logger().debug(
      "{}"
      " address={} startTime={}",
      __method__, rmtNode->ipAddress().address(), rmtNode->startTime().Format("%FT%T", False));
}

Void ApplicationThread::onRemoteNodeFailure(RemoteNodeSPtr &rmtNode)
{
   static EString __method__ = __METHOD_NAME__;
   Configuration::logger().debug(
      "{}"
      " address={} startTime={}",
      __method__, rmtNode->ipAddress().address(), rmtNode->startTime().Format("%FT%T", False));
}

Void ApplicationThread::onRemoteNodeRestart(RemoteNodeSPtr &rmtNode)
{
   static EString __method__ = __METHOD_NAME__;
   Configuration::logger().debug(
      "{}"
      " address={} startTime={}",
      __method__, rmtNode->ipAddress().address(), rmtNode->startTime().Format("%FT%T", False));
}

Void ApplicationThread::onRemoteNodeRemoved(RemoteNodeSPtr &rmtNode)
{
   static EString __method__ = __METHOD_NAME__;
   Configuration::logger().debug(
      "{}"
      " address={} startTime={}",
      __method__, rmtNode->ipAddress().address(), rmtNode->startTime().Format("%FT%T", False));
}

Void ApplicationThread::onSndReqError(AppMsgReqPtr req, SndReqException &err)
{
   static EString __method__ = __METHOD_NAME__;
   Configuration::logger().debug(
      "{}"
      " seid={}"
      " seqNbr={}"
      " msgType={}"
      " isReq={}"
      " exception={}",
      __method__, req->seid(), req->seqNbr(), req->msgType(),(req->isReq()?"True":"False"), err.what());
}

Void ApplicationThread::onSndRspError(AppMsgRspPtr rsp, SndRspException &err)
{
   static EString __method__ = __METHOD_NAME__;
   Configuration::logger().debug(
      "{}"
      " seid={}"
      " seqNbr={}"
      " msgType={}"
      " isReq={}"
      " exception={}",
      __method__, rsp->seid(), rsp->seqNbr(), rsp->msgType(),(rsp->isReq()?"True":"False"), err.what());
}

Void ApplicationThread::onEncodeReqError(PFCP::AppMsgReqPtr req, PFCP::EncodeReqException &err)
{
   static EString __method__ = __METHOD_NAME__;
   Configuration::logger().debug(
      "{}"
      " seid={}"
      " seqNbr={}"
      " msgType={}"
      " isReq={}"
      " exception={}",
      __method__, req->seid(), req->seqNbr(), req->msgType(),(req->isReq()?"True":"False"), err.what());
}

Void ApplicationThread::onEncodeRspError(PFCP::AppMsgRspPtr rsp, PFCP::EncodeRspException &err)
{
   static EString __method__ = __METHOD_NAME__;
   Configuration::logger().debug(
      "{}"
      " seid={}"
      " seqNbr={}"
      " msgType={}"
      " isReq={}"
      " exception={}",
      __method__, rsp->seid(), rsp->seqNbr(), rsp->msgType(),(rsp->isReq()?"True":"False"), err.what());
}

Void ApplicationThread::onRcvdReq(EThreadMessage &msg)
{
   static EString __method__ = __METHOD_NAME__;
   AppMsgReqPtr req = static_cast<AppMsgReqPtr>(msg.getVoidPtr());
   onRcvdReq(req);
}

Void ApplicationThread::onRcvdRsp(EThreadMessage &msg)
{
   static EString __method__ = __METHOD_NAME__;
   AppMsgRspPtr rsp =  static_cast<AppMsgRspPtr>(msg.getVoidPtr());
   onRcvdRsp(rsp);
}

Void ApplicationThread::onReqTimeout(EThreadMessage &msg)
{
   static EString __method__ = __METHOD_NAME__;
   AppMsgReqPtr req = static_cast<AppMsgReqPtr>(msg.getVoidPtr());
   onReqTimeout(req);
}

Void ApplicationThread::onRemoteNodeAdded(EThreadMessage &msg)
{
   static EString __method__ = __METHOD_NAME__;
   RemoteNodeSPtr *rn = static_cast<RemoteNodeSPtr*>(msg.getVoidPtr());
   onRemoteNodeAdded(*rn);
   delete rn;
}

Void ApplicationThread::onRemoteNodeFailure(EThreadMessage &msg)
{
   static EString __method__ = __METHOD_NAME__;
   RemoteNodeSPtr *rn = static_cast<RemoteNodeSPtr*>(msg.getVoidPtr());
   onRemoteNodeFailure(*rn);
   delete rn;
}

Void ApplicationThread::onRemoteNodeRestart(EThreadMessage &msg)
{
   static EString __method__ = __METHOD_NAME__;
   RemoteNodeSPtr *rn = static_cast<RemoteNodeSPtr*>(msg.getVoidPtr());
   onRemoteNodeAdded(*rn);
   delete rn;
}

Void ApplicationThread::onRemoteNodeRemoved(EThreadMessage &msg)
{
   static EString __method__ = __METHOD_NAME__;
   RemoteNodeSPtr *rn = static_cast<RemoteNodeSPtr*>(msg.getVoidPtr());
   onRemoteNodeRestart(*rn);
   delete rn;
}

Void ApplicationThread::onSndReqError(EThreadMessage &msg)
{
   static EString __method__ = __METHOD_NAME__;
   SndReqExceptionDataPtr data = static_cast<SndReqExceptionDataPtr>(msg.getVoidPtr());
   onSndReqError(data->req, data->err);
   delete data;
}

Void ApplicationThread::onSndRspError(EThreadMessage &msg)
{
   static EString __method__ = __METHOD_NAME__;
   SndRspExceptionDataPtr data = static_cast<SndRspExceptionDataPtr>(msg.getVoidPtr());
   onSndRspError(data->rsp, data->err);
   delete data;
}

Void ApplicationThread::onEncodeReqError(EThreadMessage &msg)
{
   static EString __method__ = __METHOD_NAME__;
   EncodeReqExceptionDataPtr data = static_cast<EncodeReqExceptionDataPtr>(msg.getVoidPtr());
   onEncodeReqError(data->req, data->err);
   delete data;
}

Void ApplicationThread::onEncodeRspError(EThreadMessage &msg)
{
   static EString __method__ = __METHOD_NAME__;
   EncodeRspExceptionDataPtr data = static_cast<EncodeRspExceptionDataPtr>(msg.getVoidPtr());
   onEncodeRspError(data->rsp, data->err);
   delete data;
}

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

BEGIN_MESSAGE_MAP(TranslationThread, EThreadPrivate)
   ON_MESSAGE(static_cast<UInt>(TranslationThread::Events::SndMsg), TranslationThread::onSndPfcpMsg)
   ON_MESSAGE(static_cast<UInt>(TranslationThread::Events::RcvdReq), TranslationThread::onRcvdReq)
   ON_MESSAGE(static_cast<UInt>(TranslationThread::Events::RcvdRsp), TranslationThread::onRcvdRsp)
   ON_MESSAGE(static_cast<UInt>(TranslationThread::Events::SndHeartbeatReq), TranslationThread::onSndHeartbeatReq)
   ON_MESSAGE(static_cast<UInt>(TranslationThread::Events::SndHeartbeatRsp), TranslationThread::onSndHeartbeatRsp)
END_MESSAGE_MAP()

TranslationThread::TranslationThread()
   : xlator_(Configuration::translator())
{
   static EString __method__ = __METHOD_NAME__;

   this_ = this;
}

TranslationThread::~TranslationThread()
{
   static EString __method__ = __METHOD_NAME__;

   this_ = nullptr;
}

Void TranslationThread::onInit()
{
   static EString __method__ = __METHOD_NAME__;

   EThreadPrivate::onInit();

   Configuration::logger().startup("{} - the translation thread has been started", __method__);
}

Void TranslationThread::onQuit()
{
   static EString __method__ = __METHOD_NAME__;

   EThreadPrivate::onQuit();
}

Void TranslationThread::onSndPfcpMsg(EThreadMessage &msg)
{
   static EString __method__ = __METHOD_NAME__;
   AppMsgPtr am = static_cast<AppMsgPtr>(msg.getVoidPtr());

   if (am->isReq())
   {
      Configuration::logger().debug("{} - sending request msgType={} seid={} seqNbr={}",
         __method__, am->msgType(), am->seid(), am->seqNbr());

      AppMsgReqPtr amrq = static_cast<AppMsgReqPtr>(am);
      ReqOutPtr reqout = nullptr;
      try
      {
         reqout = xlator_.encodeReq(amrq);
         SEND_TO_COMMUNICATION(SndReq, reqout);
      }
      catch(SndReqException &e)
      {
         auto data = new SndReqExceptionData();
         data->req = amrq;
         data->err = e;
         SEND_TO_APPLICATION(SndReqError, data);
         if (reqout != nullptr)
            delete reqout;
      }
      catch(EncodeReqException &e)
      {
         auto data = new EncodeReqExceptionData();
         data->req = amrq;
         data->err = e;
         SEND_TO_APPLICATION(EncodeReqError, data);
         if (reqout != nullptr)
            delete reqout;
      }
   }
   else
   {
      Configuration::logger().debug("{} - sending response msgType={} seid={} seqNbr={}",
         __method__, am->msgType(), am->seid(), am->seqNbr());

      AppMsgRspPtr amrs = static_cast<AppMsgRspPtr>(am);
      RspOutPtr rspout = nullptr;
      try
      {
         rspout = xlator_.encodeRsp(amrs);
         SEND_TO_COMMUNICATION(SndRsp, rspout);
      }
      catch(SndRspException &e)
      {
         auto data = new SndRspExceptionData();
         data->rsp = amrs;
         data->err = e;
         SEND_TO_APPLICATION(SndReqError, data);
         if (rspout != nullptr)
            delete rspout;
      }
   }
}

Void TranslationThread::onRcvdReq(EThreadMessage &msg)
{
   static EString __method__ = __METHOD_NAME__;
   ReqInPtr ri = static_cast<ReqInPtr>(msg.getVoidPtr());
   AppMsgReqPtr req = nullptr;
   RcvdHeartbeatReqDataPtr hb = nullptr;

   // statistics will be handled by the TranslationThread

   try
   {
      if (xlator_.isVersionSupported(ri->version()))
      {
         switch (ri->msgType())
         {
            case PfcpHeartbeatReq:
            {
               hb = xlator_.decodeHeartbeatReq(ri);
               Configuration::logger().debug(
                  "{} - received heartbeat request local={} remote={} seqNbr={}",
                  __method__, ri->localNode()->address().getAddress(),
                  ri->remoteNode()->address().getAddress(), ri->seqNbr());
               SEND_TO_COMMUNICATION(HeartbeatReq, hb);
               break;
            }
            default:
            {
               Configuration::logger().debug("{} - received request local={} remote={} msgType={} seid={} seqNbr={}",
                  __method__, ri->localNode()->address().getAddress(), ri->remoteNode()->address().getAddress(),
                  ri->msgType(), ri->seid(), ri->seqNbr());
               req = xlator_.decodeReq(ri);
               if (req == nullptr)
                  throw RcvdReqException();
               SEND_TO_APPLICATION(RcvdReq, req);
               break;
            }
         }
      }
      else
      {
         // if the version is not supported, snd version not supported
         RspOutPtr ro = xlator_.encodeVersionNotSupportedRsp(ri);
         SEND_TO_COMMUNICATION(SndRsp, ro);
      }
      delete ri;
   }
   catch (RcvdReqException &e)
   {
      auto data = new RcvdReqExceptionData();
      data->req = ri;
      data->err = e;
      SEND_TO_COMMUNICATION(RcvdReqError, data);
      if (req != nullptr)
         delete req;
      if (hb != nullptr)
         delete hb;
   }
}

Void TranslationThread::onRcvdRsp(EThreadMessage &msg)
{
   static EString __method__ = __METHOD_NAME__;
   RspInPtr ri = static_cast<RspInPtr>(msg.getVoidPtr());
   AppMsgRspPtr rsp = nullptr;
   RcvdHeartbeatRspDataPtr hb = nullptr;

   // statistics will be handled by the TranslationThread
   try
   {
      switch (ri->msgType())
      {
         case PfcpHeartbeatRsp:
         {
            Configuration::logger().debug(
               "{} - received heartbeat response local={} remote={} seqNbr={}",
               __method__, ri->localNode()->address().getAddress(),
               ri->remoteNode()->address().getAddress(), ri->seqNbr());
            hb = xlator_.decodeHeartbeatRsp(ri);
            SEND_TO_COMMUNICATION(HeartbeatRsp, hb);
            break;
         }
         default:
         {
            Configuration::logger().debug("{} - received response local={} remote={} msgType={} seid={} seqNbr={}",
               __method__, ri->localNode()->address().getAddress(), ri->remoteNode()->address().getAddress(),
               ri->msgType(), ri->seid(), ri->seqNbr());
            rsp = xlator_.decodeRsp(ri);
            if (rsp == nullptr)
               throw RcvdRspException();
            SEND_TO_APPLICATION(RcvdRsp, rsp);
            break;
         }
      }
      delete ri;
   }
   catch (RcvdRspException &e)
   {
      auto data = new RcvdRspExceptionData();
      data->rsp = ri;
      data->err = e;
      SEND_TO_COMMUNICATION(RcvdRspError, data);
      if (rsp != nullptr)
         delete rsp;
      if (hb != nullptr)
         delete hb;
   }
}

Void TranslationThread::onSndHeartbeatReq(EThreadMessage &msg)
{
   static EString __method__ = __METHOD_NAME__;
   SndHeartbeatReqDataPtr req = static_cast<SndHeartbeatReqDataPtr>(msg.getVoidPtr());
   ReqOutPtr reqout = nullptr;
   try
   {
      Configuration::logger().debug("{} - sending heartbeat request to {}",
         __method__, req->remoteNode()->address().getAddress());
      reqout = xlator_.encodeHeartbeatReq(*req);
      SEND_TO_COMMUNICATION(SndReq, reqout);
      delete req;
   }
   catch(SndHeartbeatReqException &e)
   {
      auto data = new SndHeartbeatReqExceptionData();
      data->req = req;
      data->err = e;
      SEND_TO_COMMUNICATION(SndHeartbeatReqError, data);
      if (reqout != nullptr)
         delete reqout;
   }
}

Void TranslationThread::onSndHeartbeatRsp(EThreadMessage &msg)
{
   static EString __method__ = __METHOD_NAME__;
   SndHeartbeatRspDataPtr rsp = static_cast<SndHeartbeatRspDataPtr>(msg.getVoidPtr());
   RspOutPtr rspout = nullptr;
   try
   {
      Configuration::logger().debug("{} - sending heartbeat response to {}",
         __method__, rsp->req().remoteNode()->address().getAddress());
      rspout = xlator_.encodeHeartbeatRsp(*rsp);
      SEND_TO_COMMUNICATION(SndRsp, rspout);
      delete rsp;
   }
   catch(SndHeartbeatRspException &e)
   {
      auto data = new SndHeartbeatRspExceptionData();
      data->rsp = rsp;
      data->err = e;
      SEND_TO_COMMUNICATION(SndHeartbeatRspError, data);
      if (rspout != nullptr)
         delete rspout;
   }
}

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

BEGIN_MESSAGE_MAP(CommunicationThread, ESocket::ThreadPrivate)
   ON_MESSAGE(static_cast<UInt>(CommunicationThread::Events::SndReq), CommunicationThread::onSndReq)
   ON_MESSAGE(static_cast<UInt>(CommunicationThread::Events::SndRsp), CommunicationThread::onSndRsp)
   ON_MESSAGE(static_cast<UInt>(CommunicationThread::Events::HeartbeatReq), CommunicationThread::onHeartbeatReq)
   ON_MESSAGE(static_cast<UInt>(CommunicationThread::Events::HeartbeatRsp), CommunicationThread::onHeartbeatRsp)
   ON_MESSAGE(static_cast<UInt>(CommunicationThread::Events::SndHeartbeatReqError), CommunicationThread::onSndHeartbeatReqError)
   ON_MESSAGE(static_cast<UInt>(CommunicationThread::Events::SndHeartbeatRspError), CommunicationThread::onSndHeartbeatRspError)
   ON_MESSAGE(static_cast<UInt>(CommunicationThread::Events::RcvdReqError), CommunicationThread::onRcvdReqError)
   ON_MESSAGE(static_cast<UInt>(CommunicationThread::Events::RcvdRspError), CommunicationThread::onRcvdRspError)
   ON_MESSAGE(static_cast<UInt>(CommunicationThread::Events::ReqTimeout), CommunicationThread::onReqTimeout)
END_MESSAGE_MAP()

CommunicationThread::CommunicationThread()
   : trm_(Configuration::teidRangeBits()),
     caw_(0),
     crw_(rwOne_)
{
   static EString __method__ = __METHOD_NAME__;
   this_ = this;
}

CommunicationThread::~CommunicationThread()
{
   static EString __method__ = __METHOD_NAME__;
   this_ = nullptr;
}

Void CommunicationThread::onInit()
{
   static EString __method__ = __METHOD_NAME__;

   ESocket::ThreadPrivate::onInit();

   setNbrActivityWnds(Configuration::nbrActivityWnds());

   // initialize the heartbeat wnd timer
   atmr_.setInterval(Configuration::lenActivityWnd());
   atmr_.setOneShot(False);
   initTimer(atmr_);
   atmr_.start();

   // initialize the rsp wnd timer
   rsptmr_.setInterval(Configuration::maxRspWait());
   rsptmr_.setOneShot(False);
   initTimer(rsptmr_);
   rsptmr_.start();

   Configuration::logger().startup("{} - the communication thread has been started", __method__);
}

Void CommunicationThread::onQuit()
{
   static EString __method__ = __METHOD_NAME__;

   atmr_.stop();

   ESocket::ThreadPrivate::onQuit();
}

Void CommunicationThread::onTimer(EThreadEventTimer *ptimer)
{
   static EString __method__ = __METHOD_NAME__;

   if (ptimer->getId() == atmr_.getId())
   {
      Configuration::logger().debug("{} - checking LocalNode activity", __method__);
      for (auto &kv : lns_)
         kv.second->checkActivity(kv.second);
      nextActivityWnd();
   }
   else if (ptimer->getId() == rsptmr_.getId())
   {
      crw_ ^= rwToggle_;
      for (auto &kv : lns_)
         kv.second->removeOldReqs(crw_);
   }
}

Void CommunicationThread::errorHandler(EError &err, ESocket::BasePrivate *psocket)
{
   static EString __method__ = __METHOD_NAME__;

   switch (psocket->getSocketType())
   {
      case ESocket::SocketType::Udp:
      {
         ESocket::UdpPrivate *s = static_cast<ESocket::UdpPrivate*>(psocket);
         Configuration::logger().major("CommunicationThread socket exception for [{} : {}] error - {}",
            s->getLocalAddress(), s->getLocalPort(), err.what());
         break;
      }
      default:
      {
         Configuration::logger().major("CommunicationThread socket exception - unexpected socket type ({}) - {}",
            psocket->getSocketType(), err.what());
      }
   }
}

LocalNodeSPtr CommunicationThread::createLocalNode(ESocket::Address &addr)
{
   static EString __method__ = __METHOD_NAME__;

   LocalNodeSPtr ln = std::make_shared<LocalNode>();

   ln->setAddress(addr);
   ln->setStartTime();

   ln->socket().setLocalNode(ln);
   ln->socket().bind(addr);

   lns_.insert(std::make_pair(ln->ipAddress(),ln));

   return ln;
}

Void CommunicationThread::releaseLocalNodes()
{
   auto entry = lns_.begin();
   while (entry != lns_.end())
   {
      entry->second->socket().disconnect();
      entry = lns_.erase(entry);
   }
}

Void CommunicationThread::setNbrActivityWnds(size_t nbr)
{
   static EString __method__ = __METHOD_NAME__;

   caw_ = 0;

   for (auto &kv : lns_)
      kv.second->setNbrActivityWnds(nbr);
}

Void CommunicationThread::nextActivityWnd()
{
   static EString __method__ = __METHOD_NAME__;

   caw_++;
   if (caw_ >= Configuration::nbrActivityWnds())
      caw_ = 0;
   
   ERDLock lck(lnslck_);
   for (auto &kv : lns_)
      kv.second->nextActivityWnd(caw_);
}

Void CommunicationThread::onSndReq(EThreadMessage &msg)
{
   static EString __method__ = __METHOD_NAME__;

   ReqOutPtr ro = static_cast<ReqOutPtr>(msg.getVoidPtr());
   ro->localNode()->sndInitialReq(ro);
}

Void CommunicationThread::onSndRsp(EThreadMessage &msg)
{
   static EString __method__ = __METHOD_NAME__;

   RspOutPtr ro = static_cast<RspOutPtr>(msg.getVoidPtr());
   ro->localNode()->sndRsp(ro);
}

Void CommunicationThread::onHeartbeatReq(EThreadMessage &msg)
{
   static EString __method__ = __METHOD_NAME__;
   RcvdHeartbeatReqDataPtr hbrq = static_cast<RcvdHeartbeatReqDataPtr>(msg.getVoidPtr());

   Configuration::logger().debug("{} - RcvdHeartbeatReqData msgType={} seqNbr={} seid={}",
      __method__, hbrq->req()->msgType(), hbrq->req()->seqNbr(), hbrq->req()->seid());
   
   // if remote has restarted, snd notification to application thread
   if (hbrq->req()->remoteNode()->startTime() != hbrq->startTime())
   {
      RemoteNodeSPtr *p = new RemoteNodeSPtr();
      (*p) = hbrq->req()->remoteNode();
      SEND_TO_APPLICATION(RemoteNodeRestart, p);
   }

   // snd the rsp
   SndHeartbeatRspDataPtr hbrs = new SndHeartbeatRspData(hbrq->req());
   SEND_TO_TRANSLATION(SndHeartbeatRsp, hbrs);

   // delete the heartbeat req object
   delete hbrq;
}

Void CommunicationThread::onHeartbeatRsp(EThreadMessage &msg)
{
   static EString __method__ = __METHOD_NAME__;
   RcvdHeartbeatRspDataPtr hbrs = static_cast<RcvdHeartbeatRspDataPtr>(msg.getVoidPtr());

   // if remote has restarted, snd notification to application thread
   if (hbrs->req().remoteNode()->startTime() != hbrs->startTime())
   {
      RemoteNodeSPtr *p = new RemoteNodeSPtr();
      (*p) = hbrs->req().remoteNode();
      SEND_TO_APPLICATION(RemoteNodeRestart, p);
   }

   // delete the heartbeat rsp object
   delete hbrs;   
}

Void CommunicationThread::onSndHeartbeatReqError(EThreadMessage &msg)
{
   static EString __method__ = __METHOD_NAME__;
   SndHeartbeatReqExceptionDataPtr hbrq = static_cast<SndHeartbeatReqExceptionDataPtr>(msg.getVoidPtr());

   if (hbrq != nullptr)
   {
      Configuration::logger().major(
         "{} - unable to construct heartbeat request message - {}",
         __method__, hbrq->err.what());

      if (hbrq->req)
         delete hbrq->req;
      delete hbrq;
   }
   else
   {
      Configuration::logger().major(
         "{} - SndHeartbeatReqExceptionDataPtr is null", __method__
      );
   }
}

Void CommunicationThread::onSndHeartbeatRspError(EThreadMessage &msg)
{
   static EString __method__ = __METHOD_NAME__;
   SndHeartbeatRspExceptionDataPtr hbrs = static_cast<SndHeartbeatRspExceptionDataPtr>(msg.getVoidPtr());

   if (hbrs != nullptr)
   {
      Configuration::logger().major(
         "{} - unable to construct heartbeat response message - {}",
         __method__, hbrs->err.what());
      if (hbrs->rsp != nullptr)
         delete hbrs->rsp;
      delete hbrs;
   }
   else
   {
      Configuration::logger().major(
         "{} - SndHeartbeatRspExceptionDataPtr is null", __method__
      );
   }
}

Void CommunicationThread::onRcvdReqError(EThreadMessage &msg)
{
   static EString __method__ = __METHOD_NAME__;
   RcvdReqExceptionDataPtr data = static_cast<RcvdReqExceptionDataPtr>(msg.getVoidPtr());

   if (data != nullptr)
   {
      if (data->req != nullptr)
      {
         // remove the RcvdReq from the collection
         data->req->remoteNode()->delRcvdReq(data->req->seqNbr());

         // log the error
         Configuration::logger().major(
            "{} - unable to decode request message - {} - "
            " discarding req local={} remote={} seid={} msgType={} seqNbr={} version={} msgLen={}",
            __method__, data->err.what(), data->req->localNode()->ipAddress().address(),
            data->req->remoteNode()->ipAddress().address(), data->req->seid(), data->req->msgType(),
            data->req->seqNbr(), data->req->version(), data->req->len());

         // delete ReqInPtr
         delete data->req;
      }
      else
      {
         Configuration::logger().major(
            "{}} - RcvdReqExceptionDataPtr ReqInPtr is null - {}",
            __method__, data->err.what());
      }
      delete data;
   }
   else
   {
      Configuration::logger().major(
         "{} - RcvdReqExceptionDataPtr is null", __method__
      );
   }
}

Void CommunicationThread::onRcvdRspError(EThreadMessage &msg)
{
   static EString __method__ = __METHOD_NAME__;
   RcvdRspExceptionDataPtr data = static_cast<RcvdRspExceptionDataPtr>(msg.getVoidPtr());

   if (data != nullptr)
   {
      if (data->rsp != nullptr)
      {
         // clear the response window for the request that this response is associated with
         data->rsp->localNode()->setRqstOutRespWnd(data->rsp->seqNbr(), 0);

         // log the error
         Configuration::logger().major(
            "{} - unable to decode response message - {} - "
            " discarding rsp src={} dst={} seid={} msgType={} seqNbr={} version={} msgLen={}",
            __method__, data->err.what(), data->rsp->localNode()->ipAddress().address(),
            data->rsp->remoteNode()->ipAddress().address(), data->rsp->seid(), data->rsp->msgType(),
            data->rsp->seqNbr(), data->rsp->version(), data->rsp->len());

         // delete RspInPtr
         delete data->rsp;
      }
      else
      {
         Configuration::logger().major(
            "{} - RcvdRspExceptionDataPtr RspInPtr is null - {}",
            __method__, data->err.what());
      }
      delete data;
   }
   else
   {
      Configuration::logger().major(
         "{} - RcvdRspExceptionDataPtr is null", __method__
      );
   }
}

Void CommunicationThread::onReqTimeout(EThreadMessage &msg)
{
   static EString __method__ = __METHOD_NAME__;
   ReqOutPtr ro = static_cast<ReqOutPtr>(msg.getVoidPtr());
   if (ro && !ro->localNode()->onReqOutTimeout(ro))
      delete ro;
}

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

} // namespace PFCP