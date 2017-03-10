#pragma once
#include <thread>
#include <atomic>
#include <list>
#include <vector>
#include <functional>
#include <limits>
#include <condition_variable>
#include <mutex>
#include "util.h"

//这些代码需要弄明白几个类 std::condition_variable std::mutex std::thread


namespace handy {

template<typename T> struct SafeQueue: private std::mutex, private noncopyable { //继承了一个信号量
    static const int wait_infinite = std::numeric_limits<int>::max();  //最大数
    //0 不限制队列中的任务数
    SafeQueue(size_t capacity=0): capacity_(capacity), exit_(false) {}
    //队列满则返回false
    bool push(T&& v);
    //超时则返回T()
    T pop_wait(int waitMs=wait_infinite);
    //超时返回false
    bool pop_wait(T* v, int waitMs=wait_infinite);

    size_t size();
    void exit();
    bool exited() { return exit_; } //返回的时候用
private:
    std::list<T> items_;  //存函数们
    std::condition_variable ready_;  //队列的是否ready标志 std::condition_variable
    size_t capacity_;  //队列限制
    std::atomic<bool> exit_; //多线程访问的良好特性
    void wait_ready(std::unique_lock<std::mutex>& lk, int waitMs);  //加锁等xxms
};

typedef std::function<void()> Task; //一个Task就是一个void ()函数
extern template class SafeQueue<Task>; //定义一个其他文件也能访问到的全局外部变量

struct ThreadPool: private noncopyable {
    //创建线程池
    ThreadPool(int threads, int taskCapacity=0, bool start=true);
    ~ThreadPool();
    void start();
    ThreadPool& exit() { tasks_.exit(); return *this; }
    void join();

    //队列满返回false
    bool addTask(Task&& task); //右值引用
    bool addTask(Task& task) { return addTask(Task(task)); }
    size_t taskSize() { return tasks_.size(); }  //任务队列大小
private:
    SafeQueue<Task> tasks_; //线程池里包括一个队列的任务，任务就是函数
    std::vector<std::thread> threads_; //和一个线程数组
};


template<typename T> size_t SafeQueue<T>::size() {  //队列里有几个对象
    std::lock_guard<std::mutex> lk(*this);  //这个是c++11新的加锁帮助
    return items_.size();
}

template<typename T> void SafeQueue<T>::exit() { //退了就通知大家
    exit_ = true;
    std::lock_guard<std::mutex> lk(*this);
    ready_.notify_all();
}

template<typename T> bool SafeQueue<T>::push(T&& v) { //判断一下是否能加，如果能加就通知一个
    std::lock_guard<std::mutex> lk(*this);
    if (exit_ || (capacity_ && items_.size() >= capacity_)) {
        return false;
    }
    items_.push_back(std::move(v));
    ready_.notify_one();
    return true;
}
template<typename T> void SafeQueue<T>::wait_ready(
    std::unique_lock<std::mutex>& lk, int waitMs)
{
    if (exit_ || !items_.empty()) {
        return;
    }
    if (waitMs == wait_infinite) {  //无限等待
        ready_.wait(lk, [this]{ return exit_ || !items_.empty(); });
    } else if (waitMs > 0){
        auto tp = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(waitMs);
        while (ready_.wait_until(lk, tp) != std::cv_status::timeout
            && items_.empty() && !exit_) {
        }
    }
}



template<typename T> bool SafeQueue<T>::pop_wait(T* v, int waitMs) {
    std::unique_lock<std::mutex> lk(*this);
    wait_ready(lk, waitMs); //锁解了
    if (items_.empty()) {
        return false;
    }
    *v = std::move(items_.front());  //把队列最前任务返回，是一个函数
    items_.pop_front(); //items_也曲调最前
    return true;
}

template<typename T> T SafeQueue<T>::pop_wait(int waitMs) {  //跟前一个类似，不管pop的对象
    std::unique_lock<std::mutex> lk(*this);
    wait_ready(lk, waitMs);
    if (items_.empty()) {
        return T();
    }
    T r = std::move(items_.front());
    items_.pop_front();
    return r;
}

}
