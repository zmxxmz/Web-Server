#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H    

#include<unistd.h>
#include<signal.h>
#include<sys/types.h>
#include<fcntl.h>
#include <sys/uio.h>
#include<sys/socket.h>
#include<sys/epoll.h>
#include<netinet/in.h>
#include<arpa/inet.h>
#include<assert.h>
#include<sys/stat.h>
#include<string.h>
#include<pthread.h>
#include<stdio.h>
#include<stdlib.h>
#include<sys/mman.h>
#include<stdarg.h>
#include<errno.h>
#include<unordered_map>
#include<mysql/mysql.h>
#include"../lock/locker.h"
#include"../CgiMysql/sql_connection_pool.h"
#include"../log/log.h"
#include"../config.h"
using namespace std;
class util_timer;/*前向声明*/
class http_conn
{
public:
    //文件名的最大长度
    static const int FILENAME_LEN=200;
    //读缓冲区的大小
    static const int READ_BUFFER_SIZE=2048;
    //写缓冲器的大小
    static const int WRITE_BUFFER_SIZE=1024;
    //http请求方法
    enum METHOD
    {
        GET=0,POST,HEAD,PUT,DELETE,
        TRACE,OPTIONS,CONNECT,PATCH
    };
    //解析http请求的主状态机
    enum CHECK_STATE
    {
        //正在解析请求行
        CHECK_STATE_REQUESTLINE=0,
        //正在解析请求头
        CHECK_STATE_HEADER,
        //正在解析请求正文
        CHECK_STATE_CONTENT
    };
    //解析http请求的从状态机
    enum LINE_STATUS
    {
        LINE_OK=0,
        LINE_BAD,
        LINE_OPEN
    };
    //服务器处理http请求的可能结果
    enum HTTP_CODE
    {
        NO_REQUEST,GET_REQUEST,BAD_REQUEST,
        NO_RESOURCE,FORBIDDEN_REQUEST,FILE_REQUEST,
        INTERNAL_ERROR,CLOSEND_CONNECTION
    };
public:
    static int m_epollfd;
    static int m_user_count;
    //数据库连接池
    static connection_pool* sql_pool;
    //缓存用户名和密码
    static unordered_map<string,string> usersmap;
    //对应map的锁
    static pthread_mutex_t maplock;
    //定时器
    util_timer* m_timer;
private:
    //该http请求连接的socket
    int m_sockfd;
    //对方的socket地址
    sockaddr_in m_address;
    //读缓冲区
    char m_read_buf[READ_BUFFER_SIZE];
    //读缓冲区实际储存数据的最后的一个字节的下一个位置
    int m_read_idx;
    //当前正在解析的字符在读缓冲区位置？
    int m_checked_idx;
    //当前正在解析的行的起始位置
    int m_start_line;
    //读缓存区
    char m_write_buf[WRITE_BUFFER_SIZE];
    //写缓冲区待发送的字节数
    int m_write_idx;
    //主状态机当前所在的状态
    CHECK_STATE m_check_state;
    //请求方法
    METHOD m_method;
    //客户请求的目标文件的完整路径
    char m_real_file[FILENAME_LEN];
    //请求正文
    char* m_context;
    //客户请求的目标文件的文件名
    char* m_url;
    //http协议版本号
    char* m_version;
    //主机名？
    char* m_host;
    //http请求的消息体的长度
    int m_content_length;
    //http请求是否要求保持连接
    bool m_linger;
    //客户请求的目标文件被mmap到内存的起始位置
    char* m_file_address;
    //目标文件的状态？
    struct stat m_file_stat;
    int post_ation;
    //内存块？
    struct iovec m_iv[2];
    //内存块数量
    int m_iv_count;
    int bytes_have_send;
    int bytes_to_send;
public:
    http_conn(){}
    ~http_conn(){}
    //初始化客户账号、密码
    void static initmysql_result();
public:
    //初始化新的连接
    void init(int sockfd,const  sockaddr_in& addr,util_timer* timer);
    //关闭连接
    void close_conn(bool real_close=true);
    //由线程池中的工作线程调用，这是进入http-conn对象的入口函数，对于每一个任务，只会执行一次
    void process();
    //io模式为模拟proactor，所以socket上的读写都会在主线程上
    //非阻塞读操作 当读操作出现错误的时候，返回假
    bool read();
    //非阻塞写操作
    bool write();
private:
    //初始化连接
    void init();
    //解析整个http请求
    HTTP_CODE process_read();
    
    //根据服务器处理HTTP请求的结果，决定返回给客户端的内容
    bool process_write(HTTP_CODE ret);

    /*下面函数辅助process_read解析http请求*/
    //读入并预处理http请求的一整行(从状态机)
    LINE_STATUS parse_line();
    //解析请求行的内容
    HTTP_CODE parse_request_line(char*text);
    //解析请求头的内容
    HTTP_CODE parse_headers(char* text);
    //解析请求正文的内容
    HTTP_CODE parse_content(char* text);

    /*下面函数辅助process_write来填充http应答*/
    //当得到一个完整、正确的HTTP请求时，我们就分析目标文件的属性。如果目标文件存在、对所有用户可读，且不是目录，则使用mmap将其映射到内存地址m_file_address处，并告诉调用者获取文件成功
    HTTP_CODE do_request();
    char* get_line()
    {
        return m_read_buf+m_start_line;
    }
    void unmap();
    bool add_response(const char* format,...);
    bool add_content(const char* content);
    bool add_status_line(int status,const char* title);
    bool add_headers(int content_length);
    bool add_content_length(int content_length);
    bool add_linger();
    bool add_blank_line();
};

#endif