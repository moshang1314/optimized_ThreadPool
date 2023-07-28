#ifndef THREADPOOL_H
#define THREADPOOL_H
#include <vector>
#include <queue>
#include <memory>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <unordered_map>
#include <thread>
#include <future>
#include <iostream>
#include <string>

const int TASK_MAX_THRESHHOLD = 1;
const int THREAD_MAX_THRESHHOLD = 100;
const int THREAD_MAX_IDLE_TIME = 60; //单位：s


//线程池支持的模式
enum class PoolMode {
    MODE_FIXED, //固定数量的线程
    MODE_CACHED // 线程数量可动态增长
};

//线程类型
class Thread {
public:
    using ThreadFunc = std::function<void(int)>;
    //线程构造
    Thread( ThreadFunc func )
        : func_(func)
        , threadId_(generateId_++) {}
    //线程析构
    ~Thread() = default;
    //启动线程
    void start() {
        //创建一个线程来执行一个线程函数
        std::thread t( func_ , threadId_);
        t.detach(); //设置分离线程
    }
    //获取线程id
    int  getId() const {
        return threadId_;
    }
private:
    ThreadFunc func_;
    static int generateId_;
    int threadId_;  //保存线程id
};

int Thread::generateId_ = 0;

class ThreadPool {
public:
    //线程池构造
    ThreadPool()
        : initThreadSize_( 0 )
        , taskSize_( 0 )
        , idleThreadSize_( 0 )
        , curThreadSize_(0)
        , taskQueMaxThreshHold_( TASK_MAX_THRESHHOLD )
        , threadSizeThreshHold_(THREAD_MAX_THRESHHOLD)
        , poolMode_( PoolMode::MODE_FIXED )
        , isPoolRunning_( false ) {}
    
    //线程池析构
    ~ThreadPool() {
        isPoolRunning_ = false;
        //等待线程池里面所有的线程返回 有两种状态：阻塞 & 正在执行任务，还有注意一种隐藏的状态，线程正在获取锁的状态，此时notify_all()并不能唤醒该线程，会导致死锁
        std::unique_lock<std::mutex> lock( taskQueMtx_ );
        notEmpty_.notify_all();
        exitCond_.wait( lock , [ & ] ()->bool {return curThreadSize_ == 0;} );
    }

    //设置线程池的工作模式
    void setMode( PoolMode mode ) {
        if (checkRunningState()) {
            return;
        }
        poolMode_ = mode;
    }

    // 设置task任务队列上限阈值
    void setTaskQueMaxThreshHold( int threshhold ) {
        if (checkRunningState())
            return;
        taskQueMaxThreshHold_ = threshhold;
    }

    // 设置线程池cached模式下的线程数阈值
    void setThreadSizeThreshHold( int threshhold ) {
        if (checkRunningState())
            return;
        if (poolMode_ == PoolMode::MODE_CACHED) {
            threadSizeThreshHold_ = threshhold;
        }
    }

    //给线程池提交任务
    //decltype()推导表达式类型但执行表达式
    template<typename Func , typename... Args>
    auto submitTask( Func&& func , Args&&... args ) -> std::future<decltype( func( args... ) )> {
        //打包任务，放入任务队列
        using RType = decltype( func( args... ) );
        auto task = std::make_shared<std::packaged_task<RType()>>(
            std::bind( std::forward<Func>( func ) , std::forward<Args>( args )... ) );
        std::future<RType> result = task->get_future();

        //获取锁
        std::unique_lock<std::mutex> lock( taskQueMtx_ );
        //std::cout << "Start wait..." << std::endl;
        //线程通信 等待任务队列有空余
        // 用户提交任务，最长不能阻塞超过1s，否则判断提交失败，返回
        if (!notFull_.wait_for( lock , std::chrono::seconds( 1 ) , [ &  ] ()->bool {return taskQue_.size() < (size_t)taskQueMaxThreshHold_;} )) {
            //表示条件变量等待超时
            //std::cerr << "task queue is full, submit task fail." << std::endl;
            // return task->getResult(); // 线程执行完task，task对象就被析构掉了
            auto task = std::make_shared<std::packaged_task<RType()>>(
                [] ()->RType { throw std::string("TaskQueue is full, submit task fail"); }
            );
            (*task)();
            return task->get_future();
        } 
        //如果有空余，把任务放入任务队列中
        taskQue_.emplace( [task] () {
            ( *task )( );
            } );
        taskSize_++;

        //因为放了新任务，任务队列肯定不空了，notEmpty_通知
        notEmpty_.notify_all();

        // cache模式
        if (poolMode_ == PoolMode::MODE_CACHED && taskSize_ > idleThreadSize_ && curThreadSize_ < threadSizeThreshHold_) {
            //创建新线程
            auto ptr = std::make_unique<Thread>( std::bind( &ThreadPool::threadFunc , this , std::placeholders::_1) );
            int threadId = ptr->getId();
            threads_.emplace( threadId , std::move( ptr ) );
            threads_[threadId]->start();
            curThreadSize_++;
            idleThreadSize_++;
            std::cout << "create new thread...." << std::endl;
        }  

        //返回任务的Result对象
        return result;
    }

