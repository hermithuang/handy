#pragma  once
#include <string>
#include <stdio.h>
#include <atomic>
#include "util.h"

#ifdef NDEBUG    //开了debug   后边是宏，用了很多内部define FILE LINE func VA_ARGS
#define hlog(level, ...) \
    do { \
        if (level<=Logger::getLogger().getLogLevel()) { \
            Logger::getLogger().logv(level, __FILE__, __LINE__, __func__, __VA_ARGS__); \
        } \
    } while(0)
#else //不开debug
#define hlog(level, ...) \
    do { \
        if (level<=Logger::getLogger().getLogLevel()) { \
            snprintf(0, 0, __VA_ARGS__); \
            Logger::getLogger().logv(level, __FILE__, __LINE__, __func__, __VA_ARGS__); \
        } \
    } while(0)

#endif

#define trace(...) hlog(Logger::LTRACE, __VA_ARGS__)
#define debug(...) hlog(Logger::LDEBUG, __VA_ARGS__)
#define info(...) hlog(Logger::LINFO, __VA_ARGS__)
#define warn(...) hlog(Logger::LWARN, __VA_ARGS__)
#define error(...) hlog(Logger::LERROR, __VA_ARGS__)
#define fatal(...) hlog(Logger::LFATAL, __VA_ARGS__)
#define fatalif(b, ...) do { if((b)) { hlog(Logger::LFATAL, __VA_ARGS__); } } while (0)
#define check(b, ...) do { if((b)) { hlog(Logger::LFATAL, __VA_ARGS__); } } while (0)
#define exitif(b, ...) do { if ((b)) { hlog(Logger::LERROR, __VA_ARGS__); _exit(1); }} while(0)

#define setloglevel(l) Logger::getLogger().setLogLevel(l)
#define setlogfile(n) Logger::getLogger().setFileName(n)

namespace handy {

struct Logger: private noncopyable {   //无法拷贝，继承自一个自己定义的结构noncopyable
  //这个属性好像boost里有
    enum LogLevel{LFATAL=0, LERROR, LUERR, LWARN, LINFO, LDEBUG, LTRACE, LALL};  //定义了若干个级别
    Logger();
    ~Logger();
    void logv(int level, const char* file, int line, const char* func, const char* fmt ...);

    void setFileName(const std::string& filename);  //设置日至名
    void setLogLevel(const std::string& level);       //设置级别
    void setLogLevel(LogLevel level) { level_ = std::min(LALL, std::max(LFATAL, level)); }  //设置级别用enum

    LogLevel getLogLevel() { return level_; }  //int型
    const char* getLogLevelStr() { return levelStrs_[level_]; } //返回字符串
    int getFd() { return fd_; } //估计是文件描述符

    void adjustLogLevel(int adjust) { setLogLevel(LogLevel(level_+adjust)); } //改变级别
    void setRotateInterval(long rotateInterval) { rotateInterval_ = rotateInterval; } //设置刷新周期
    static Logger& getLogger(); //获得Logger
private:
    void maybeRotate();
    static const char* levelStrs_[LALL+1];
    int fd_;
    LogLevel level_;
    long lastRotate_;
    std::atomic<int64_t> realRotate_; //若一个线程写入原子对象，同时另一线程从它读取，则行为良好定义
    long rotateInterval_;
    std::string filename_;
};

}
