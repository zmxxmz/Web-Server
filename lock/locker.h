#ifndef LOCKER_H
#define LOCKER_H

#include<exception>
#include<pthread.h>
#include<semaphore.h>

class sem
{
public:
	sem()
	{
		//int sem_init(sem_t *__sem, int __pshared, unsigned int __value)
		//(*,0的话就是当前进程的局部信号量，*)
		if(sem_init(&m_sem,0,0)!=0)
		{
			//类构造函数没有返回值，可以抛出异常报告错误
			throw std::exception();
		}
	}
	sem(int num){
		if(sem_init(&m_sem,0,num)!=0)
		{
			throw std::exception();
		}
	}
	~sem()
	{
		sem_destroy(&m_sem);
	}
	bool wait()
	{
		//阻塞的 (sem_trywait立即返回）
		return sem_wait(&m_sem)==0;
	}
	bool post()
	{
		//原子操作把信号量加1，大于0时，唤醒等待该信号量的线程
		return sem_post(&m_sem)==0;
	}
private:
	//union
	sem_t m_sem;
};

class locker
{
public:
	locker()
	{
		if(pthread_mutex_init(&m_mutex,NULL)!=0)
		{
			throw std::exception();
		}
	}
	~locker()
	{
		pthread_mutex_destroy(&m_mutex);
	}
	bool lock()
	{
		return pthread_mutex_lock(&m_mutex)==0;
	}
	bool unlock()
	{
		return pthread_mutex_unlock(&m_mutex)==0;
	}
	pthread_mutex_t* get(){
		return &m_mutex;
	}
private:
	pthread_mutex_t m_mutex;
};

class cond
{
public:
	cond()
	{
		if(pthread_cond_init(&m_cond,NULL)!=0)
		{
			throw std::exception();
		}
	}
	~cond()
	{
		pthread_cond_destroy(&m_cond);
	}
	bool wait(pthread_mutex_t* m_mutex)
	{
		int ret=0;
		ret=pthread_cond_wait(&m_cond,m_mutex);
		return ret;
	}
	bool signal(){
	return pthread_cond_signal(&m_cond)==0;
	}
	bool broadcast()
	{
		return pthread_cond_broadcast(&m_cond)==0;
	}
private:
	pthread_cond_t m_cond;
};

#endif