#ifndef LightCgiServer_H_
#define LightCgiServer_H_

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <signal.h>
#include <pthread.h>
#include <sys/wait.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <dirent.h>
#include <netdb.h>
#include <ctype.h>

struct socket_request
{
	int fd;  //socket 文件描述符
	struct sockaddr_in remote_addr; //远程连接地址
	pthread_t thread;	//线程
};

typedef struct _queue
{
	void **buf;
	unsigned int size;
	unsigned int allSize;
}queue_t;


// Request Variables
typedef struct _request_h
{
	char* filename;    //Filename was received(ie,/index.html)
	char* querystring; //Query String,URL Encoded
	int request_type;     //Request type,GET = 0,POST = 1,HEAD = 2...
	char* _filename;   //FileName relative to server
	char* ext;  //Extension for requested file
	char* host;  //Hostname for request
	char* http_version;  //HTTP version used in request
	unsigned long c_length; //Content-Type,usually for POST
	char* c_type;   //Content-Type,usually for POST
	char* c_cookie; //HTTP_COOKIE
	char* c_uagent; //User-Agent,for cgi
	char* c_referer; //Referer,for cgi
}request_h;

struct cgi_data
{
	int fd;   // 读描述符
	int fd2;  // 写描述符
	int pid;  // Process ID
};

#define PORT 80  //定义服务器默认端口号
#define HEADER_SIZE 10240//request请求头的最大字节
#define QUEUE_SIZE 1024  //队列个数
#define CGI_POST 10240   //动态脚本传送的数据
#define HTML_SIZE  10240  //HTML文本最大字节
#define PAGES "www"   //HTML文件目录    


void handleSignal(int sigNo);
void disconnect(struct socket_request* socket,FILE *fd,request_h* request_header);
queue_t* alloc_queue();
void queue_append(queue_t* queue,void* value);
unsigned int queue_size(queue_t* queue);
void free_queue(queue_t* queue);
void* queue_at(queue_t* queue,unsigned int id);
void delete_queue(queue_t* queue);
char from_hex(char c);
void generic_response(FILE* fd,char* status,char* message);
void* wait_pid(void* data);
void unsupport(struct socket_request* socket,queue_t* queue,FILE* fd,request_h* request_header);
int processHeader(struct socket_request* socket,queue_t* queue,FILE* fd,int* type_width,request_h* request_header);
int request_file(struct socket_request* socket,queue_t* queue,FILE* fd,request_h* request_header);
int set_env(struct socket_request* socket,queue_t* queue,request_h* request_header);
int check_dir(FILE* fd,request_h* request_header,struct stat* Stats);
int determine_mime(queue_t* queue,FILE* fd,FILE* content,request_h* request_header);
void pipe_trans(queue_t* queue,FILE* fd,FILE* cgi_w,request_h* request_header);
int read_header(struct socket_request* socket,queue_t* queue,FILE* fd,FILE* cgi_r,request_h* request_header,pthread_t* waitthread);
int parse_html(struct socket_request* socket,queue_t* queue,FILE* fd,request_h* request_header);
int readRequest(struct socket_request* socket,queue_t* queue,FILE* fd,request_h* request_header);
void* handleRequest(void* socket);

#endif

