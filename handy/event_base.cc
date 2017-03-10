#include "event_base.h"
#include "logging.h"
#include "util.h"
#include <map>
#include <string.h>
#include <fcntl.h>
#include "poller.h"
#include "conn.h"
using namespace std;

namespace handy {

namespace {

struct TimerRepeatable {     //可重复执行的任务
    int64_t at; //current timer timeout timestamp， timeout at at
    int64_t interval;   //重复间隔
    TimerId timerid;    //定时器，时间对
    Task cb;            //任务函数
};

struct IdleNode {      //空节点，就是一个连接和他的执行函数
    TcpConnPtr con_;   //连接指针
    int64_t updated_;  //更新时间
    TcpCallBack cb_;   //连接相关的执行函数 std::function<void(const TcpConnPtr&)>
};

}

struct IdleIdImp {    //IdleNode列表和迭代器组合
    IdleIdImp() {}
    typedef list<IdleNode>::iterator Iter;     //迭代器
    IdleIdImp(list<IdleNode>* lst, Iter iter): lst_(lst), iter_(iter){}
    list<IdleNode>* lst_;   //列表
    Iter iter_;             //迭代器
};

struct EventsImp {
    EventBase* base_;  //指向包含自己的父类EventBase
    PollerBase* poller_;
    std::atomic<bool> exit_;   //是否退出
    int wakeupFds_[2];    //唤醒管道fd
    int nextTimeout_;     //下次超时
    SafeQueue<Task> tasks_;   //任务函数队列

    std::map<TimerId, TimerRepeatable> timerReps_;   //定时器id对可重复任务map
    std::map<TimerId, Task> timers_;                //定时器id对任务map
    std::atomic<int64_t> timerSeq_;                 //定时器序列
    std::map<int, std::list<IdleNode>> idleConns_;  //空闲时间 idleNode list的 map
    std::set<TcpConnPtr> reconnectConns_;           //tcp连接指针 set
    bool idleEnabled;                               //启用空

    EventsImp(EventBase* base, int taskCap);        //Event和任务上限
    ~EventsImp();
    void init();                                    //初始化
    void callIdles();                               //调用空闲任务
    IdleId registerIdle(int idle, const TcpConnPtr& con, const TcpCallBack& cb); //对Tcp连接注册id和回调函数
    void unregisterIdle(const IdleId& id);          //取消注册
    void updateIdle(const IdleId& id);              //更新注册
    void handleTimeouts();                          //处理超时
    void refreshNearest(const TimerId* tid=NULL);   //刷新最近的结点
    void repeatableTimeout(TimerRepeatable* tr);    //重复任务超时

    //eventbase functions
    EventBase& exit() {exit_ = true; wakeup(); return *base_;} //置个退出标志，wakeup，返回base_
    bool exited() { return exit_; }   //检测是否需要退出
    void safeCall(Task&& task) { tasks_.push(move(task)); wakeup(); } //函数队列里加一个函数
    void loop();                      //循环
    //处理已到期的事件,waitMs表示若无当前需要处理的任务，需要等待的时间
    void loop_once(int waitMs) { poller_->loop_once(std::min(waitMs, nextTimeout_)); handleTimeouts(); }
    void wakeup() {  //往管道里写个1
        int r = write(wakeupFds_[1], "", 1);
        fatalif(r<=0, "write error wd %d %d %s", r, errno, strerror(errno));
    }

