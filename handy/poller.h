#pragma once
#include <poll.h>
#include <assert.h>
#include <map>
#include <atomic>
#include <sys/time.h>
#include <sys/types.h>
#include <map>
#include <string.h>


namespace handy {

const int kMaxEvents = 2000;   //最大事件
const int kReadEvent = POLLIN;  //有数据可读
const int kWriteEvent = POLLOUT; //写数据不会导致阻塞

struct PollerBase: private noncopyable {
    int64_t id_;
    int lastActive_;
    PollerBase(): lastActive_(-1) { static std::atomic<int64_t> id(0); id_ = ++id; }
    virtual void addChannel(Channel* ch) = 0;
    virtual void removeChannel(Channel* ch) = 0;
    virtual void updateChannel(Channel* ch) = 0;
    virtual void loop_once(int waitMs) = 0;
    virtual ~PollerBase(){};
};

PollerBase* createPoller();

}
