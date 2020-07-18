#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <list>
#include <cstdio>
#include <exception>
#include <pthread.h>
#include "../lock/locker.h"
#include"../config.h"
template<typename T>
class threadpool
{
private:
    //线程池线程个数
    int m_thread_number;
    //请求队列
    std::list<T*> m_workqueue;
    //请求队列所允许的最大请求数
    int m_max_requests;
    //描述线程池的数祖
    pthread_t* m_threads;
    //请求队列的互斥锁
    locker m_queuelocker;
    //请求队列的信号量，是否还有任务未处理，目的是为了在有任务的时候唤醒阻塞线程
    sem m_queuestat;
    //停止？
    bool m_stop;
public:
    threadpool(int thread_number=1,int max_requests=1000);
    ~threadpool();
    bool apend(T* requst);
private:
    //线程运行的入口函数，因为是类成员函数，必须定义为静态的
    static void* worker(void* arg);
    void run();
};
template<typename T>
threadpool<T>::threadpool(int thread_number,int max_requests):
    m_thread_number(thread_number),m_max_requests(max_requests),
    m_stop(false),m_threads(NULL)
{
    if((m_thread_number<=0)||(max_requests<=0))
    {
        throw std::exception();
    }
    m_threads=new pthread_t[m_thread_number];
    if(!m_threads)
    {
        throw std::exception();
    }
    for(int i=0;i<m_thread_number;++i)
    {
        if(pthread_create(m_threads+i,NULL,worker,this)!=0)
        {
            delete[] m_threads;
            throw std::exception();
        }
        //设置线程相分离 由系统管理线程的资源
        if(pthread_detach(m_threads[i]))
        {
            delete[] m_threads;
            throw std::exception();
        }
    }
}

template<typename T>
threadpool<T>::~threadpool()
{
    delete[] m_threads;
    m_stop=true;
} 

template<typename T>
bool threadpool<T>::apend(T* request)
{
    m_queuelocker.lock();
    if(m_workqueue.size()>m_max_requests)
    {
        m_queuelocker.unlock();
        return false;
    }
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post();
    return true;
}

template<typename T>
void* threadpool<T>::worker(void* arg){
    threadpool* pool =(threadpool*)arg;
    pool->run();
    return pool;
}   

template<typename T>
void threadpool<T>::run()
{
    while(!m_stop)
    {
        m_queuestat.wait();
        m_queuelocker.lock();
        if(m_workqueue.empty())
        {
            m_queuelocker.unlock();
            continue;
        }
        T* request =m_workqueue.front();
        m_workqueue.pop_front();
        m_queuelocker.unlock();
        if(!request)
        {
            continue;
        }
        request->process();
    }
}

#endif