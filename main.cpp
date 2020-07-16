#include<sys/socket.h>
#include<netinet/in.h>
#include<stdio.h>
#include<unistd.h>
#include<errno.h>
#include<string.h>
#include<fcntl.h>
#include<stdlib.h>
#include<cassert>
#include<sys/epoll.h>
#include<iostream>
#include"lock/locker.h"
#include"threadpool/threadpool.h"
#include"http/http_conn.h"
#include"timer/lst_timer.h"
#include"CgiMysql/sql_connection_pool.h"
#include"config.h"
using namespace std;
#define MAX_FD 65536    
#define MAX_EVENT_NUMBER 10000
#define TIMESLOT 5
//下面的三个函数定义在http_conn.cpp中，所以必须申明函数已经定义
extern int addfd(int epollfd,int fd,bool one_shot);
extern int removefd(int epollfd,int fd);
extern int setnonblocking(int fd);

struct epoll_events :public epoll_event
{
public:
    void process();
};
static int pipefd[2];
static int epollfd;
int listenfd;
http_conn* users=new http_conn[MAX_FD];
static sort_timer_lst timer_lst;
bool timeout=false;
bool stop_server=false;
#ifdef PROCTOR
threadpool<http_conn>*pool=NULL;
#endif

#ifdef REACTOR
threadpool<epoll_events>*pool=NULL;
#endif

void sig_handler(int sig)
{
    int save_errno=errno;
    int msg=sig;
    send(pipefd[1],(char*)&msg,1,0);
    errno=save_errno;
}
void timer_handler()
{
    //定时处理任务，实际上就是调用tick函数
    timer_lst.tick();
    //因为一次alarm调用只会引起一次SIGALRM信号，所以我们要重新定时，以不断触发SIGALRM信号
    alarm(TIMESLOT);
}
//定时器回调函数，它删除非活动连接socket上的注册事件，并关闭之
void cb_func(http_conn*user_data)
{
    user_data->close_conn();
}
void addsig(int sig,void(handler)(int),bool restart=true)
{
    struct sigaction sa;
    memset(&sa,'\0',sizeof(sa));
    sa.sa_handler=handler;
    if(restart)
    {
        sa.sa_flags|=SA_RESTART;
    }
    sigfillset(&sa.sa_mask);
    assert(sigaction(sig,&sa,NULL)!=-1);
}

