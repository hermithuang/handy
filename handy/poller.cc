#include "event_base.h"
#include "logging.h"
#include "util.h"
#include <fcntl.h>
#include "poller.h"
#include "conn.h"

#ifdef OS_LINUX     //这些是跨平台的编译时写法
#include <sys/epoll.h>
#elif defined(OS_MACOSX)
#include <sys/event.h>
#else
#error "platform unsupported"
#endif


namespace handy {

#ifdef OS_LINUX

struct PollerEpoll : public PollerBase{
    int fd_; //一个fd
    std::set<Channel*> liveChannels_;   //一个channels的set
    //for epoll selected active events
    struct epoll_event activeEvs_[kMaxEvents];  //active 事件数组
    PollerEpoll();
    ~PollerEpoll();
    void addChannel(Channel* ch) override;    //说明是从父类重写的
    void removeChannel(Channel* ch) override;
    void updateChannel(Channel* ch) override;
    void loop_once(int waitMs) override;
};

PollerBase* createPoller() { return new PollerEpoll(); } //新建

PollerEpoll::PollerEpoll(){
    fd_ = epoll_create1(EPOLL_CLOEXEC);  //拿到一个epoll的描述符，execl时关闭
    fatalif(fd_<0, "epoll_create error %d %s", errno, strerror(errno));
    info("poller epoll %d created", fd_);
}

PollerEpoll::~PollerEpoll() {
    info("destroying poller %d", fd_);
    while (liveChannels_.size()) {
        (*liveChannels_.begin())->close();
    }
    ::close(fd_);
    info("poller %d destroyed", fd_);
}

void PollerEpoll::addChannel(Channel* ch) {
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev)); //置0
    ev.events = ch->events(); //设定epoll的event
    ev.data.ptr = ch; //设定epoll的data
    trace("adding channel %lld fd %d events %d epoll %d", (long long)ch->id(), ch->fd(), ev.events, fd_);
    int r = epoll_ctl(fd_, EPOLL_CTL_ADD, ch->fd(), &ev);//绑定注册到内核
    fatalif(r, "epoll_ctl add failed %d %s", errno, strerror(errno));
    liveChannels_.insert(ch); //加到LiveChannels里去
}

void PollerEpoll::updateChannel(Channel* ch) {
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = ch->events();
    ev.data.ptr = ch;
    trace("modifying channel %lld fd %d events read %d write %d epoll %d",
          (long long)ch->id(), ch->fd(), ev.events & POLLIN, ev.events & POLLOUT, fd_);
    int r = epoll_ctl(fd_, EPOLL_CTL_MOD, ch->fd(), &ev); //改监控数据
    fatalif(r, "epoll_ctl mod failed %d %s", errno, strerror(errno));
}

void PollerEpoll::removeChannel(Channel* ch) {
    trace("deleting channel %lld fd %d epoll %d", (long long)ch->id(), ch->fd(), fd_);
    liveChannels_.erase(ch);
    for (int i = lastActive_; i >= 0; i --) {
        if (ch == activeEvs_[i].data.ptr) {    //从activeEvs_里找到频道数据置空
            activeEvs_[i].data.ptr = NULL;
            break;
        }
    }
}

void PollerEpoll::loop_once(int waitMs) { //这个是EventBase的loop里调用的
    int64_t ticks = util::timeMilli();
    lastActive_ = epoll_wait(fd_, activeEvs_, kMaxEvents, waitMs); //等待最新准备好的数目
    int64_t used = util::timeMilli()-ticks;  //耗时多少ms
    trace("epoll wait %d return %d errno %d used %lld millsecond",
          waitMs, lastActive_, errno, (long long)used);
    fatalif(lastActive_ == -1 && errno != EINTR, "epoll return error %d %s", errno, strerror(errno));
    //没等到就报fatal
    while (--lastActive_ >= 0) { //一个个处理
        int i = lastActive_;
        Channel* ch = (Channel*)activeEvs_[i].data.ptr; //复制数据
        int events = activeEvs_[i].events; //复制事件
        if (ch) {
            if (events & (kReadEvent | POLLERR)) {   //读事件
                trace("channel %lld fd %d handle read", (long long)ch->id(), ch->fd());
                ch->handleRead();
            } else if (events & kWriteEvent) {   //写事件
                trace("channel %lld fd %d handle write", (long long)ch->id(), ch->fd());
                ch->handleWrite();
            } else {
                fatal("unexpected poller events");
            }
        }
    }
}
//mac的就不读了
#elif defined(OS_MACOSX)

