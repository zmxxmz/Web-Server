#ifndef BLOCK_QUEUE_H
#define BLOCK_QUEUE_H

#include<iostream>
#include<stdlib.h>
#include<pthread.h>
#include<sys/time.h>
#include"../lock/locker.h"

//成员函数都是线程安全的
//当不满足条件的时候，都是阻塞的
template<class T>
class block_queue
{
public:
    block_queue(int max_size=1000)
    {
        if(max_size<=0)
        {
            exit(-1);
        }
        m_max_size=max_size;
        m_arry=new T[max_size];
        m_size=0;
        m_front=-1;
        m_back=-1;
    }
    void clear()
    {
        m_mutex.lock();
        m_size=0;
        m_front=-1;
        m_back=-1;
        m_mutex.unlock();
    }
    ~block_queue()
    {
        m_mutex.lock();
        if(m_arry!=NULL)
        {
            delete[] m_arry;
        }
        m_mutex.unlock();
    }
    bool full()
    {
        m_mutex.lock();
        if(m_size>=m_max_size)
        {
            m_mutex.unlock();
            return true;
        }
        m_mutex.unlock();
        return false;
    }
    bool empty()
    {
        m_mutex.lock();
        if(0==m_size)
        {
            m_mutex.unlock();
            return true;
        }
        m_mutex.unlock();
        return false;
    }
    bool front(T& value)
    {
        m_mutex.lock();
        if(0==m_size)
        {
            m_mutex.unlock();
            return false;
        }
        value=m_arry[m_front];
        m_mutex.unlock();
        return true;
    }
    bool back(T& value)
    {
        m_mutex.lock();
        if(0==m_size)
        {
            m_mutex.unlock();
            return false;
        }
        value=m_arry[m_back];
        m_mutex.unlock();
        return true;
    }
    int size()
    {
        int tmp=0;
        m_mutex.lock();
        tmp=m_size;
        m_mutex.unlock();
        return tmp;
    }
    int max_size()
    {
        int tmp=0;
        m_mutex.lock();
        tmp=m_max_size;
        m_mutex.unlock();
        return tmp;
    }
    bool push(const T& item)
    {
        m_mutex.lock();
        if(m_size>=m_max_size)
        {
            m_cond.broadcast();
            m_mutex.unlock();
            return false;
        }
        m_back=(m_back+1)%m_max_size;
        m_arry[m_back]=item;
        m_size++;
        m_cond.broadcast();
        m_mutex.unlock();
        return true;
    }
    bool pop(T& item)
    {
        m_mutex.lock();
        //当条件变量成功获得的时候，可能资源还是不够的，继续下一遍获取
        while(m_size<=0)
        {
            if(!m_cond.wait(m_mutex.get()))
            {
                m_mutex.unlock();
                return false;
            }
        }
    
        m_front=(m_front+1)%m_max_size;
        item=m_arry[m_front];
        m_size--;
        m_mutex.unlock();
        return true;
    }
private:
    locker m_mutex;
    cond m_cond;

    T* m_arry;
    int m_size;
    int m_max_size;
    int m_front;
    int m_back;
};

#endif