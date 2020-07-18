#include "http_conn.h"
const char* ok_200_title="OK";
const char* error_400_title="Bad Request";
const char* error_400_form="Your request has bad syntax or is inherently impossible to satisfy.\n";
const char* error_403_title="Forbidden";
const char* error_403_form="You do not have permission to get file from this server.\n";
const char* error_404_title="Not Found";
const char* error_404_form="The requested file was not found on this server.\n";
const char* error_500_title="Internal Error";
const char* error_500_form="There was an unusual problem serving the requested file.\n";

const char* doc_root="/home/zmx/web-server/root";   
//因为后面都是采用epoll io复用，socket都设置成非阻塞的 
string match[]={"judge.html","log.html","logError.html","register.html","registerError.html"};

int http_conn::m_user_count=0;
int http_conn::m_epollfd=-1;
unordered_map<string,string> http_conn::usersmap;
pthread_mutex_t http_conn::maplock=PTHREAD_MUTEX_INITIALIZER;
connection_pool* http_conn::sql_pool=nullptr;

int setnonblocking(int fd)
{
    int old_option=fcntl(fd,F_GETFL);
    int new_option=old_option|O_NONBLOCK;
    fcntl(fd,F_SETFL,new_option);
    return old_option;
}

void addfd(int epollfd,int fd,bool one_shot)
{
    epoll_event event;
    event.data.fd=fd;
    #ifdef ET
    //                         可读     ET模式            对方关闭连接或关闭了写端
    event.events=EPOLLIN|EPOLLET|EPOLLRDHUP;
    #endif

    #ifdef LT
    event.events=EPOLLIN|EPOLLRDHUP;
    #endif

    if(one_shot)
    {
        //无论什么事件，都只会触发一次
        event.events|=EPOLLONESHOT;
    }
    epoll_ctl(epollfd,EPOLL_CTL_ADD,fd,&event);
    setnonblocking(fd);
}

//从监听epoll上去除，关闭socket连接
void removefd(int epollfd,int fd)
{
    epoll_ctl(epollfd,EPOLL_CTL_DEL,fd,0);
    close(fd);
}

void modfd(int epollfd,int fd,int ev)
{
    //取消已经注册的监听事件，置入新的事件
    epoll_event event;
    event.data.fd=fd;
    #ifdef ET
    event.events=ev|EPOLLONESHOT|EPOLLET|EPOLLRDHUP;
    #endif

    #ifdef LT
    event.events=ev|EPOLLONESHOT|EPOLLRDHUP;
    #endif
    epoll_ctl(epollfd,EPOLL_CTL_MOD,fd,&event);
}


void http_conn::close_conn(bool real_close)
{
    if(real_close&&(m_sockfd!=-1))
    {
        removefd(m_epollfd,m_sockfd);
        m_sockfd=-1;
        --m_user_count;
    }
}

void http_conn::init(int sockfd,const  sockaddr_in& addr,util_timer* timer)
{
    m_sockfd=sockfd;
    m_address=addr;
    m_timer=timer;
    //以下两行仅用于调试 对于tcp socket，可以消除time-wait状态，复用本地端口和本地地址
    int reuse=1;
    setsockopt(m_sockfd,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));
    addfd(m_epollfd,sockfd,true);   
    ++m_user_count;
    init();
    initmysql_result();
}
void http_conn::init()
{
    m_check_state=CHECK_STATE_REQUESTLINE;
    m_linger=false;
    m_method=GET;
    m_url=0;
    m_version=0;
    m_content_length=0;
    m_host=0;   
    m_start_line=0;
    m_checked_idx=0;
    m_read_idx=0;
    m_write_idx=0;
    bytes_have_send=0;
    bytes_to_send=0;
    post_ation=-1;
    memset(m_read_buf,'\0',READ_BUFFER_SIZE);
    memset(m_write_buf,'\0',WRITE_BUFFER_SIZE);
    memset(m_real_file,'\0',FILENAME_LEN);
}

