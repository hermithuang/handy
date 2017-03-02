#pragma once
#include <errno.h>
#include <string.h>
#include <stdarg.h>
#include "util.h"

namespace handy {

inline const char* errstr(){ return strerror(errno); }

struct Status {
    // Create a success status.
    Status() : state_(NULL) { } //空构造
    Status(int code, const char* msg); //
    Status(int code, const std::string& msg): Status(code, msg.c_str()) {}
    ~Status() { delete[] state_; }  //处理指针指向的分配空间

    // Copy the specified status.
    Status(const Status& s) { state_ = copyState(s.state_); }
    void operator=(const Status& s) { delete[] state_; state_ = copyState(s.state_); }
    Status(Status&& s) { state_ = s.state_; s.state_ = NULL; }
    void operator = (Status&& s) { delete[] state_; state_ = s.state_; s.state_ = NULL; }  //右指引用，就是干掉当前，move,
    //右值引用是用来支持转移语义的。转移语义可以将资源 ( 堆，系统对象等 ) 从一个对象转移到另一个对象,
    //这样能够减少不必要的临时对象的创建、拷贝以及销毁，能够大幅度提高 C++ 应用程序的性能
    static Status fromSystem() { return Status(errno, strerror(errno)); }
    static Status fromSystem(int err) { return Status(err, strerror(err)); }
    static Status fromFormat(int code, const char* fmt, ...);  //不定参数个数，自己定义一套错误
    static Status ioError(const std::string& op, const std::string& name) {
        return Status::fromFormat(errno, "%s %s %s", op.c_str(), name.c_str(), errstr());
    }

    //state_ +4开始4位是code，+8开始是msg
    int code() { return state_ ? *(int32_t*)(state_+4) : 0; }
    const char* msg() { return state_ ? state_ + 8 : ""; }
    bool ok() { return code() == 0; }
    std::string toString() { return util::format("%d %s", code(), msg()); }
private:
    //    state_[0..3] == length of message ！！！
    //    state_[4..7]    == code ！！！
    //    state_[8..]  == message ！！！
    const char* state_;
    const char* copyState(const char* state);
};

inline const char* Status::copyState(const char* state) {  //新分配一块空间，把state复制下来
    if (state == NULL) {
        return state;
    }
    uint32_t size = *(uint32_t*)state;
    char* res = new char[size];
    memcpy(res, state, size);
    return res;
}

inline Status::Status(int code, const char* msg) {  //非常重要，拼装 前四位存state_大小
    uint32_t sz = strlen(msg) + 8;
    char* p = new char[sz];
    state_ = p;
    *(uint32_t*)p= sz;
    *(int32_t*)(p+4) = code;
    memcpy(p+8, msg, sz-8);
}

inline Status Status::fromFormat(int code, const char* fmt, ...) { //当我们无法列出传递函数的所有实参的类型和数目时,可以用省略号指定参数表
    va_list ap;
    va_start(ap, fmt);   //宏，用fmt初始化ap
    uint32_t size  = 8 + vsnprintf(NULL, 0, fmt, ap) + 1; //NULL for store, size = len code + len msg + 参数长度
    va_end(ap);   //宏
    Status r; // vsnprintf(NULL, 0, fmt, ap)返回参数列表长度
    r.state_ = new char[size];
    *(uint32_t*)r.state_ = size;
    *(int32_t*)(r.state_+4) = code;
    va_start(ap, fmt);
    vsnprintf((char*)r.state_+8, size - 8, fmt, ap);  //从+8开始写，把msg写进去
    va_end(ap);
    return r;
}

}
