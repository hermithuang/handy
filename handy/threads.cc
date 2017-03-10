#include "threads.h"
#include <assert.h>
#include <utility>
using namespace std;

namespace handy {

template class SafeQueue<Task>;

ThreadPool::ThreadPool(int threads, int maxWaiting, bool start):
tasks_(maxWaiting), threads_(threads) //设置capacity,初始化线程数组的初始数目
{
    if (start) {
        this->start();
    }
}

ThreadPool::~ThreadPool() {
    assert(tasks_.exited());
    if (tasks_.size()) {
        fprintf(stderr, "%lu tasks not processed when thread pool exited\n",
            tasks_.size());
    }
}

void ThreadPool::start() { //定义了一个pool，然后启动他
    for (auto& th: threads_) {
        thread t(
            [this] {
                while (!tasks_.exited()) { //当this->exit_不为真时
                    Task task; //定义个函数
                    if (tasks_.pop_wait(&task)) {  //合适的时机把task函数设为从队列头函数
                        task();  //跑这个函数
                    }
                }
            }
        );
        th.swap(t);
    }
}

void ThreadPool::join() {
    for (auto& t: threads_) {
        t.join(); //等待线程结束
    }
}

bool ThreadPool::addTask(Task&& task) {
    return tasks_.push(move(task));  //加进去
}

}
