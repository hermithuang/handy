#include "conn.h"
#include "logging.h"
#include <poll.h>
#include <fcntl.h>
#include "poller.h"

//channel是读写的通道
using namespace std;
namespace handy {

void handyUnregisterIdle(EventBase* base, const IdleId& idle);
void handyUpdateIdle(EventBase* base, const IdleId& idle);

//把事件管理器附加到TcpConn上
void TcpConn::attach(EventBase* base, int fd, Ip4Addr local, Ip4Addr peer)
{
    fatalif((destPort_<=0 && state_ != State::Invalid) || (destPort_>=0 && state_ != State::Handshaking),
        "you should use a new TcpConn to attach. state: %d", state_); //判断下能不能attach上
    base_ = base;
    state_ = State::Handshaking;
    local_ = local;
    peer_ = peer;
    delete channel_;
    channel_ = new Channel(base, fd, kWriteEvent|kReadEvent);  //建一个channel
    trace("tcp constructed %s - %s fd: %d",
        local_.toString().c_str(),
        peer_.toString().c_str(),
        fd);
    TcpConnPtr con = shared_from_this();  //就是TcpConn con
    con->channel_->onRead([=] { con->handleRead(con); }); //设上回调函数
    con->channel_->onWrite([=] { con->handleWrite(con); }); //设上回调函数
}

void TcpConn::connect(EventBase* base, const string& host, short port, int timeout, const string& localip) {
    fatalif(state_ != State::Invalid && state_ != State::Closed && state_ != State::Failed,
            "current state is bad state to connect. state: %d", state_);
    destHost_ = host;
    destPort_ = port;
    connectTimeout_ = timeout;
    connectedTime_ = util::timeMilli();
    localIp_ = localip;
    Ip4Addr addr(host, port);
    int fd = socket(AF_INET, SOCK_STREAM, 0);  //连接
    fatalif(fd<0, "socket failed %d %s", errno, strerror(errno));
    net::setNonBlock(fd);
    int t = util::addFdFlag(fd, FD_CLOEXEC);
    fatalif(t, "addFdFlag FD_CLOEXEC failed %d %s", t, strerror(t));
    int r = 0;
    if (localip.size()) { //定义了localip
        Ip4Addr addr(localip, 0);
        r = ::bind(fd,(struct sockaddr *)&addr.getAddr(),sizeof(struct sockaddr)); //绑在localip上
        error("bind to %s failed error %d %s", addr.toString().c_str(), errno, strerror(errno));
    }
    if (r == 0) {
        r = ::connect(fd, (sockaddr *) &addr.getAddr(), sizeof(sockaddr_in)); //连接
        if (r != 0 && errno != EINPROGRESS) {
            error("connect to %s error %d %s", addr.toString().c_str(), errno, strerror(errno));
        }
    }

    sockaddr_in local;
    socklen_t alen = sizeof(local);
    if (r == 0) {
        r = getsockname(fd, (sockaddr *) &local, &alen);
        if (r < 0) {
            error("getsockname failed %d %s", errno, strerror(errno));
        }
    }
    state_ = State::Handshaking;
    attach(base, fd, Ip4Addr(local), addr);  //把事件处理器绑定在连接上
    if (timeout) {  //设置了超时
        TcpConnPtr con = shared_from_this();
        timeoutId_ = base->runAfter(timeout, [con] { //设置一个超时函数，将channel关闭
            if (con->getState() == Handshaking) { con->channel_->close(); }  //超时前还在握手，就关闭连接
        });
    }
}

void TcpConn::close() {   //连接关闭时主要是关闭传输通道
    if (channel_) {
        TcpConnPtr con = shared_from_this();
        getBase()->safeCall([con]{ if (con->channel_) con->channel_->close(); });  //定时器设置关闭channel任务
    }
}

void TcpConn::cleanup(const TcpConnPtr& con) {
    if (readcb_ && input_.size()) { //清除的时候还有数据可以读
        readcb_(con);   //读出来处理
    }
    if (state_ == State::Handshaking) {   //如果还在握手
        state_ = State::Failed;     //状态置错误
    } else {
        state_ = State::Closed;     //状态置关闭
    }
    trace("tcp closing %s - %s fd %d %d",
        local_.toString().c_str(),
        peer_.toString().c_str(),
        channel_ ? channel_->fd(): -1, errno);
    getBase()->cancel(timeoutId_);  //把定时器去掉
    if (statecb_) {  //状态转换函数设置
        statecb_(con); //调用
    }
    if (reconnectInterval_ >= 0 && !getBase()->exited()) { //reconnect
        reconnect();
        return;
    }
    for (auto& idle: idleIds_) {
        handyUnregisterIdle(getBase(), idle);
    }
    //channel may have hold TcpConnPtr, set channel_ to NULL before delete
    readcb_ = writablecb_ = statecb_ = nullptr;  //删了之后把指针全部置空
    Channel* ch = channel_;
    channel_ = NULL;
    delete ch;
}

void TcpConn::handleRead(const TcpConnPtr& con) {
    if (state_ == State::Handshaking && handleHandshake(con)) {
        return;
    }
    while(state_ == State::Connected) {
        input_.makeRoom();
        int rd = 0;
        if (channel_->fd() >= 0) {
            rd = readImp(channel_->fd(), input_.end(), input_.space()); //从end开始写入，读到一个空格
            trace("channel %lld fd %d readed %d bytes", (long long)channel_->id(), channel_->fd(), rd);
        }
        if (rd == -1 && errno == EINTR) {  //没读到就再循环
            continue;
        } else if (rd == -1 && (errno == EAGAIN || errno == EWOULDBLOCK) ) { //没东西可读
            for(auto& idle: idleIds_) {
                handyUpdateIdle(getBase(), idle);
            }
            if (readcb_ && input_.size()) {
                readcb_(con);
            }
            break;
        } else if (channel_->fd() == -1 || rd == 0 || rd == -1) {
            cleanup(con);
            break;
        } else { //rd > 0
            input_.addSize(rd);  //读缓冲区增加Size
        }
    }
}

int TcpConn::handleHandshake(const TcpConnPtr& con) { //连接处理握手
    fatalif(state_ != Handshaking, "handleHandshaking called when state_=%d", state_);
    struct pollfd pfd;
    pfd.fd = channel_->fd();
    pfd.events = POLLOUT | POLLERR;
    int r = poll(&pfd, 1, 0);
    if (r == 1 && pfd.revents == POLLOUT) {
        channel_->enableReadWrite(true, false);
        state_ = State::Connected;
        if (state_ == State::Connected) {
            connectedTime_ = util::timeMilli();
            trace("tcp connected %s - %s fd %d",
                local_.toString().c_str(), peer_.toString().c_str(), channel_->fd());
            if (statecb_) {
                statecb_(con);
            }
        }
    } else {
        trace("poll fd %d return %d revents %d", channel_->fd(), r, pfd.revents);
        cleanup(con);
        return -1;
    }
    return 0;
}

void TcpConn::handleWrite(const TcpConnPtr& con) {
    if (state_ == State::Handshaking) { //握手就处理握手
        handleHandshake(con);
    } else if (state_ == State::Connected) { //如果已经连接
        ssize_t sended = isend(output_.begin(), output_.size());
        output_.consume(sended);
        if (output_.empty() && writablecb_) {
            writablecb_(con);
        }
        if (output_.empty() && channel_->writeEnabled()) { // writablecb_ may write something
            channel_->enableWrite(false); //这个通道关闭写
        }
    } else {
        error("handle write unexpected");
    }
}

ssize_t TcpConn::isend(const char* buf, size_t len) {
    size_t sended = 0;
    while (len > sended) { //当没发送完时
        ssize_t wd = writeImp(channel_->fd(), buf + sended, len - sended); //写一些内容
        trace("channel %lld fd %d write %ld bytes", (long long)channel_->id(), channel_->fd(), wd);
        if (wd > 0) {
            sended += wd;
            continue;
        } else if (wd == -1 && errno == EINTR) {
            continue;
        } else if (wd == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (!channel_->writeEnabled()) {
                channel_->enableWrite(true);
            }
            break;
        } else {
            error("write error: channel %lld fd %d wd %ld %d %s", (long long)channel_->id(), channel_->fd(), wd, errno, strerror(errno));
            break;
        }
    }
    return sended;
}

void TcpConn::send(Buffer& buf) {
    if (channel_) {
        if (channel_->writeEnabled()) { //just full
            output_.absorb(buf);   //把buf 加进来
        }
        if (buf.size()) {
            ssize_t sended = isend(buf.begin(), buf.size());
            buf.consume(sended);
        }
        if (buf.size()) {
            output_.absorb(buf);
            if (!channel_->writeEnabled()) {
                channel_->enableWrite(true);
            }
        }
    } else {
        warn("connection %s - %s closed, but still writing %lu bytes",
            local_.toString().c_str(), peer_.toString().c_str(), buf.size());
    }
}

void TcpConn::send(const char* buf, size_t len) {
    if (channel_) {
        if (output_.empty()) {
            ssize_t sended = isend(buf, len);
            buf += sended;
            len -= sended;
        }
        if (len) {
            output_.append(buf, len);
        }
    } else {
        warn("connection %s - %s closed, but still writing %lu bytes",
            local_.toString().c_str(), peer_.toString().c_str(), len);
    }
}

void TcpConn::onMsg(CodecBase* codec, const MsgCallBack& cb) { //当有消息时
    assert(!readcb_); //需要读函数
    codec_.reset(codec);
    onRead([cb](const TcpConnPtr& con) { //把cd传递进去
        int r = 1;
        while (r) {
            Slice msg;
            r = con->codec_->tryDecode(con->getInput(), msg); //消息解了出来
            if (r < 0) {
                con->channel_->close();
                break;
            } else if (r > 0) {
                trace("a msg decoded. origin len %d msg len %ld", r, msg.size());
                cb(con, msg); //调用消息的回调函数
                con->getInput().consume(r); //消耗掉
            }
        }
    });
}

void TcpConn::sendMsg(Slice msg) {
    codec_->encode(msg, getOutput());  //加密
    sendOutput(); //发送
}

TcpServer::TcpServer(EventBases* bases):
base_(bases->allocBase()),
bases_(bases),
listen_channel_(NULL),
createcb_([]{ return TcpConnPtr(new TcpConn); }) //tcpServer包含一个事件处理器和创建TcpConn对象的的回调函数
{
}

int TcpServer::bind(const std::string &host, short port, bool reusePort) {
    addr_ = Ip4Addr(host, port);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int r = net::setReuseAddr(fd);
    fatalif(r, "set socket reuse option failed");
    r = net::setReusePort(fd, reusePort);
    fatalif(r, "set socket reuse port option failed");
    r = util::addFdFlag(fd, FD_CLOEXEC);
    fatalif(r, "addFdFlag FD_CLOEXEC failed");
    r = ::bind(fd,(struct sockaddr *)&addr_.getAddr(),sizeof(struct sockaddr)); //TcpServer绑在什么口上
    if (r) {
        close(fd);
        error("bind to %s failed %d %s", addr_.toString().c_str(), errno, strerror(errno));
        return errno;
    }
    r = listen(fd, 20);
    fatalif(r, "listen failed %d %s", errno, strerror(errno));
    info("fd %d listening at %s", fd, addr_.toString().c_str());
    listen_channel_ = new Channel(base_, fd, kReadEvent); //bind时会新建一个频道
    listen_channel_->onRead([this]{ handleAccept(); }); //当频道上有数据可读时，处理接收事件
    return 0;
}

TcpServerPtr TcpServer::startServer(EventBases* bases, const std::string& host, short port, bool reusePort) {
  //startServer也就是建个新Server
    TcpServerPtr p(new TcpServer(bases)); //新建一个Server
    int r = p->bind(host, port, reusePort);
    if (r) {
        error("bind to %s:%d failed %d %s", host.c_str(), port, errno, strerror(errno));
    }
    return r == 0 ? p : NULL;
}

void TcpServer::handleAccept() { //处理接收
    struct sockaddr_in raddr;
    socklen_t rsz = sizeof(raddr);
    int lfd = listen_channel_->fd();
    int cfd;
    while (lfd >= 0 && (cfd = accept(lfd,(struct sockaddr *)&raddr,&rsz))>=0) { //有连接上来
        sockaddr_in peer, local;
        socklen_t alen = sizeof(peer);
        int r = getpeername(cfd, (sockaddr*)&peer, &alen);
        if (r < 0) {
            error("get peer name failed %d %s", errno, strerror(errno));
            continue;
        }
        r = getsockname(cfd, (sockaddr*)&local, &alen);
        if (r < 0) {
            error("getsockname failed %d %s", errno, strerror(errno));
            continue;
        }
        r = util::addFdFlag(cfd, FD_CLOEXEC);
        fatalif(r, "addFdFlag FD_CLOEXEC failed");
        EventBase* b = bases_->allocBase(); //初始化事件处理器
        auto addcon = [=] {
            TcpConnPtr con = createcb_();
            con->attach(b, cfd, local, peer);
            if (statecb_) {
                con->onState(statecb_);
            }
            if (readcb_) {
                con->onRead(readcb_);
            }
            if (msgcb_) {
                con->onMsg(codec_->clone(), msgcb_);
            }
        };
        if (b == base_) {
            addcon(); //已经有事件处理器了，直接加上处理函数
        } else {
            b->safeCall(move(addcon)); //事件处理器调用添加事件处理函数
        }
    }
    if (lfd >= 0 && errno != EAGAIN && errno != EINTR) {
        warn("accept return %d  %d %s", cfd, errno, strerror(errno));
    }
}

HSHAPtr HSHA::startServer(EventBase* base, const std::string& host, short port, int threads) {
    HSHAPtr p = HSHAPtr(new HSHA(threads));
    p->server_ = TcpServer::startServer(base, host, port);
    return p->server_ ? p : NULL;
}

void HSHA::onMsg(CodecBase* codec, const RetMsgCallBack& cb) {
    server_->onConnMsg(codec, [this, cb](const TcpConnPtr& con, Slice msg) {
        std::string input = msg;
        threadPool_.addTask([=]{
            std::string output = cb(con, input);
            server_->getBase()->safeCall([=] {if (output.size()) con->sendMsg(output); });
        });
    });
}

}
