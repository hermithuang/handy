#include "net.h"
#include "util.h"
#include "logging.h"
#include <netinet/tcp.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <string>

using namespace std;
namespace handy {

int net::setNonBlock(int fd, bool value) { //设置block and unblock ，use bool 1 or 0
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return errno;
    }
    if (value) {
        return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
    return fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
}

int net::setReuseAddr(int fd, bool value) { //地址重用
    int flag = value;
    int len = sizeof flag;
    return setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &flag, len);
}

int net::setReusePort(int fd, bool value) { //端口重用
#ifndef SO_REUSEPORT
    fatalif(value, "SO_REUSEPORT not supported");
    return 0;
#else
    int flag = value;
    int len = sizeof flag;
    return setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &flag, len);
#endif
}

int net::setNoDelay(int fd, bool value) { //这个选项的作用就是启用或禁用 Nagle’s Algorithm
    int flag = value;
    int len = sizeof flag;
    return setsockopt(fd, SOL_SOCKET, TCP_NODELAY, &flag, len);
}
/*Nagle’s Algorithm 是为了提高带宽利用率设计的算法等待时间大概40ms，其做法是合并小的TCP 包为一个，避免了过多的小报文的 TCP 头所浪费的带宽。
如果开启了这个算法 （默认），则协议栈会累积数据直到以下两个条件之一满足的时候才真正发送出去：
1.积累的数据量到达最大的 TCP Segment Size
2.收到了一个 Ack*/

Ip4Addr::Ip4Addr(const string& host, short port) {
    memset(&addr_, 0, sizeof addr_); //置空
    addr_.sin_family = AF_INET;
    addr_.sin_port = htons(port);
    if (host.size()) {
        addr_.sin_addr = port::getHostByName(host);
    } else {
        addr_.sin_addr.s_addr = INADDR_ANY;
    }
    if (addr_.sin_addr.s_addr == INADDR_NONE){  //host有size，但无法解析
        error("cannot resove %s to ip", host.c_str());
    }
}

string Ip4Addr::toString() const { //Ip4Addr转成字符串
    uint32_t uip = addr_.sin_addr.s_addr;
    return util::format("%d.%d.%d.%d:%d",
        (uip >> 0)&0xff,
        (uip >> 8)&0xff,
        (uip >> 16)&0xff,
        (uip >> 24)&0xff,
        ntohs(addr_.sin_port));
}

string Ip4Addr::ip() const {
    uint32_t uip = addr_.sin_addr.s_addr;
    return util::format("%d.%d.%d.%d",
        (uip >> 0)&0xff,
        (uip >> 8)&0xff,
        (uip >> 16)&0xff,
        (uip >> 24)&0xff);
}

short Ip4Addr::port() const {
    return ntohs(addr_.sin_port);
}

unsigned int Ip4Addr::ipInt() const {
    return ntohl(addr_.sin_addr.s_addr);
}
bool Ip4Addr::isIpValid() const { //s_addr不是INADRR_NONE
    return addr_.sin_addr.s_addr != INADDR_NONE;
}

char* Buffer::makeRoom(size_t len) {
    if (e_ + len <= cap_) {   //最可能的，后边可以放下
    } else if (size() + len < cap_ / 2) {
        moveHead();
    } else {  //扩了一个len
        expand(len);
    }
    return end(); //可以容的下，直接返回 end_位置
}

void Buffer::expand(size_t len) {
    size_t ncap = std::max(exp_, std::max(2*cap_, size()+len));
    char* p = new char[ncap];
    std::copy(begin(), end(), p);
    e_ -= b_;
    b_ = 0;
    delete[] buf_;
    buf_ = p;
    cap_ = ncap;
}

void Buffer::copyFrom(const Buffer& b) { //把b的数据给新建
    memcpy(this, &b, sizeof b);
    if (b.buf_) {
        buf_ = new char[cap_];  //buf_是新的
        memcpy(data(), b.begin(), b.size()); //更新buf_
    }
}

Buffer& Buffer::absorb(Buffer& buf) { //吸收
    if (&buf != this) { //不是自己
        if (size() == 0) {
            char b[sizeof buf];
            memcpy(b, this, sizeof b); //当前状态保存到b
            memcpy(this, &buf, sizeof b); //使用buf更新当前
            memcpy(&buf, b, sizeof b); //好像交换了...
            std::swap(exp_, buf.exp_); //keep the origin exp_
        } else {
            append(buf.begin(), buf.size()); //把buf接在后边了
            buf.clear(); //buf被清空了
        }
    }
    return *this;
}


}