void show_error(int connfd,const char* info)
{
    printf("%s",info);
    send(connfd,info,strlen(info),0);
    close(connfd);
}
void epoll_events::process()
{
        int sockfd=(*this).data.fd;
        if(sockfd==listenfd)
        {
            struct sockaddr_in client_address;
            socklen_t client_addrlength=sizeof(client_address);
            #ifdef ET
                int connfd=0;
                while((connfd=accept(listenfd,(struct sockaddr*)&client_address,&client_addrlength))>=0)
                {
                    if(connfd<0)break;
                    if(http_conn::m_user_count>=MAX_FD) 
                    {
                        show_error(connfd,"Internal server busy");
                        break;
                    }
                    /*创建定时器，设置其回调函数与超时时间，然后绑定定时器与用户数据，最后将定时器添加到链表timer_lst中*/
                    util_timer*timer=new util_timer;
                    time_t cur=time(NULL);
                    timer->expire=cur+3*TIMESLOT;
                    timer->user_data=&users[connfd];
                    timer->cb_func=cb_func;
                    users[connfd].init(connfd,client_address,timer);
                    timer_lst.add_timer(timer);
                }
            #endif
            #ifdef LT
                int connfd=accept(listenfd,(struct sockaddr*)&client_address,&client_addrlength);
                if(connfd<0)break;
                if(http_conn::m_user_count>=MAX_FD) 
                {
                    show_error(connfd,"Internal server busy");
                    break;
                }
                /*创建定时器，设置其回调函数与超时时间，然后绑定定时器与用户数据，最后将定时器添加到链表timer_lst中*/
                util_timer*timer=new util_timer;
                time_t cur=time(NULL);
                timer->expire=cur+3*TIMESLOT;
                timer->user_data=&users[connfd];
                timer->cb_func=cb_func;
                users[connfd].init(connfd,client_address,timer);
                timer_lst.add_timer(timer);
            #endif
        }   
        else if((sockfd==pipefd[0])&&((*this).events&EPOLLIN))
        {
            int sig;
            char signals[1024];
            int ret=recv(pipefd[0],signals,sizeof(signals),0);
            if(ret==-1)
            {
                //handle the error
                return;
            }
            else if(ret==0)
            {
                return;
            }
            else
            {
                for(int i=0;i<ret;++i)
                {
                    switch(signals[i])
                    {
                        case SIGALRM:
                        {
                            /*用timeout变量标记有定时任务需要处理，但不立即处理定时任务。这是因为定时任务的优先级不是很高，我们优先处理其他更重要的任务*/
                            timeout=true;
                            break;
                        }
                        case SIGTERM:
                        {
                            stop_server=true;
                        }
                    }
                }
            }
        }
        //                                                对方关闭连接 挂起              错误
        else if((*this).events&(EPOLLRDHUP|EPOLLHUP|EPOLLERR))
        {
            users[sockfd].close_conn();
        }
        else if((*this).events&EPOLLIN)
        {
            if(users[sockfd].read())
            {
                //pool->apend(users+sockfd);
                users[sockfd].process();
                time_t cur=time(NULL);
                util_timer* timer=users[sockfd].m_timer;
                timer->expire=cur+TIMESLOT*3;
                timer_lst.adjust_timer(timer);
            }
            else
            {
                users[sockfd].close_conn();
            }
        }
        else if((*this).events&EPOLLOUT)
        {
            if(!users[sockfd].write())
            {
                users[sockfd].close_conn();
            }
            else
            {
                time_t cur=time(NULL);
                util_timer* timer=users[sockfd].m_timer;
                timer->expire=cur+TIMESLOT*3;
                timer_lst.adjust_timer(timer);
            }
        }
        else
        {
        }
}
int main(int argc,char*argv[])
{
    if(argc<=2)
    {
        printf("usage: %s ip_address port_number\n",basename(argv[0]));
        return 1;
    }
    const char* ip=argv[1];
    int port=atoi(argv[2]);
    Log::get_instance()->init("ServerLog", 2000, 800000, 8);
    #ifdef PROCTOR
    try
    {
        pool=new threadpool<http_conn>;
    }
    catch(...)
    {
        return 1;
    }
    #endif

    #ifdef REACTOR
    try
    {
        pool=new threadpool<epoll_events>;
    }
    catch(...)
    {
        return 1;
    }
    #endif
    assert(users);
    connection_pool* sql_pool=connection_pool::GetInstance();
    sql_pool->init("localhost", "zmx", "zmxmysql", "yourdb", 3306, 8);
    http_conn::sql_pool=sql_pool;
    int user_count=0;
    listenfd=socket(PF_INET,SOCK_STREAM,0);
    assert(listenfd>=0);
    //优雅关闭socket 会等待数据发送完毕再关闭socket
    // struct linger tmp={1,0};
    // setsockopt(listenfd,SOL_SOCKET,SO_LINGER,&tmp,sizeof(tmp));
    int ret=0;
    struct sockaddr_in address;
    bzero(&address,sizeof(address));
    address.sin_family=AF_INET;
    inet_pton(AF_INET,ip,&address.sin_addr);
    address.sin_port=htons(port);
    ret=bind(listenfd,(struct sockaddr*)&address,sizeof(address));
    assert(ret>=0);
    ret=listen(listenfd,5);
    assert(ret>=0);
    #ifdef PROCTOR
    epoll_event events[MAX_EVENT_NUMBER];
    #endif 
    #ifdef REACTOR
    epoll_events events[MAX_EVENT_NUMBER];
    #endif
    epollfd=epoll_create(5);
    assert(epollfd!=-1);
    addfd(epollfd,listenfd,false);
    ret=socketpair(PF_UNIX,SOCK_STREAM,0,pipefd);
    assert(ret!=-1);
    setnonblocking(pipefd[1]);
    addfd(epollfd,pipefd[0],false);
    //往一个关闭的socket写会触发SIGPIPE,默认行为为结束进程，所以我们这里要不管这个对这个信号函数设置为不处理
    addsig(SIGPIPE,SIG_IGN);
    addsig(SIGALRM,sig_handler);
    addsig(SIGTERM,sig_handler);
    http_conn::m_epollfd=epollfd;
    pthread_mutex_init(&http_conn::maplock,NULL);
    alarm(TIMESLOT);
    while(!stop_server)
    {
        int number=epoll_wait(epollfd,(epoll_event*)events,MAX_EVENT_NUMBER,-1);
        if((number<0)&&(errno!=EINTR))
        {   
            printf("epoll failure\n");
            break;
        }
        #ifdef PROCTOR
        for(int i=0;i<number;i++)
        {
            int sockfd=events[i].data.fd;
            if(sockfd==listenfd)
            {
                struct sockaddr_in client_address;
                socklen_t client_addrlength=sizeof(client_address);
                #ifdef ET
                    int connfd=0;
                    while((connfd=accept(listenfd,(struct sockaddr*)&client_address,&client_addrlength))>=0)
                    {
                        if(connfd<0)break;
                        if(http_conn::m_user_count>=MAX_FD) 
                        {
                            show_error(connfd,"Internal server busy");
                            break;
                        }
                        /*创建定时器，设置其回调函数与超时时间，然后绑定定时器与用户数据，最后将定时器添加到链表timer_lst中*/
                        util_timer*timer=new util_timer;
                        time_t cur=time(NULL);
                        timer->expire=cur+3*TIMESLOT;
                        timer->user_data=&users[connfd];
                        timer->cb_func=cb_func;
                        users[connfd].init(connfd,client_address,timer);
                        timer_lst.add_timer(timer);
                    }
                #endif
                #ifdef LT
                    int connfd=accept(listenfd,(struct sockaddr*)&client_address,&client_addrlength);
                    if(connfd<0)break;
                    if(http_conn::m_user_count>=MAX_FD) 
                    {
                        show_error(connfd,"Internal server busy");
                        break;
                    }
                    /*创建定时器，设置其回调函数与超时时间，然后绑定定时器与用户数据，最后将定时器添加到链表timer_lst中*/
                    util_timer*timer=new util_timer;
                    time_t cur=time(NULL);
                    timer->expire=cur+3*TIMESLOT;
                    timer->user_data=&users[connfd];
                    timer->cb_func=cb_func;
                    users[connfd].init(connfd,client_address,timer);
                    timer_lst.add_timer(timer);
                #endif
            }   
            else if((sockfd==pipefd[0])&&(events[i].events&EPOLLIN))
            {
                int sig;
                char signals[1024];
                ret=recv(pipefd[0],signals,sizeof(signals),0);
                if(ret==-1)
                {
                    //handle the error
                    continue;
                }
                else if(ret==0)
                {
                    continue;
                }
                else
                {
                    for(int i=0;i<ret;++i)
                    {
                        switch(signals[i])
                        {
                            case SIGALRM:
                            {
                                /*用timeout变量标记有定时任务需要处理，但不立即处理定时任务。这是因为定时任务的优先级不是很高，我们优先处理其他更重要的任务*/
                                timeout=true;
                                break;
                            }
                            case SIGTERM:
                            {
                                stop_server=true;
                            }
                        }
                    }
                }
            }
            //                                                对方关闭连接 挂起              错误
            else if(events[i].events&(EPOLLRDHUP|EPOLLHUP|EPOLLERR))
            {
                users[sockfd].close_conn();
            }
            else if(events[i].events&EPOLLIN)
            {
                if(users[sockfd].read())
                {
                    pool->apend(users+sockfd);
                    time_t cur=time(NULL);
                    util_timer* timer=users[sockfd].m_timer;
                    timer->expire=cur+TIMESLOT*3;
                    timer_lst.adjust_timer(timer);
                }
                else
                {
                    users[sockfd].close_conn();
                }
            }
            else if(events[i].events&EPOLLOUT)
            {
                if(!users[sockfd].write())
                {
                    users[sockfd].close_conn();
                }
                else
                {
                    time_t cur=time(NULL);
                    util_timer* timer=users[sockfd].m_timer;
                    timer->expire=cur+TIMESLOT*3;
                    timer_lst.adjust_timer(timer);
                }
            }
            else
            {
            }
        }
        #endif

        #ifdef REACTOR
        for(int i=0;i<number;i++)
        {
            pool->apend((events+i));
        }
        #endif
        if(timeout)
        {
            timer_handler();
            timeout=false;
        }
    }
    close(epollfd);
    close(listenfd);
    delete[] users;
    delete pool;
    return 0;
}
