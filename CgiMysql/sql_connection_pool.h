#ifndef _CONNECTION_POOL_
#define _CONNECTION_POOL_

#include <stdio.h>
#include <list>
#include <error.h>
#include <string.h>
#include <iostream>
#include <string>
#include "../lock/locker.h"
#include "../log/log.h"
#include <mysql/mysql.h>
#include <stdio.h>
#include <string>
#include <string.h>
#include <stdlib.h>
#include <list>
#include <pthread.h>
#include <iostream>
#include "sql_connection_pool.h"

using namespace std;


class connection_pool
{
public:
 	//获取数据库连接
	MYSQL *GetConnection();				
	//释放连接
	bool ReleaseConnection(MYSQL *conn); 
	//获取连接
	int GetFreeConn();					
	//销毁所有连接 
	void DestroyPool();					 

	//单例模式
	static connection_pool *GetInstance();

	void init(string url, string User, string PassWord, string DataBaseName, int Port, int MaxConn); 

private:
	connection_pool();
	~connection_pool();

	//最大连接数
	int m_MaxConn;  
	//当前已使用的连接数
	int m_CurConn;  
	//当前空闲的连接数
	int m_FreeConn; 
	locker lock;
	 //连接池
	list<MYSQL *> connList;
	sem reserve;

public:
	//主机地址
	string m_url;			 
	//数据库端口号
	string m_Port;		
	//登陆数据库用户名
	string m_User;		 
	//登陆数据库密码
	string m_PassWord;	 
	//使用数据库名
	string m_DatabaseName; 
	//日志开关
	int m_close_log;	
};

class connectionRAII{

public:
	connectionRAII(MYSQL **con, connection_pool *connPool);
	~connectionRAII();
	
private:
	MYSQL *conRAII;
	connection_pool *poolRAII;
};

#endif
