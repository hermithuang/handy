#pragma once
#include "event_base.h"

namespace handy {

//Tcp连接，使用引用计数
    struct TcpConn: public std::enable_shared_from_this<TcpConn>, private noncopyable {
        //Tcp连接的个状态
        enum State { Invalid=1, Handshaking, Connected, Closed, Failed, };
        //Tcp构造函数，实际可用的连接应当通过createConnection创建
        TcpConn();
        virtual ~TcpConn();
        //可传入连接类型，返回智能指针
        //两种建立连接的方式，connect或者attach一到一个fd上
        template<class C=TcpConn> static TcpConnPtr createConnection(EventBase* base, const std::string& host, short port,
                                   int timeout=0, const std::string& localip="") {
            TcpConnPtr con(new C); con->connect(base, host, port, timeout, localip); return con;
        }
        template<class C=TcpConn> static TcpConnPtr createConnection(EventBase* base, int fd, Ip4Addr local, Ip4Addr peer) {
            TcpConnPtr con(new C); con->attach(base, fd, local, peer); return con;
        }

        bool isClient() { return destPort_ > 0; }
        //automatically managed context. allocated when first used, deleted when destruct
        template<class T> T& context() { return ctx_.context<T>(); }

        EventBase* getBase() { return base_; }  //获得事件处理器
        State getState() { return state_; }    //获得状态
        //TcpConn的输入输出缓冲区
        Buffer& getInput() { return input_; }   //获得输入缓冲区
        Buffer& getOutput() { return output_; } //获得输出缓冲区

        Channel* getChannel() { return channel_; }  //获得Channel
        bool writable() { return channel_ ? channel_->writeEnabled(): false; }

        //发送数据
        void sendOutput() { send(output_); }
        void send(Buffer& msg);
        void send(const char* buf, size_t len);
        void send(const std::string& s) { send(s.data(), s.size()); }
        void send(const char* s) { send(s, strlen(s)); }

        //数据到达时回调
        void onRead(const TcpCallBack& cb) { assert(!readcb_); readcb_ = cb; };
        //当tcp缓冲区可写时回调
        void onWritable(const TcpCallBack& cb) { writablecb_ = cb;}
        //tcp状态改变时回调
        void onState(const TcpCallBack& cb) { statecb_ = cb; }
        //tcp空闲回调
        void addIdleCB(int idle, const TcpCallBack& cb);  //TCP空闲回调

        //消息回调，此回调与onRead回调冲突，只能够调用一个
        //codec所有权交给onMsg
        void onMsg(CodecBase* codec, const MsgCallBack& cb);
        //发送消息
        void sendMsg(Slice msg);

        //conn会在下个事件周期进行处理
        void close();
        //设置重连时间间隔，-1: 不重连，0:立即重连，其它：等待毫秒数，未设置不重连
        void setReconnectInterval(int milli) { reconnectInterval_ = milli; }

        //!慎用。立即关闭连接，清理相关资源，可能导致该连接的引用计数变为0，从而使当前调用者引用的连接被析构
        void closeNow() { if (channel_) channel_->close(); }

        //远程地址的字符串
        std::string str() { return peer_.toString(); }

    public:
        EventBase* base_;                 //事件处理器
        Channel* channel_;                //连接频道
        Buffer input_, output_;           //输入输出buffer
        Ip4Addr local_, peer_;            //本端，对端
        State state_;                     //状态
        TcpCallBack readcb_, writablecb_, statecb_;     //读，写，状态的回调函数
        std::list<IdleId> idleIds_;       //what's this
        TimerId timeoutId_;               //定时器
        AutoContext ctx_, internalCtx_;   //一些自动生成的内容
        std::string destHost_, localIp_;  //字符串，目标主机和本地IP
        int destPort_, connectTimeout_, reconnectInterval_; //目标端口，连接超时，重连间隔
        int64_t connectedTime_;           //已连接时间
        std::unique_ptr<CodecBase> codec_;    //解码器指针
        void handleRead(const TcpConnPtr& con); //处理读事件
        void handleWrite(const TcpConnPtr& con);  //处理写事件
        ssize_t isend(const char* buf, size_t len); //发送了多少内容，并发送
        void cleanup(const TcpConnPtr& con);  //清除连接
        void connect(EventBase* base, const std::string& host, short port, int timeout, const std::string& localip);  //连接，配套事件处理器
        void reconnect(); //重联
        void attach(EventBase* base, int fd, Ip4Addr local, Ip4Addr peer); //把事件处理器加到连接
        virtual int readImp(int fd, void* buf, size_t bytes) { return ::read(fd, buf, bytes); }  //读写
        virtual int writeImp(int fd, const void* buf, size_t bytes) { return ::write(fd, buf, bytes); }
        virtual int handleHandshake(const TcpConnPtr& con);   //处理握手
    };

//Tcp服务器
    struct TcpServer: private noncopyable {
        TcpServer(EventBases* bases); //方便指针的使用？
        //return 0 on sucess, errno on error
        int bind(const std::string& host, short port, bool reusePort=false);   //绑在端口上
        static TcpServerPtr startServer(EventBases* bases, const std::string& host, short port, bool reusePort=false);
        ~TcpServer() { delete listen_channel_; } //tcpserver析沟时只需要把频道删掉
        Ip4Addr getAddr() { return addr_; }
        EventBase* getBase() { return base_; }
        void onConnCreate(const std::function<TcpConnPtr()>& cb) { createcb_ = cb; }
        void onConnState(const TcpCallBack& cb) { statecb_ = cb; }
        void onConnRead(const TcpCallBack& cb) { readcb_ = cb; assert(!msgcb_); }
        // 消息处理与Read回调冲突，只能调用一个
        void onConnMsg(CodecBase* codec, const MsgCallBack& cb) { codec_.reset(codec); msgcb_ = cb; assert(!readcb_); }
    private:
        EventBase* base_;  //事件相关处理
        EventBases* bases_; //EventBase的基类
        Ip4Addr addr_;  //地址
        Channel* listen_channel_; //channel
        TcpCallBack statecb_, readcb_;  //状态改变回调函数等
        MsgCallBack msgcb_; //消息到了的回调函数
        std::function<TcpConnPtr()> createcb_;  //createcb_指向一个创建TcpConn的构造函数
        std::unique_ptr<CodecBase> codec_; //一个解码器
        void handleAccept(); //处理接受连接
    };

    typedef std::function<std::string (const TcpConnPtr&, const std::string& msg)> RetMsgCallBack;
    //半同步半异步服务器
    struct HSHA;
    typedef std::shared_ptr<HSHA> HSHAPtr;
    struct HSHA {
        static HSHAPtr startServer(EventBase* base, const std::string& host, short port, int threads);
        HSHA(int threads): threadPool_(threads) {}
        void exit() {threadPool_.exit(); threadPool_.join(); }
        void onMsg(CodecBase* codec, const RetMsgCallBack& cb);
        TcpServerPtr server_;
        ThreadPool threadPool_;
    };


}