void http_conn::initmysql_result() 
{
    //先从连接池中取一个连接
    MYSQL *mysql = NULL;
    connectionRAII mysqlcon(&mysql, sql_pool);
    //在user表中检索username，passwd数据，浏览器端输入
    if (mysql_query(mysql, "SELECT username,passwd FROM user"))
    {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }

    //从表中检索完整的结果集
    MYSQL_RES *result = mysql_store_result(mysql);

    //返回结果集中的列数
    int num_fields = mysql_num_fields(result);

    //返回所有字段结构的数组
    MYSQL_FIELD *fields = mysql_fetch_fields(result);

    //从结果集中获取下一行，将对应的用户名和密码，存入map中
    while (MYSQL_ROW row = mysql_fetch_row(result))
    {
        string temp1(row[0]);
        string temp2(row[1]);
        usersmap[temp1] = temp2;
    }
}


void http_conn::process()
{
    //前面的解析部分会处理好http请求，如果需要发送文件的话就会调用do-request()
    HTTP_CODE read_ret = process_read();
    if (read_ret == NO_REQUEST)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        return;
    }
    //填写http状态行、响应头，而响应正文如果需要的话就已经执行do-requet()已经映射好了

    bool write_ret = process_write(read_ret);
    if (!write_ret)
    {
        close_conn();
    }
    //完整写入好http响应再监听可写事件，防止响应正文与前面的状态行，响应头乱序
    modfd(m_epollfd, m_sockfd, EPOLLOUT);
   
}

http_conn::HTTP_CODE http_conn::process_read()
{
    LINE_STATUS line_status=LINE_OK;
    HTTP_CODE ret=NO_REQUEST;
    char* text=0;
    while(((m_check_state==CHECK_STATE_CONTENT)&&(line_status==LINE_OK))||((line_status=parse_line())==LINE_OK))
    {
        text=get_line();
        m_start_line=m_checked_idx;
        switch(m_check_state)
        {
            case CHECK_STATE_REQUESTLINE:
            {
                ret=parse_request_line(text);
                if(ret==BAD_REQUEST)
                {
                    return BAD_REQUEST;
                }
                break;
            }
            case CHECK_STATE_HEADER:
            {
                ret=parse_headers(text);
                if(ret==BAD_REQUEST)
                {
                    return BAD_REQUEST;
                }
                else if(ret==GET_REQUEST)
                {
                    return do_request();
                }
                break;
            }
            case CHECK_STATE_CONTENT:
            {
                ret=parse_content(text);
                if(ret==GET_REQUEST)
                {
                    return do_request();
                }
                line_status=LINE_OPEN;
                break;
            }
            default:
            {
                return INTERNAL_ERROR;
            }
        }
    }
    return NO_REQUEST;
}

bool http_conn::process_write(HTTP_CODE ret)
{
    switch (ret)
    {
    case INTERNAL_ERROR:
    {
        add_status_line(500, error_500_title);
        add_headers(strlen(error_500_form));
        if (!add_content(error_500_form))
            return false;
        break;
    }
    case BAD_REQUEST:
    {
        add_status_line(400, error_400_title);
        add_headers(strlen(error_400_form));
        if (!add_content(error_400_form))
            return false;
        break;
    }
    case NO_RESOURCE:
    {
        add_status_line(400, error_404_title);
        add_headers(strlen(error_404_form));
        if (!add_content(error_404_form))
            return false;
        break;
    }
    case FORBIDDEN_REQUEST:
    {
        add_status_line(403, error_403_title);
        add_headers(strlen(error_403_form));
        if (!add_content(error_403_form))
            return false;
        break;
    }
    case FILE_REQUEST:
    {
        add_status_line(200, ok_200_title);
        if (m_file_stat.st_size != 0)
        {
            add_headers(m_file_stat.st_size);
            m_iv[0].iov_base = m_write_buf;
            m_iv[0].iov_len = m_write_idx;
            m_iv[1].iov_base = m_file_address;
            m_iv[1].iov_len = m_file_stat.st_size;
            m_iv_count = 2;
            bytes_to_send = m_write_idx + m_file_stat.st_size;
            return true;
        }
        else
        {
            const char *ok_string = "<html><body></body></html>";
            add_headers(strlen(ok_string));
            if (!add_content(ok_string))
                return false;
        }
    }
    default:
        return false;
    }
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}


