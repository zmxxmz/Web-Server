#ifndef LOH_H
#define LOH_H

#include<stdio.h>
#include<iostream>
#include<string>
#include<stdarg.h>
#include<pthread.h>
#include"block_queue.h"

using namespace std;

class Log
{
private:
    //路径名
    char dir_name[128];
    //log文件名
    char log_name[128];
    //日志最大行数
    int m_split_lines;
    //日志缓存区大小
    int m_log_buf_size;
    //日志行数记录
    long long m_count;
    //当前时间是哪一天
    int m_today;
    //打开log的文件指针
    FILE* m_fp;
    char* m_buf;
    //阻塞队列
    block_queue<string>* m_log_queue;
    //同步标志位
    bool m_is_async;
    locker m_mutex;
    //关闭日志
    int m_close_log;
public:
    static Log* get_instance()
    {
        static Log instance;
        return &instance;
    }
    static void* flush_log_thread(void* args)
    {
        Log::get_instance()->async_write_log();
    }
    bool init(const char *file_name, int close_log, int log_buf_size = 8192, int split_lines = 5000000, int max_queue_size = 0);
    void write_log(int level,const char* format,...);
    void flush(void);
private:
    Log();
    virtual ~Log();
    void* async_write_log()
    {
        string single_log;
        while(m_log_queue->pop(single_log))
        {
            m_mutex.lock();
            fputs(single_log.c_str(),m_fp);
            m_mutex.unlock();
        }
    }
};

#define LOG_DEBUG(format, ...) Log::get_instance()->write_log(0, format, ##__VA_ARGS__)
#define LOG_INFO(format, ...) Log::get_instance()->write_log(1, format, ##__VA_ARGS__)
#define LOG_WARN(format, ...) Log::get_instance()->write_log(2, format, ##__VA_ARGS__)
#define LOG_ERROR(format, ...) Log::get_instance()->write_log(3, format, ##__VA_ARGS__)
#endif