struct PollerKqueue : public PollerBase {
    int fd_;
    std::set<Channel*> liveChannels_;
    //for epoll selected active events
    struct kevent activeEvs_[kMaxEvents];
    PollerKqueue();
    ~PollerKqueue();
    void addChannel(Channel* ch) override;
    void removeChannel(Channel* ch) override;
    void updateChannel(Channel* ch) override;
    void loop_once(int waitMs) override;
};

PollerBase* createPoller() { return new PollerKqueue(); }

PollerKqueue::PollerKqueue(){
    fd_ = kqueue();
    fatalif(fd_ <0, "kqueue error %d %s", errno, strerror(errno));
    info("poller kqueue %d created", fd_);
}

PollerKqueue::~PollerKqueue() {
    info("destroying poller %d", fd_);
    while (liveChannels_.size()) {
        (*liveChannels_.begin())->close();
    }
    ::close(fd_);
    info("poller %d destroyed", fd_);
}

void PollerKqueue::addChannel(Channel* ch) {
    struct timespec now;
    now.tv_nsec = 0;
    now.tv_sec = 0;
    struct kevent ev[2];
    int n = 0;
    if (ch->readEnabled()) {
        EV_SET(&ev[n++], ch->fd(), EVFILT_READ, EV_ADD|EV_ENABLE, 0, 0, ch);
    }
    if (ch->writeEnabled()) {
        EV_SET(&ev[n++], ch->fd(), EVFILT_WRITE, EV_ADD|EV_ENABLE, 0, 0, ch);
    }
    trace("adding channel %lld fd %d events read %d write %d  epoll %d",
          (long long)ch->id(), ch->fd(), ch->events() & POLLIN, ch->events() & POLLOUT, fd_);
    int r = kevent(fd_, ev, n, NULL, 0, &now);
    fatalif(r, "kevent add failed %d %s", errno, strerror(errno));
    liveChannels_.insert(ch);
}

void PollerKqueue::updateChannel(Channel* ch) {
    struct timespec now;
    now.tv_nsec = 0;
    now.tv_sec = 0;
    struct kevent ev[2];
    int n = 0;
    if (ch->readEnabled()) {
        EV_SET(&ev[n++], ch->fd(), EVFILT_READ, EV_ADD|EV_ENABLE, 0, 0, ch);
    } else {
        EV_SET(&ev[n++], ch->fd(), EVFILT_READ, EV_DELETE, 0, 0, ch);
    }
    if (ch->writeEnabled()) {
        EV_SET(&ev[n++], ch->fd(), EVFILT_WRITE, EV_ADD|EV_ENABLE, 0, 0, ch);
    } else {
        EV_SET(&ev[n++], ch->fd(), EVFILT_WRITE, EV_DELETE, 0, 0, ch);
    }
    trace("modifying channel %lld fd %d events read %d write %d epoll %d",
          (long long)ch->id(), ch->fd(), ch->events() & POLLIN, ch->events() & POLLOUT, fd_);
    int r = kevent(fd_, ev, n, NULL, 0, &now);
    fatalif(r, "kevent mod failed %d %s", errno, strerror(errno));
}

void PollerKqueue::removeChannel(Channel* ch) {
    trace("deleting channel %lld fd %d epoll %d", (long long)ch->id(), ch->fd(), fd_);
    liveChannels_.erase(ch);
    // remove channel if in ready stat
    for (int i = lastActive_; i >= 0; i --) {
        if (ch == activeEvs_[i].udata) {
            activeEvs_[i].udata = NULL;
            break;
        }
    }
}

void PollerKqueue::loop_once(int waitMs) {
    struct timespec timeout;
    timeout.tv_sec = waitMs / 1000;
    timeout.tv_nsec = (waitMs % 1000) * 1000 * 1000;
    long ticks = util::timeMilli();
    lastActive_ = kevent(fd_, NULL, 0, activeEvs_, kMaxEvents, &timeout);
    trace("kevent wait %d return %d errno %d used %lld millsecond",
          waitMs, lastActive_, errno, util::timeMilli()-ticks);
    fatalif(lastActive_ == -1 && errno != EINTR, "kevent return error %d %s", errno, strerror(errno));
    while (--lastActive_ >= 0) {
        int i = lastActive_;
        Channel* ch = (Channel*)activeEvs_[i].udata;
        struct kevent& ke = activeEvs_[i];
        if (ch) {
            // only handle write if read and write are enabled
            if (!(ke.flags & EV_EOF) && ch->writeEnabled()) {
                trace("channel %lld fd %d handle write", (long long)ch->id(), ch->fd());
                ch->handleWrite();
            } else if ((ke.flags & EV_EOF) || ch->readEnabled()) {
                trace("channel %lld fd %d handle read", (long long)ch->id(), ch->fd());
                ch->handleRead();
            } else {
                fatal("unexpected epoll events %d", ch->events());
            }
        }
    }
}

#endif
}