http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp;
    for(;m_checked_idx<m_read_idx;++m_checked_idx)
    {
        temp=m_read_buf[m_checked_idx];
        if(temp=='\r')
        {
            //必须要先判断
            if((m_checked_idx+1)==m_read_idx)
            {
                return LINE_OPEN;
            }
            else if(m_read_buf[m_checked_idx+1]=='\n')
            {
                m_read_buf[m_checked_idx++]='\0';
                m_read_buf[m_checked_idx++]='\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
        else if(temp=='\n')
        {
            if((m_checked_idx>1)&&(m_read_buf[m_checked_idx-1]=='\r'))
            {
                m_read_buf[m_checked_idx-1]='\0';
                m_read_buf[m_checked_idx++]='\0';
            }
            return LINE_BAD;
        }
    }
    return LINE_BAD;
}

bool http_conn::read()
{
    if(m_read_idx>=READ_BUFFER_SIZE)
    {
        return false;
    }
    int bytes_read=0;
    while(true)
    {
        bytes_read=recv(m_sockfd,m_read_buf+m_read_idx,READ_BUFFER_SIZE-m_read_idx,0);
        if(bytes_read==-1)
        {
            if(errno==EAGAIN||errno==EWOULDBLOCK)
            {
                break;
            }
            return false;
        }
        else if(bytes_read==0)
        {
            return false;
        }
        m_read_idx+=bytes_read;
    }
    return true;
}

bool http_conn::write()
{
    int temp = 0;

    if (bytes_to_send == 0)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        init();
        return true;
    }

    while (1)
    {
        temp = writev(m_sockfd, m_iv, m_iv_count);

        if (temp < 0)   
        {
            if (errno == EAGAIN)
            {
                modfd(m_epollfd, m_sockfd, EPOLLOUT);
                return true; 
            }
            unmap();
            return false;
        }

        bytes_have_send += temp;
        bytes_to_send -= temp;
        if (bytes_have_send >= m_iv[0].iov_len)
        {
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }
        else
        {
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
        }

        if (bytes_to_send <= 0)
        {
            unmap();
            modfd(m_epollfd, m_sockfd, EPOLLIN);

            if (m_linger)
            {
                init();
                return true;
            }
            else
            {
                return false;
            }
        }
    }
}