    bool cancel(TimerId timerid); //取消一个时间对应的任务
    TimerId runAt(int64_t milli, Task&& task, int64_t interval);  //task运行
};

EventBase::EventBase(int taskCapacity) {   //初始化EventBase，初始化 _imp
    imp_.reset(new EventsImp(this, taskCapacity)); //_imp里的base_指针指向this
    imp_->init();
}

EventBase::~EventBase() {}
//EventBase感觉像一个 EventsImp的封装,都是imp_干活
EventBase& EventBase::exit() { return imp_->exit(); }

bool EventBase::exited() { return imp_->exited(); }

void EventBase::safeCall(Task&& task) { imp_->safeCall(move(task)); }

void EventBase::wakeup(){ imp_->wakeup(); }

void EventBase::loop() { imp_->loop(); }

void EventBase::loop_once(int waitMs) { imp_->loop_once(waitMs); }

bool EventBase::cancel(TimerId timerid) { return imp_ && imp_->cancel(timerid); }

TimerId EventBase::runAt(int64_t milli, Task&& task, int64_t interval) {
    return imp_->runAt(milli, std::move(task), interval);
}

EventsImp::EventsImp(EventBase* base, int taskCap): //不设置超时,idle disabled
    base_(base), poller_(createPoller()), exit_(false), nextTimeout_(1<<30), tasks_(taskCap),
    timerSeq_(0), idleEnabled(false)
{
}

void EventsImp::loop() {
    while (!exit_)
        loop_once(10000);  //10000ns or ms
    timerReps_.clear(); //清除所有
    timers_.clear();
    idleConns_.clear();
    for (auto recon: reconnectConns_) { //重连的连接无法通过channel清理，因此单独清理
        recon->cleanup(recon);
    }
    loop_once(0);
}

void EventsImp::init() {
    int r = pipe(wakeupFds_);  //搞一个管道
    fatalif(r, "pipe failed %d %s", errno, strerror(errno));
    r = util::addFdFlag(wakeupFds_[0], FD_CLOEXEC); //使用execl执行的程序里，此描述符被关闭，不能再使用它
    fatalif(r, "addFdFlag failed %d %s", errno, strerror(errno));
    r = util::addFdFlag(wakeupFds_[1], FD_CLOEXEC); //使用execl执行的程序里，此描述符被关闭，不能再使用它
    fatalif(r, "addFdFlag failed %d %s", errno, strerror(errno));
    trace("wakeup pipe created %d %d", wakeupFds_[0], wakeupFds_[1]);
    Channel* ch = new Channel(base_, wakeupFds_[0], kReadEvent); //读事件,对于事件管理器管道的读入口，也就是事件管理器有可读时
    ch->onRead([=] {  //注册一个频道的读回调函数
        char buf[1024];
        int r = ch->fd() >= 0 ? ::read(ch->fd(), buf, sizeof buf) : 0;
        if (r > 0) {  //读到了东西
            Task task;
            while (tasks_.pop_wait(&task, 0)) { //事件管理器的任务函数里pop一个内容到task
                task(); //执行
            }
        } else if (r == 0) {  //没读到
            delete ch;    //删掉频道
        } else if (errno == EINTR) {
        } else {
            fatal("wakeup channel read error %d %d %s", r, errno, strerror(errno));
        }
    });
}

void EventsImp::handleTimeouts() { //如何处理定时任务
    int64_t now = util::timeMilli();
    TimerId tid { now, 1L<<62 }; //现在到永远
    while (timers_.size() && timers_.begin()->first < tid) { //在now之前需要执行的
        Task task = move(timers_.begin()->second);  //把定时器需要执行的函数拿出来
        timers_.erase(timers_.begin()); //清除任务
        task(); //执行
    }
    refreshNearest();
}

EventsImp::~EventsImp() {
    delete poller_; //干掉poller
    ::close(wakeupFds_[1]); //这种是外部的close函数，也就是系统的close
}

void EventsImp::callIdles() { //idleNode关联函数
    int64_t now = util::timeMilli() / 1000;
    for (auto& l: idleConns_) {
        int idle = l.first; //int，从后边看是个事件相关的内容
        auto lst = l.second;  //std::list<IdleNode>
        while(lst.size()) {
            IdleNode& node = lst.front();
            if (node.updated_ + idle > now) { //更新时间 > 当前 - 空闲时间（当前不用更新就break）
                break;
            }
            node.updated_ = now; //更新这些结点的更新时间
            lst.splice(lst.end(), lst, lst.begin()); //把第一个元素挪到尾部
            node.cb_(node.con_); //node.cb_是std::function<void(const TcpConnPtr&)>，node_con_是TcpConnPtr，这是一个调用，不是赋值。
            //上一句是call，把需要执行的结点函数都执行掉。
        }
    }
}

IdleId EventsImp::registerIdle(int idle, const TcpConnPtr& con, const TcpCallBack& cb) {
    if (!idleEnabled) { //idleEnabled为假
        base_->runAfter(1000, [this] { callIdles(); }, 1000); //1秒后执行，每1秒执行一次
        idleEnabled = true;
    }
    auto& lst = idleConns_[idle]; //std::map<int, std::list<IdleNode>>
    lst.push_back(IdleNode {con, util::timeMilli()/1000, move(cb) }); //把TcpConnPtr和其回调函数加进空间list
    //更新事件为当前
    trace("register idle");
    return IdleId(new IdleIdImp(&lst, --lst.end())); //IdleId是指向IdleIdImp的 ptr //IdleIdImp是list<IdleNode>和他的迭代器
}

void EventsImp::unregisterIdle(const IdleId& id) { //删掉一个IdleId
    trace("unregister idle");
    id->lst_->erase(id->iter_);
}

void EventsImp::updateIdle(const IdleId& id) { //更新一个IdleId的时间，并把他挪到最后
    trace("update idle");
    id->iter_->updated_ = util::timeMilli() / 1000;
    id->lst_->splice(id->lst_->end(), *id->lst_, id->iter_);
}

void EventsImp::refreshNearest(const TimerId* tid){ //tid没用上
    if (timers_.empty()) {
        nextTimeout_ = 1 << 30;   //定时器数组是空的，就永远不超时
    } else {
        const TimerId &t = timers_.begin()->first;
        nextTimeout_ = t.first - util::timeMilli();
        nextTimeout_ = nextTimeout_ < 0 ? 0 : nextTimeout_;
    }
}

void EventsImp::repeatableTimeout(TimerRepeatable* tr) {
    tr->at += tr->interval;  //设置下一次超时
    tr->timerid = { tr->at, ++timerSeq_ };  //定时器id由下次超时时间和时间序列构成
    timers_[tr->timerid] = [this, tr] { repeatableTimeout(tr); }; //定时器id对应的再设置函数
    refreshNearest(&tr->timerid); //把这个已经执行的挪到最后去
    tr->cb(); //执行定时器回调函数
}

TimerId EventsImp::runAt(int64_t milli, Task&& task, int64_t interval) {
    if (exit_) {
        return TimerId();
    }
    if (interval) { //有执行间隔
        TimerId tid { -milli, ++timerSeq_};
        TimerRepeatable& rtr = timerReps_[tid]; //重复任务的数组
        rtr = { milli, interval, { milli, ++timerSeq_ }, move(task) }; //设置TimerRepeatable和任务
        TimerRepeatable* tr = &rtr;
        timers_[tr->timerid] = [this, tr] { repeatableTimeout(tr); }; //添加任务函数到std::map<TimerId, Task>
        refreshNearest(&tr->timerid);
        return tid;
    } else { //一次执行非间隔
        TimerId tid { milli, ++timerSeq_}; //当前和timerSeq_
        timers_.insert({tid, move(task)}); //直接加进去就行了
        refreshNearest(&tid);
        return tid;
    }
}

bool EventsImp::cancel(TimerId timerid) {
    if (timerid.first < 0) {  //间隔任务
        auto p = timerReps_.find(timerid); //TimeReps数组里找到TimerId
        auto ptimer = timers_.find(p->second.timerid); //timerid和任务处理函数
        if (ptimer != timers_.end()) { //找到了
            timers_.erase(ptimer);   //删掉这个定时器
        }
        timerReps_.erase(p); //删掉这个任务
        return true;
    } else { //非间隔任务
        auto p = timers_.find(timerid);
        if (p != timers_.end()) {
            timers_.erase(p); //删掉
            return true;
        }
        return false;
    }
}

void MultiBase::loop() {
    int sz = bases_.size();
    vector<thread> ths(sz -1);
    for(int i = 0; i < sz -1; i++) {
        thread t([this, i]{ bases_[i].loop();});
        ths[i].swap(t); //这个是干嘛
    }
    bases_.back().loop();
    for (int i = 0; i < sz -1; i++) {
        ths[i].join(); //主线程等待子线程结束
    }
}

Channel::Channel(EventBase* base, int fd, int events): base_(base), fd_(fd), events_(events) {
    fatalif(net::setNonBlock(fd_) < 0, "channel set non block failed");
    static atomic<int64_t> id(0);
    id_ = ++id;
    poller_ = base_->imp_->poller_;
    poller_->addChannel(this); //poller可以添加频道
}

Channel::~Channel() {
    close();
}

void Channel::enableRead(bool enable) {
    if (enable) {
        events_ |= kReadEvent;
    } else {
        events_ &= ~kReadEvent;
    }
    poller_->updateChannel(this);
}

void Channel::enableWrite(bool enable) {
    if (enable) {
        events_ |= kWriteEvent;
    } else {
        events_ &= ~kWriteEvent;
    }
    poller_->updateChannel(this);
}

void Channel::enableReadWrite(bool readable, bool writable) {
    if (readable) {
        events_ |= kReadEvent;
    } else {
        events_ &= ~kReadEvent;
    }
    if (writable) {
        events_ |= kWriteEvent;
    } else {
        events_ &= ~kWriteEvent;
    }
    poller_->updateChannel(this);
}

void Channel::close() {
    if (fd_>=0) {
        trace("close channel %ld fd %d", (long)id_, fd_);
        poller_->removeChannel(this);
        ::close(fd_);
        fd_ = -1;
        handleRead();
    }
}

bool Channel::readEnabled() { return events_ & kReadEvent; }
bool Channel::writeEnabled() { return events_ & kWriteEvent; }

void handyUnregisterIdle(EventBase* base, const IdleId& idle) {
    base->imp_->unregisterIdle(idle);
}

void handyUpdateIdle(EventBase* base, const IdleId& idle) {
    base->imp_->updateIdle(idle);
}

TcpConn::TcpConn()
:base_(NULL), channel_(NULL), state_(State::Invalid), destPort_(-1),
 connectTimeout_(0), reconnectInterval_(-1),connectedTime_(util::timeMilli())
{
}

TcpConn::~TcpConn() {
    trace("tcp destroyed %s - %s", local_.toString().c_str(), peer_.toString().c_str());
    delete channel_;
}

void TcpConn::addIdleCB(int idle, const TcpCallBack& cb) {
    if (channel_) {
        idleIds_.push_back(getBase()->imp_->registerIdle(idle, shared_from_this(), cb));
    }
}

void TcpConn::reconnect() {
    auto con = shared_from_this();
    getBase()->imp_->reconnectConns_.insert(con);
    long long interval = reconnectInterval_-(util::timeMilli()-connectedTime_);
    interval = interval>0?interval:0;
    info("reconnect interval: %d will reconnect after %lld ms", reconnectInterval_, interval);
    getBase()->runAfter(interval, [this, con]() {
        getBase()->imp_->reconnectConns_.erase(con);
        connect(getBase(), destHost_, (short)destPort_, connectTimeout_, localIp_);
    });
    delete channel_;
    channel_ = NULL;
}

}
