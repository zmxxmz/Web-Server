server:main.cpp ./http/http_conn.cpp ./log/log.cpp ./CgiMysql/sql_connection_pool.cpp
	g++ -o server $^  -lpthread -lmysqlclient
clean:
	rm -r server