http_conn::HTTP_CODE http_conn::parse_request_line(char* text)
{
    //第一个字符的位置
    m_url=strpbrk(text," ");
    if(!m_url)
    {
        return BAD_REQUEST;
    }
    *m_url++='\0';
    char* method=text;
    //忽略大小比较字符串
    if(strcasecmp(method,"GET")==0)
    {
        m_method=GET;
    }
    else if(strcasecmp(method,"POST")==0)
    {
        m_method=POST;
    }
    else
    {
        return BAD_REQUEST;
    }
    //跳过所有空格
    m_url+=strspn(m_url," ");
    //第一个字符的位置
    m_version=strpbrk(m_url," ");
    if(!m_version)
    {
        return BAD_REQUEST;
    }
    *m_version++='\0';
    //跳过所有空格
    m_version+=strspn(m_version," ");
    if(strcasecmp(m_version,"HTTP/1.0")!=0&&strcasecmp(m_version,"HTTP/1.1")!=0)
    {
        return BAD_REQUEST;
    }
    if(strcasecmp(m_version,"HTTP/1.1")==0)
    {
        m_linger=true;
    }
    if(strncasecmp(m_url,"http://",7)==0)
    {
        m_url+=7;
        //第一个字符的位置
        m_url=strchr(m_url,'/');
    }
    if(!m_url||m_url[0]!='/')
    {
        return BAD_REQUEST;
    }
    m_check_state=CHECK_STATE_HEADER;
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::parse_headers(char*text)
{
    if(text[0]=='\0')
    {
        if(m_content_length!=0)
        {
            m_check_state=CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        return GET_REQUEST;
    }
    else if(strncasecmp(text,"Content-Length:",15)==0)
    {
        text+=15;
        text+=strspn(text," ");
        m_content_length=atol(text);
    }
    else if(strncasecmp(text,"Host:",5)==0)
    {
        text+=5;
        text+=strspn(text," ");
        m_host=text;
    }
    else if(strncasecmp(text,"Connection:",11)==0)
    {
        text+=11;
         text+=strspn(text," ");
        m_linger=true;
    }
    else{   
    }
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::parse_content(char*text)
{
    if(m_read_idx>=(m_content_length+m_checked_idx))
    {
        m_context=text;
        text[m_content_length]='\0';
        return GET_REQUEST;
    }
    return NO_REQUEST;
}
http_conn::HTTP_CODE http_conn::do_request()
{
    if(m_method==POST)
    {
        if(strcmp(m_url,"/0")==0)
            m_url="/register.html";
        else  if(strcmp(m_url,"/1")==0)
            m_url="/log.html";
        //登录验证
        else if(strcmp(m_url,"/cgi_log")==0)
        {
            char user[100], password[100];
            int i;
            for (i = 5; m_context[i] != '&'; ++i)
                user[i - 5] = m_context[i];
            user[i - 5] = '\0';

            int j = 0;
            for (i = i + 10; m_context[i] != '\0'; ++i, ++j)
                password[j] = m_context[i];
            password[j] = '\0';
            pthread_mutex_lock(&maplock);
            if(usersmap.find(user)!=usersmap.end()&&usersmap[user]==password)
                m_url="/logsuccess.html";
            else m_url="/logError.html";
            pthread_mutex_unlock(&maplock);
        }
        else if(strcmp(m_url,"/cgi_register")==0)
        {
            char user[100], password[100];
            int i;
            for (i = 5; m_context[i] != '&'; ++i)
                user[i - 5] = m_context[i];
            user[i - 5] = '\0';

            int j = 0;
            for (i = i + 10; m_context[i] != '\0'; ++i, ++j)
                password[j] = m_context[i];
            password[j] = '\0';
            if(usersmap.find(user)==usersmap.end())
            {
                char *sql_insert = (char *)malloc(sizeof(char) * 200);
                strcpy(sql_insert, "INSERT INTO user(username, passwd) VALUES(");
                strcat(sql_insert, "'");
                strcat(sql_insert, user);
                strcat(sql_insert, "', '");
                strcat(sql_insert, password);
                strcat(sql_insert, "')");
                MYSQL *mysql = NULL;
                connectionRAII mysqlcon(&mysql, sql_pool);
                pthread_mutex_lock(&maplock);
                int ret = mysql_query(mysql, sql_insert);
                usersmap.insert(pair<string, string>(user, password));
                pthread_mutex_unlock(&maplock);
                free(sql_insert);
                if(!ret)
                    m_url="/log.html";
                else
                {
                    m_url="/registerError.html";
                }
                
            }
            else
            {
                m_url="/registerError.html";
            }
        }
    }
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);
    strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);
    if (stat(m_real_file, &m_file_stat) <0)
    {
        return NO_RESOURCE;
    }
    //其他用户读权限
    if (!(m_file_stat.st_mode&S_IROTH))
    {
        return FORBIDDEN_REQUEST;
    }
    //是否是一个目录
    if (S_ISDIR(m_file_stat.st_mode))
    {
        return BAD_REQUEST;
    }
    int fd = open(m_real_file, O_RDONLY);
    m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    return FILE_REQUEST;
}

void http_conn::unmap()
{
    if(m_file_address)
    {
        munmap(m_file_address,m_file_stat.st_size);
        m_file_address=0;
    }
}

bool http_conn::add_response(const char* format,...)
{
    if(m_write_idx>=WRITE_BUFFER_SIZE)
    {
        return false;
    }
    va_list arg_list;
    va_start(arg_list,format);
    int len=vsnprintf(m_write_buf+m_write_idx,WRITE_BUFFER_SIZE-1-m_write_idx,format,arg_list);
    if(len>=(WRITE_BUFFER_SIZE-1-m_write_idx))
    {
        return false;
    }
    m_write_idx+=len;
    va_end(arg_list);
    return true;
}

bool http_conn::add_status_line(int status, const char *title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}

bool http_conn::add_headers(int content_len)
{
    add_linger();
    add_response("%s\r\n","Content-type: text/html");
    add_content_length(content_len);
    add_blank_line();
}

bool http_conn::add_content_length(int content_len)
{
    return add_response("Content-Length: %d\r\n", content_len);
}

bool http_conn::add_linger()
{
    add_response("Connection: %s\r\n", (m_linger == true) ? "keep-alive" : "close");
    return true;
}

bool http_conn::add_blank_line()
{
    return add_response("%s", "\r\n");
}

bool http_conn::add_content(const char *content)
{
    return add_response("%s", content);
}
