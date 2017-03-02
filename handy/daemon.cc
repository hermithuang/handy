#include "daemon.h"
#include <functional>
#include <utility>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <time.h>
#include <strings.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <map>
#include <string>

using namespace std;

namespace handy {

namespace {    //再来个匿名空间，本模块用，类似static

struct ExitCaller {  //退出时调用的
    ~ExitCaller() { functor_(); }
    ExitCaller(std::function<void()>&& functor): functor_(std::move(functor)) {} //不是很懂，但基本上就是把一个函数引用的引用通过std::move赋给私有成员,move之后，原来的函数定义就没有了，没有作赋值
private:
    std::function<void()> functor_; //返回void，参数空
};

}

static int writePidFile(const char *pidfile)  //做一个pidfile
{
    char str[32];
    int lfp = open(pidfile, O_WRONLY|O_CREAT|O_TRUNC, 0600);
    if (lfp < 0 || lockf(lfp, F_TLOCK, 0) < 0) {   //尝试加个锁，或者打不开，就出错
        fprintf(stderr, "Can't write Pid File: %s", pidfile);
        return -1;
    }
    ExitCaller call1([=] { close(lfp); });   //lambda [=] 以值捕获所有lambda体内的odr使用的自动变量，及以引用捕获当前对象，若它存在。就是找到lfp
    sprintf(str, "%d\n", getpid());
    ssize_t len = strlen(str);
    ssize_t ret = write(lfp, str, len);
    if (ret != len ) {
        fprintf(stderr, "Can't Write Pid File: %s", pidfile);
        return -1;
    }
    return 0;
}

int Daemon::getPidFromFile(const char *pidfile)  //拿到pid
{
    char buffer[64], *p;
    int lfp = open(pidfile, O_RDONLY, 0);
    if (lfp < 0) {
        return lfp;
    }
    ssize_t rd = read(lfp, buffer, 64);
    close(lfp);
    if (rd <= 0) {
        return -1;
    }
    buffer[63] = '\0';
    p = strchr(buffer, '\n');
    if (p != NULL)
        *p = '\0';
    return atoi(buffer);
}


int Daemon::daemonStart(const char* pidfile) {
    int pid = getPidFromFile(pidfile);
    if (pid > 0) {
        if (kill(pid, 0) == 0 || errno == EPERM) { //发个signal 0测试一下，或者没权限
            fprintf(stderr, "daemon exists, use restart\n");
            return -1;
        }
    }
    if (getppid() == 1) {  //daemon父进程是1
        fprintf(stderr, "already daemon, can't start\n");
        return -1;
    }

    pid = fork();
    if (pid < 0) {   //失败
        fprintf(stderr, "fork error: %d\n", pid);
        return -1;
    }
    if (pid > 0) {
        exit(0); // parent exit
    }
    setsid();   //建立daemon的过程，重建session id
    int r = writePidFile(pidfile);  //写个pid文件
    if (r != 0) {
        return r;
    }
    int fd =open("/dev/null", 0);
    if (fd >= 0) {
        close(0);
        dup2(fd, 0);    //把标准输入输出定向到/dev/null
        dup2(fd, 1);
        close(fd);
        string pfile = pidfile;
        static ExitCaller del([=] {
            unlink(pfile.c_str());   //删掉pidfile
        });
        return 0;
    }
    return -1;
}

int Daemon::daemonStop(const char* pidfile) {
    int pid = getPidFromFile(pidfile);
    if (pid <= 0) {
        fprintf(stderr, "%s not exists or not valid\n", pidfile);
        return -1;
    }
    int r = kill(pid, SIGQUIT);
    if (r < 0) {   //未发送成功
        fprintf(stderr, "program %d not exists\n", pid);
        return r;
    }
    for (int i = 0; i < 300; i++) {  //多试几次
        usleep(10*1000);
        r = kill(pid, SIGQUIT);
        if (r != 0) {   //未发送成功,说明进程已经退了，返回。一直发送成功，说明程序没退
            fprintf(stderr, "program %d exited\n", pid);
            unlink(pidfile);
            return 0;
        }
    }
    fprintf(stderr, "signal sended to process, but still exists after 3 seconds\n"); //3秒了还没退
    return -1;
}

int Daemon::daemonRestart(const char* pidfile) {
    int pid = getPidFromFile(pidfile);
    if (pid > 0) {
        if (kill(pid, 0) == 0) { //pid还在
            int r = daemonStop(pidfile); //停
            if (r < 0) {      //没成功
                return r;
            }
        } else if (errno == EPERM) {
            fprintf(stderr, "do not have permission to kill process: %d\n", pid);
            return -1;
        }
    } else {
        fprintf(stderr, "pid file not valid, just ignore\n");
    }
    return daemonStart(pidfile);  //没有pid，直接启动
}

void Daemon::daemonProcess(const char* cmd, const char* pidfile) { //daemon控制
    int r = 0;
    if (cmd == NULL || strcmp(cmd, "start")==0) {
        r = daemonStart(pidfile);
    } else if (strcmp(cmd, "stop")==0) {
        r = daemonStop(pidfile);
        if (r == 0) {
            exit(0);
        }
    } else if (strcmp(cmd, "restart")==0) {
        r = daemonRestart(pidfile);
    } else {
        fprintf(stderr, "ERROR: bad daemon command. exit\n");
        r = -1;
    }
    if (r) {
        //exit on error
        exit(1);
    }
}

void Daemon::changeTo(const char* argv[]) {  //子程序调用另一个程序
    int pid = getpid();
    int r = fork();
    if (r < 0) {
        fprintf(stderr, "fork error %d %s", errno, strerror(errno));
    } else if (r > 0) { //parent;
        return;
    } else { //child
        //wait parent to exit
        while(kill(pid, 0) == 0) {
            usleep(10*1000);
        }
        if (errno != ESRCH) {
            const char* msg = "kill error\n";
            ssize_t w1 = write(2, msg, strlen(msg)); //write to std err
            (void)w1; //???
            _exit(1);
        }
        execvp(argv[0], (char* const*)argv);
    }
}

namespace {     //信号处理函数map列表，绑起来
    map<int, function<void()>> handlers;
    void signal_handler(int sig) {
        handlers[sig]();
    }
}

void Signal::signal(int sig, const function<void()>& handler) {
    handlers[sig] = handler;
    ::signal(sig, signal_handler);
}

}