    //开启线程池
    void start( int initThreadSize ) { 
        //设置线程池的启动状态
        isPoolRunning_ = true;
        //记录初始线程个数
        initThreadSize_ = initThreadSize;

        //创建线程个数
        for (int i = 0; i < initThreadSize_; i++) {
            auto ptr = std::make_unique<Thread>( std::bind( &ThreadPool::threadFunc , this , std::placeholders::_1) );
            int threadId = ptr->getId();
            threads_.emplace( threadId , std::move(ptr));
            //threads_.emplace_back( std::move( ptr ) );
        }
 
        //启动所有线程
        for (int i = 0; i < initThreadSize_; i++) {
            threads_[i]->start();
            idleThreadSize_++; //记录初始空闲线程的数量
            curThreadSize_++;
        }
    }

    ThreadPool( const ThreadPool& ) = delete;
    ThreadPool& operator=( const ThreadPool& ) = delete;

private:
    //线程函数，线程池的所有线程从任务队列中消费任务
    void threadFunc( int threadId ) {
        Task task;
        auto lastTime = std::chrono::high_resolution_clock().now();
        for(;;) {
        {
            //先获取锁
            std::unique_lock<std::mutex> lock( taskQueMtx_ );

            std::cout << "tid: " << std::this_thread::get_id()
                << "尝试获取任务..." << std::endl;
            while (taskQue_.size() == 0) {
                //线程池要结束，回收线程资源
                if (!isPoolRunning_) {
                    threads_.erase( threadId );
                    curThreadSize_--;
                    exitCond_.notify_all();

                    return;
                }
                //cache模式下，有可能创建了很多线程，但是空闲时间超过60s，应该把多余的线程回收掉
                if (poolMode_ == PoolMode::MODE_CACHED) {
                     if (std::cv_status::timeout == notEmpty_.wait_for( lock , std::chrono::seconds( 1 ) )) {
                        auto now = std::chrono::high_resolution_clock().now();
                        auto dur = std::chrono::duration_cast<std::chrono::seconds>( now - lastTime );
                        if (dur.count() >= 60 && curThreadSize_ > initThreadSize_) {
                            //开始回收线程
                            //记录线程数量的相关变量的值修改
                            //把  threadFunc <-> Thread对象
                            threads_.erase( threadId );
                            curThreadSize_--;
                            idleThreadSize_--;
                            std::cout << "thread: " << std::this_thread::get_id() << " exit..." << std::endl;
                            return;
                        }
                     }
                }
                else {
                    //等待notEmpty条件
                    notEmpty_.wait( lock);
                }
            }
            idleThreadSize_--;
            //从任务队列中取出一个任务出来
            task = taskQue_.front();
            taskQue_.pop();
            taskSize_--;
            
            std::cout << "tid: " << std::this_thread::get_id()
                << "获取任务成功..." << std::endl;
            
            //如果依然还有剩余任务，继续通知其他线程执行任务
            if (taskQue_.size() > 0) {
                notEmpty_.notify_all();
            }
            //取出一个任务，进行通知
            notFull_.notify_all();
        }
        //当前线程负责执行这个任务
        if (task != nullptr) {
            task( );
        }
        idleThreadSize_++;
        lastTime = std::chrono::high_resolution_clock().now();  //更新线程执行完任务的时间
    }
}
    //检查pool的运行状态
    bool checkRunningState()const {
        return isPoolRunning_;
    }
private:
    std::unordered_map<int , std::unique_ptr<Thread>> threads_; //线程列表
    size_t initThreadSize_; //初始的线程数量
    int threadSizeThreshHold_; // 线程数量上限阈值
    std::atomic_int curThreadSize_;//记录当前线程池里面线程的总数量
    std::atomic_int idleThreadSize_; //记录空闲线程的数量

    using Task = std::function<void()>;
    std::queue<Task> taskQue_; // 任务队列
    std::atomic_int taskSize_;  //任务数量
    int taskQueMaxThreshHold_;  //任务队列数量上限阈值

    std::mutex taskQueMtx_; //保证任务队列的线程安全
    std::condition_variable notFull_;  //表示任务队列不满
    std::condition_variable notEmpty_;  //表示任务队列不空
    std::condition_variable exitCond_;  //等待线程资源回收

    PoolMode poolMode_; //当前线程池的工作模式
    std::atomic_bool isPoolRunning_; //表示当前线程池的启动状态
};

#endif