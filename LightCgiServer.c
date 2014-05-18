#include "LightCgiServer.h"


int port;  //默认端口
int sockfd; //socket

// SIGINT信号处理函数
void handleSignal(int sigNo)
{
	printf("Shutting down the Server!\n");
	//终止sock通信的读取和传送操作
	shutdown(sockfd,2);
	close(sockfd);
	exit(sigNo);
}

// 链接终止，释放内存
void disconnect(struct socket_request *socket,FILE *fd,request_h* request_header)
{
	struct socket_request *request = socket;
	if(fd)
	{
		fclose(fd);
	}

	// 终止读取和传送操作
	shutdown(request->fd,2);
	
	// 将子线程的状态设置为detached，则该线程运行结束后将自动释放所有资源
	if(request->thread)
	{
		pthread_detach(request->thread);	
	}

	free(request_header);
	free(request);

}

// 将request header依次放到队列里，初始化队列
queue_t* alloc_queue()
{
	queue_t* queue = (queue_t*)malloc(sizeof(struct _queue));
	queue->buf = (void **)malloc(QUEUE_SIZE * sizeof(void *));
	queue->size = 0;
	queue->allSize = QUEUE_SIZE;

	return queue;
}

// 将请求添加到队列中
void queue_append(queue_t *queue,void *value)
{
	if(queue->size == queue->allSize)
	{
		queue->allSize = queue->allSize * 2; //如果队列满了，则2倍空间重新分配内存
		queue->buf = (void **)realloc(queue->buf,queue->allSize * sizeof(void*));
	}
	queue->buf[queue->size] = value;
	queue->size++;
}

// 目前队列里request headers的个数
unsigned int queue_size(queue_t* queue)
{
	return queue->size;
}

// 释放队列和队列里申请的内存
void free_queue(queue_t* queue)
{
	//free(queue->buf);
	free(queue);
}

// 返回队列里指定第id个request header
void *queue_at(queue_t* queue,unsigned int id)
{
	if(id >= queue->size)
	{
		return NULL;
	}
	return queue->buf[id];

}

// 释放队列内容
void delete_queue(queue_t* queue)
{
	unsigned int i;
	for(i = 0; i < queue_size(queue);++i)
	{
		free(queue_at(queue,i));
	}
	
	free_queue(queue);
}

// 将十六进制数，比如b转为对应的数11
char from_hex(char c)
{
	return isdigit(c) ? c - '0' : tolower(c) - 'a' + 10;
}

// 将Response格式化输出到fd
void generic_response(FILE* fd,char* status,char* message)
{
	fprintf(fd,"HTTP/1.1 %s\r\n"
			"Content-Type: text/plain\r\n"
			"Content-Length: %d\r\n"
			"\r\n"
			"%s\r\n",status,strlen(message),message);
}

// 关闭管道并且等待子进程，防止产生僵尸进程
void* wait_pid(void* data)
{
	struct cgi_data* cgi_d = (struct cgi_data*)data;
	// 等待进程号为cgi->pid子进程完成，防止产生僵尸进程
	int status;
	waitpid(cgi_d->pid,&status,0);
	
	// 关闭管道
	close(cgi_d->fd);
	close(cgi_d->fd2);
	return NULL;	
}	

// 不支持的HTTP方法请求处理
void unsupport(struct socket_request* socket,queue_t* queue,FILE* fd,request_h* request_header)
{
	generic_response(fd,"501 Not Implemented","Not implemented: The request type was not understood by the server.\n");
	delete_queue(queue);
	disconnect(socket,fd,request_header);
}

// 处理request header，判断请求类型
int processHeader(struct socket_request* socket,queue_t* queue,FILE* fd,int* type_width,request_h* request_header)
{
	unsigned int i;
	for(i = 0; i < queue_size(queue); ++i)
	{
		char *str = (char*)(queue_at(queue,i));	
		//查找header里的:
		char* colon = strstr(str,": ");
		// 如果不存在: 且不为第一行，则处理请求方式
		if(!colon)
		{
			// 处理Reqest Line
			if(i > 0)
			{
				generic_response(fd,"400 Bad Request","Bad Request: A header line was missing colon.");
				delete_queue(queue);
				disconnect(socket,fd,request_header);
				return -1;
			}
			// 根据首字节，判断HTTP 请求方式，即GET，POST等等
			switch(str[0])
			{
				case 'G':
					if(strstr(str,"GET ") == str)
					{
						*type_width = 4;
						request_header->request_type = 1;
					}
					else
					{
						// 没有发现GET 类型，跳到不支持的类型处理函数，下同理
						unsupport(socket,queue,fd,request_header);
						return -1;
					}
					break;
				case 'P':
					if(strstr(str,"POST ") == str)
					{
						// POST 加上空格，为5个字节，向后移动5字节，下同
						*type_width = 5;
						request_header->request_type = 2;
					}
					else
					{
						unsupport(socket,queue,fd,request_header);
						return -1;
					}
					break;
				case 'H':
					if(strstr(str,"HEAD ") == str)
					{
						*type_width = 5;
						request_header->request_type = 3;
					}
					else
					{
						unsupport(socket,queue,fd,request_header);
					}
					break;
				default:
					unsupport(socket,queue,fd,request_header);
					return -1;
					//break;
			}
			
			// 向后移动字节
			request_header->filename = str + (*type_width);
		
			// 网站首页一般为 '\'，其他为网址后的URI
			if(request_header->filename[0] == ' ' || request_header->filename[0] == '\r' || request_header->filename[0] == '\n')
			{
				generic_response(fd,"400 Bad Request","Bad Request: No File.\n");
				delete_queue(queue);
				disconnect(socket,fd,request_header);
				return -1;
			}

			// 判断HTTP的版本，有HTTP 1.0 和 HTTP1.1，现在多为1.1版本
			request_header->http_version = strstr(request_header->filename,"HTTP/");
			if(!(request_header->http_version))
			{
				// No Http Version
				generic_response(fd,"400 Bad Request","Bad Request: No HTTP Version.\n");
				delete_queue(queue);
				disconnect(socket,fd,request_header);
				return -1;
			}

			// 得到filename的值
			request_header->http_version[-1] = '\0';
			char *tmp;
			tmp = strstr(request_header->http_version,"\r\n");
			if(tmp)
			{
				tmp[0] = '\0';
			}
			tmp = strstr(request_header->http_version,"\n");
			if(tmp)
			{
				tmp[0] = '\0';
			}

			//获取查询的字符串，如http://xxx.com/?s=python
			request_header->querystring = strstr(request_header->filename,"?");
			if(request_header->querystring)
			{
				request_header->querystring++;
				request_header->querystring[-1] = '\0';	
			}	
		}
		else
		{
			// 如果第一行请求出现：，表示出错
			if(i == 0)
			{
				// Request Line 错误
				generic_response(fd,"400 Bad Request","Bad Request: First Line was not a Correct Request.\n");
				delete_queue(queue);
				disconnect(socket,fd,request_header);
				return -1;
			}
			// 处理header，指针移动向后移动两位，指向header各响应值
			colon[0] = '\0';
			colon = colon + 2;
			
			// 判断是否包含此请求类型
			if(strcmp(str,"Host") == 0)
			{
				request_header->host = colon;
			}
			else if(strcmp(str,"Content-Type") == 0)
			{
				// MIME-Type Message
				request_header->c_type = colon;
			}
			else if(strcmp(str,"Cookie") == 0)
			{
				request_header->c_cookie = colon;
			}
			else if(strcmp(str,"User-Agent") == 0)
			{
				request_header->c_uagent = colon;
			}
			else if(strcmp(str,"Content-Length") == 0)
			{
				request_header->c_length = atol(colon);
			}
			else if(strcmp(str,"Referer") == 0)
			{
				request_header->c_referer = colon;
			}

		}

	}

	// 如果HTTP请求类型不存在
	if(!(request_header->request_type))
	{
		unsupport(socket,queue,fd,request_header);
		return -1;
	}
	return 0;
}

// 解析文件和URL编码
int request_file(struct socket_request* socket,queue_t* queue,FILE* fd,request_h* request_header)
{
	// 如果请求地址URI为空，或者包含' 空格等特殊字符，则请求失败
	//printf("%s\n %s\n",request_header->querystring,request_header->filename);
	if(!(request_header->filename) || strstr(request_header->filename,"'") || strstr(request_header->filename," ") || (request_header->querystring && strstr(request_header->querystring," ")))
	{
		generic_response(fd,"400 Bad Request","Bad Request: Filename was Error!");
		delete_queue(queue);
		disconnect(socket,fd,request_header);
		return -1;
	}
	
	request_header->_filename = calloc(sizeof(char) * (strlen(PAGES) + strlen(request_header->filename) + 2),1);
	// 连接pages + 文件名，strcat返回首地址
	strcat(request_header->_filename,PAGES);
	strcat(request_header->_filename,request_header->filename);
	
	// 如果文件名+目录里包含 % 字符，表示为中文，需要作URL解码
	if(strstr(request_header->_filename,"%"))
	{
		char buf[1024] = {0};
		char *pstr = request_header->_filename;
		char *pbuf = buf;
		while(*pstr)
		{
			if(*pstr == '%')
			{
				if(pstr[1] && pstr[2])
				{
					*pbuf++ = from_hex(pstr[1]) << 4 | from_hex(pstr[2]);
					pstr += 2;
				}	
			}
			else if(*pstr == '+')
			{
				*pbuf++ = ' ';
			}
			else
			{
				*pbuf++ = *pstr;
			}
			pstr++;
		}	
		*pbuf = '\0';
		request_header->_filename = (char*)buf;
	}
	//printf("request_header->filename:%s\n",request_header->filename);
	// 获取文件后缀
	request_header->ext = request_header->filename + 1;
	// 循环得出最后的文件后缀开始
	while(strstr(request_header->ext + 1,"."))
	{
		request_header->ext = strstr(request_header->ext + 1,".");
	}
	if(request_header->ext == request_header->filename + 1)
	{
		
		request_header->ext = NULL;
	}
	
	return 0;	
}

// 设置CGI环境变量
int set_env(struct socket_request* socket,queue_t* queue,request_h* request_header)
{
	if(!(request_header->host))
	{
		char hostname[1024];
		gethostname(hostname,sizeof(hostname));
		setenv("SERVER_NAME",hostname,1);
		setenv("HTTP_HOST",hostname,1);
	}
	else
	{
		setenv("SERVER_NAME",request_header->host,1);
		setenv("HTTP_HOST",request_header->host,1);
	}
	setenv("GATEWAY_INTERFACE","CGI/1.0",1);
	setenv("SERVER_PROTOCOL","HTTP/1.1",1);
	char c_port[20];
	sprintf(c_port,"%d",port);
	setenv("SERVER_PORT",c_port,1);
	// 设置请求类型
	if(request_header->request_type == 1)
	{
		setenv("REQUEST_METHOD","GET",1);
	}
	else if(request_header->request_type == 2)
	{
		setenv("REQUEST_METHOD","POST",1);
	}
	else if(request_header->request_type == 3)
	{
		setenv("REQUEST_METHOD","HEAD",1);
	}
	// URL中查询字符串，即问号后面那个
	if(request_header->querystring)
	{
		if(strlen(request_header->querystring))
		{
			setenv("QUERY_STRING",request_header->querystring,1);
		}
		else
		{
			setenv("QUERY_STRING","",1);
		}
	}
	// 去掉之前目录，获得单文件名字
	while(strstr(request_header->_filename,"/"))
	{
		request_header->_filename = strstr(request_header->_filename,"/") + 1;
	}
	request_header->_filename[-1] = '\0';
	char fullpath[1024 + strlen(request_header->_filename)];
	getcwd(fullpath,1023);
	strcat(fullpath,"/"PAGES"/");
	//fprintf(stderr,"%s\n",request_header->filename);
	strcat(fullpath,request_header->_filename);
	setenv("PATH_TRANSLATED",fullpath,1);
	setenv("SCRIPT_NAME",request_header->filename,1);
	setenv("SCRIPT_FILENAME",request_header->_filename,1);
	setenv("REDIRECT_STATUS","200",1);
	char c_length[100];
	c_length[0] = '\0';
	sprintf(c_length,"%lu",request_header->c_length);
	setenv("CONTENT_LENGTH",c_length,1);
	if(request_header->c_type)
	{
		setenv("CONTENT_TYPE",request_header->c_type,1);
	}
	// 获取远程客户端地址和主机名字
	
	struct hostent* client;
	client = gethostbyaddr((const char*)&socket->remote_addr.sin_addr.s_addr,sizeof(socket->remote_addr.sin_addr.s_addr),AF_INET);
	setenv("REMOTE_HOST",client->h_name,1);
	//printf("k%s\n",client->h_name);
	
	//printf("%s\n",request_header->_filename);
	setenv("REMOTE_ADDR",inet_ntoa(socket->remote_addr.sin_addr),1);
	// 设置Cookie
	if(request_header->c_cookie)
	{
		setenv("HTTP_COOKIE",request_header->c_cookie,1);
	}
	// 设置 USER_AGENT
	if(request_header->c_uagent)
	{
		setenv("HTTP_USER_AGENT",request_header->c_uagent,1);
	}

	// 设置 Referer
	if(request_header->c_referer)
	{
		setenv("HTTP_REFERER",request_header->c_referer,1);
	}
	char execute[1024];
	execute[0] = '\0';
	//sprintf(execute,"./%s",request_header->_filename);
	sprintf(execute,"%s",fullpath);
	// execlp 从PATH 环境变量中查找文件并执行，执行成功则不返回，否则返回-1
	execlp(execute,execute,(char*)0);
	// 如果执行失败，则继续下面操作
	fprintf(stderr,"[warn] Failed to execute CGI Script:%s?%s.\n",fullpath,request_header->querystring);	
	// 回收内存
	delete_queue(queue);
	pthread_detach(socket->thread);
	free(socket);	

	return -1;
}


// 判断_filename 是不是目录
int check_dir(FILE* fd,request_h* request_header,struct stat* Stats)
{
	if(request_header->_filename[strlen(request_header->_filename) - 1] != '/')
	{
		// 目录结尾无 '/'
		fprintf(fd,"HTTP/1.1 301 Moved Permanently\r\n");
		fprintf(fd,"Location: %s/\r\n",request_header->filename);
		fprintf(fd,"Content-Length: 0\r\n\r\n");			
		
		return -1;
	}
	else
	{
		// 无默认文件，列出目录列表
		struct dirent **files = {0};
		int filecount = -1;
		filecount = scandir(request_header->_filename,&files,0,alphasort);
			// 打印目录列表，先发送HTTP Response
		fprintf(fd,"HTTP/1.1 200 OK\r\n");
		fprintf(fd,"Content-Type: text/html\r\n");

		// 为显示的HTML内容分配一些内存
		char *html = malloc(1024);
		html[0] = '\0';
		strcat(html,"<!doctype html><html><head><title>Directory Listing</title></head><body>");
		// 打印列表内容，并构造html
		unsigned int i;
		for(i = 0; i < filecount; ++i)
		{
			char fullname[strlen(request_header->_filename) + 1 + strlen(files[i]->d_name) + 1];
			sprintf(fullname,"%s/%s",request_header->_filename,files[i]->d_name);
				// 忽略目录下的目录
			if(stat(fullname,&(*Stats)) == 0 && S_ISDIR((*Stats).st_mode))
			{
				free(files[i]);
				continue;
			}

			char _file[2 * strlen(files[i]->d_name) + 64];
			sprintf(_file,"<a href=\"%s\">%s</a><br>\n",files[i]->d_name,files[i]->d_name);
			// 重新分配内存，将上述内容添加到末尾
			html = realloc(html,strlen(html) + strlen(_file) + 1);
			strcat(html,_file);
			free(files[i]);
		}
		free(files);
		html = realloc(html,strlen(html) + 64);
		strcat(html,"</body></html>");
	
		// 发送Response 
		fprintf(fd,"Content-Length: %d\r\n",(sizeof(char) * strlen(html)));
		fprintf(fd,"\r\n");
		fprintf(fd,"%s",html);
		free(html);
	}
	return 0;
}

// 确定MIME类型，根据请求类型发送Response
int determine_mime(queue_t* queue,FILE* fd,FILE* content,request_h* request_header)
{
	if(request_header->ext)
	{
		// 判断请求文件类型，对不同的请求，Response不同的值
		if(!strcmp(request_header->ext,".htm") || !strcmp(request_header->ext,".html"))
		{
			fprintf(fd,"Content-Type: text/html\r\n");	
		}
		else if(!strcmp(request_header->ext,".css"))
		{
			fprintf(fd,"Content-Type: text/css\r\n");
		}
		else if(!strcmp(request_header->ext,".png"))
		{
			fprintf(fd,"Content-Type: image/png\r\n");
		}
		else if(!strcmp(request_header->ext,".jpg"))
		{
			fprintf(fd,"Content-Type: image/jpeg\r\n");
		}
		else if(!strcmp(request_header->ext,".gif"))
		{
			fprintf(fd,"Content-Type: image/gif\r\n");
		}
		else if(!strcmp(request_header->ext,".pdf"))
		{
			fprintf(fd,"Content-Type: application/pdf\r\n");
		}
		// HTML5里面的东西，好像是做缓存用的
		else if(!strcmp(request_header->ext,".manifest"))
		{
			fprintf(fd,"Content-Type: text/cache-manifest\r\n");
		}
		else
		{
			fprintf(fd,"Content-Type: text/unknown\r\n");
		}
	}
	else
	{
		fprintf(fd,"Content-Type: text/unknown\r\n");
	}

	// HEAD request，只需要读取headers
	if(request_header->request_type == 3)
	{
		fprintf(fd,"\r\n");
		fclose(content);
		fflush(fd);
		delete_queue(queue);
		
		return 1;	
	}

	fseek((content),0,SEEK_END);
	long size = ftell(content);
	fseek((content),0,SEEK_SET);

	// 发送出去header后的文本长度
	fprintf(fd,"Content-Length: %ld\r\n",size);
	fprintf(fd,"\r\n");	
	
	char buf[HTML_SIZE];
	while(!feof(content))
	{
		size_t n = fread(buf,1,HTML_SIZE - 1,content);
		fwrite(buf,1,n,fd);
	}
	fprintf(fd,"\r\n");
	fclose(content);
	return 0;
}

// 父进程读取客户端动态脚本传来的数据（Form表单数据），通过管道传入到子进程
void pipe_trans(queue_t* queue,FILE* fd,FILE* cgi_w,request_h* request_header)
{
	// 读取客户端数据，并通过管道传入到子进程，传输POST数据
	if(request_header->c_length > 0)
	{
		size_t total = 0;
		char buf[CGI_POST];
		while((total < request_header->c_length) && (!feof(fd)))
		{
			size_t left = request_header->c_length - total;
		   	if(left > CGI_POST)
			{
				// 请求太大
				left = CGI_POST;
			}
			size_t n = fread(buf,1,left,fd);
			total += n;
			
			// 向管道写入数据
			fwrite(buf,1,n,cgi_w);		
		}	
	}
	
	if(cgi_w)
	{
		fclose(cgi_w);
	}	

}

// 子进程通过管道将数据传送过来，父进程解析HTTP头
int read_header(struct socket_request* socket,queue_t* queue,FILE* fd,FILE* cgi_r,request_h* request_header,pthread_t* waitthread)
{
	char buf[HEADER_SIZE];
	if(!cgi_r)
	{
		generic_response(fd,"500 Internal Server Error","Failed to execute CGI Script.\n");
		pthread_detach(*waitthread);
		fflush(fd);
		delete_queue(queue);
		return 1;	
	}	
	fprintf(fd,"HTTP/1.1 200 OK\r\n");
	unsigned int i = 0;
	
	// 读取子进程通过管道传输的数据，并写到socket中
	while(!feof(cgi_r))
	{
		char* in = fgets(buf,HEADER_SIZE - 1,cgi_r);
		if(!in)
		{
			fprintf(stderr,"[warn] Read nothing [%d on %p].\n",ferror(cgi_r),cgi_r);
			buf[0] = '\0';
			break;
		}

		if(!strcmp(in,"\r\n") || !strcmp(in,"\n"))
		{
			buf[0] = '\0';
			break;
		}

		if(!strcmp(in,": ") && !strcmp(in,"\r\n"))
		{
			fprintf(stderr,"[warn] Reuqest Line was too long or Error %zu.\n",strlen(buf));
			break;
		}

		fwrite(in,1,strlen(in),fd);
		++i;
	}
	if(i < 1)
	{
		fprintf(stderr,"[warn] CGI Script didn't give us headers.\n");
	}

	
	if(feof(cgi_r))
	{
		fprintf(stderr,"[warn] The End of File,May be the Pipe is closed.\n");
	}

	// HEAD请求，只请求页面的首部
	if(request_header->request_type == 3)
	{
		fprintf(fd,"\r\n");
		pthread_detach(*waitthread);	
		fflush(fd);
		delete_queue(queue);
		return 1;
	}

	int mode = 0;
	// 如果HTTP协议为HTTP/1.1，则设置Transfer-Encodingchunked
	// 这样我们可以分块发送，而不用一次全部发送
	if(!strcmp(request_header->http_version,"HTTP/1.1"))
	{
		fprintf(fd,"Transfer-Encoding: chunked\r\n");
	}
	else
	{
		//  如果不是HTTP/1.1，即为HTTP/1.0，
		//  则没实现长链接，Connection应设置为close	
		fprintf(fd,"Connection: close\r\n\r\n");
		mode = 1;
	}
	
	
	if(strlen(buf) > 0)
	{
		fprintf(stderr,"[warn] Trying to dump remaing content.\n");
		// 其内容为一个chunk，用CRLF隔开，即\r\n
		// ASCII值打印
		fprintf(fd,"\r\n%zX\r\n",strlen(buf));
		fwrite(buf,1,strlen(buf),fd);
	}	

	// 继续从子进程的CGI 应用程序读取数据
	// 如果为HTTP/1.1，则以chunks形式发送
	while(!feof(cgi_r))
	{
		size_t n_read = -1;
		n_read = fread(buf,1,HEADER_SIZE-1,cgi_r);
		if(n_read < 1)
		{
			fprintf(stderr,"[warn] Read nothing from CGI Application.\n");
			break;
		}
		// 如果为HTTP/1.1
		if(mode == 0)
		{
			fprintf(fd,"\r\n%zX\r\n",n_read);
		}
		fwrite(buf,1,n_read,fd);
	}

	if(mode == 0)
	{
		// 使用 0 字节长度的块结束chunked
		fprintf(fd,"\r\n0\r\n\r\n");
	}

	pthread_detach(*waitthread);
	if(cgi_r)
	{
		fclose(cgi_r);
	}
	
	// 如果是HTTP/1.1就保持长链接，不断开，只需要释放内存，继续while循环即可
	if(mode == 0)
	{
		fflush(fd);
		delete_queue(queue);
		
		// 此处存疑，稍后处理，即返回值，应该继续执行
		return -1;
	}
	// 如果是HTTP/1.0，断开链接
	else
	{
		delete_queue(queue);
		disconnect(socket,fd,request_header);
		return -1;
	}

}

int parse_html(struct socket_request* socket,queue_t* queue,FILE* fd,request_h* request_header)
{
	//printf("%s\n",request_header->_filename);
	FILE *content = fopen(request_header->_filename,"rb");
	// 如果打不开文件，显示404错误，或者也可能是403权限问题，这里用400表示
	if(!content)
	{
		generic_response(fd,"400 Bad Request","Bad Request: No File Or Forbidden.\n");
		fflush(fd);
		delete_queue(queue);
		// 此处。。。 free _filename
		// disconnect(socket,fd,request_header);
		// 此处有问题
		return 1;
	}
	else
	{
		struct stat Stats;
		// 判断文件是否可执行
		if(stat(request_header->_filename,&Stats) == 0  && (Stats.st_mode & S_IXOTH))
		{
			fclose(content);
			
			// 使用双向管道
			int pipe_r[2];
			int pipe_w[2];
			if(pipe(pipe_r) < 0)
			{
				fprintf(stderr,"Failed to create read pipe!\n");
			}
			if(pipe(pipe_w) < 0)
			{
				fprintf(stderr,"Failed to create write pipe!\n");
			}
			// 使用多进程，父子进程
			pid_t pid = 0;
			pid = fork();

			// 子进程
			if(pid == 0)
			{
				// 重定向
				dup2(pipe_r[0],STDIN_FILENO);
				dup2(pipe_w[1],STDOUT_FILENO);
				// 关闭
				close(pipe_r[1]);
				close(pipe_w[0]);	
				// 已重定向，向STDOUT发送消息，即通过管道向父进程发送消息
				// 控制缓存的时间，即立即过期
				fprintf(stdout,"Expires: -1\r\n");
				// 进入www目录下
				char* dir = request_header->_filename; 
				char wwwroot[1024];
				getcwd(wwwroot,1024);
				strcat(wwwroot,"/"PAGES);
				chdir(dir);
				// 设置DOCUMENT_ROOT 环境变量，嫌麻烦，没有统一处理，还得传参
				setenv("DOCUMENT_ROOT",wwwroot,1);
				int id = set_env(socket,queue,request_header);
				if(id == -1)
				{
					// 即子进程脚本没有执行
					return -1;
				}
			}

			// 父进程
			struct cgi_data* cgi_d = malloc(sizeof(struct cgi_data));
			cgi_d->pid = pid;
			// 待关闭
			cgi_d->fd = pipe_w[1];
			cgi_d->fd2 = pipe_r[0];
			pthread_t waitthread;
			// 创建一个线程，关闭父进程里的文件描述符，使其成为双向管道
			pthread_create(&waitthread,NULL,wait_pid,(void*)(cgi_d));
			
			// cgi_r 读取CGI 程序的输出，cgi_w映射到CGI程序的标准输入
			FILE* cgi_r = fdopen(pipe_w[0],"r");
			FILE* cgi_w = fdopen(pipe_r[1],"w");
			
			// 管道
			pipe_trans(queue,fd,cgi_w,request_header);
			
			// 读取header
			int id = read_header(socket,queue,fd,cgi_r,request_header,&waitthread);
			if(id == 1)
			{
				return 1;
			}
			else if(id == -1)
			{
				return -1;
			}
		
		}
		fprintf(fd,"HTTP/1.1 200 OK\r\n");
	}
	// 确定MIME类型
	if((determine_mime(queue,fd,content,request_header)) == 1)
	{
		return 1;
	}
	else
	{
		return 0;
	}

	return 0;
}

// 将 request headers 放到队列当中，直到客户端断开连接
int readRequest(struct socket_request* socket,queue_t* queue,FILE *fd,request_h* request_header)
{
	char buf[HEADER_SIZE];
	while(!feof(fd))
	{
		// 当读到一个换行符或EOF，结束读取
		char* in = fgets(buf,HEADER_SIZE - 2,fd);
		// 到达文件末尾
		if(!in)
		{
			break;
		}

		// 到达headers末尾
		if(!strcmp(in,"\r\n") || !strcmp(in,"\n"))
		{
			break;
		}

		// request line后面有个\n，判断是否存在request line
		if(!strstr(in,"\n"))
		{
			generic_response(fd,"400 Bad Request","Bad Request: Request Line was too long.\n");	
			delete_queue(queue);
			disconnect(socket,fd,request_header);
			return -1;	
		}
		
		char* request_line = malloc((strlen(buf)+1) * sizeof(char));
		strcpy(request_line,buf);
		// 将request line 存储到queue里
		//printf("%s\n",request_line);
		queue_append(queue,(void*)request_line);
	}
	// Socket无文件结束标志，如果出现这个表示客户端关闭连接
	if(feof(fd))
	{
		delete_queue(queue);
		disconnect(socket,fd,request_header);	
		return -1;
	}
	return 0;
}

// 线程处理函数，处理每一个连接请求
void *handleRequest(void *socket)
{
	// GET POST 占的宽度
	int type_width = 0;
	
	request_h *request_header = (request_h*)malloc(sizeof(struct _request_h));
	request_header->filename = NULL;
	request_header->querystring = NULL;
	request_header->request_type = 0;
	request_header->_filename = NULL;
	request_header->ext = NULL;
	request_header->host = NULL;
	request_header->http_version = NULL;
	request_header->c_length = 0;
	request_header->c_type = NULL;
	request_header->c_cookie = NULL;
	request_header->c_uagent = NULL;
	request_header->c_referer = NULL;

	struct socket_request *request = (struct socket_request*)socket;
	// 将socket文件描述符转换为标准的文件描述符，因为socket描述符不能使用标准I/O fopen打开
	FILE *fd = fdopen(request->fd,"r+");
	if(!fd)
	{
		fprintf(stderr,"Transfer the Socket fd to File Error!\n");
		disconnect(request,fd,request_header);
		return NULL;
	}
	
	// 读取requests请求，直到客户端断开连接
	while(1)
	{
		queue_t *queue = alloc_queue();				
	
		// 读取request请求
		if((readRequest(request,queue,fd,request_header)) == -1)
		{
			return NULL;
		}
		// 处理headers
		if ((processHeader(request,queue,fd,&type_width,request_header)) == -1)
		{
			return NULL;
		}
		//printf("%s\n%s\n%s\n",request_header->_filename,request_header->http_version,request_header->ext);
		// 解析文件和编码
		if((request_file(request,queue,fd,request_header)) == -1)
		{
			return NULL;
		}

		// 判断是否是目录
		struct stat Stats;
		if(stat(request_header->_filename,&Stats) == 0 && S_ISDIR(Stats.st_mode))
		{
			if(check_dir(fd,request_header,&Stats) == -1)
			{
				return NULL;
			}
		}
		else
		{
			// 解析HTML ，返回-1，表示错误，线程不在执行
			int id = parse_html(request,queue,fd,request_header);
			if(id == -1)
			{
				return NULL;
			}
			// 返回1，表示长链接
			else if(id == 1)
			{
				continue;
			}
	  
		}
	}

}


int main(int argc,char *argv[])
{
	port = PORT;

	// 如果命令行参数个数大于1，即改变默认端口号，则修改默认端口号
	if(argc > 1)
	{
		port = atoi(argv[1]);
	}

	// 初始化TCP Socket
	struct sockaddr_in addr;
	if((sockfd = socket(AF_INET,SOCK_STREAM,0)) < 0)
	{
		fprintf(stderr,"Create Socket Error!\n");
		return -1;
	}

	bzero(&addr,sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr.s_addr = htonl(INADDR_ANY);

	// 设置socket关闭后，仍可继续重用该socket
	int reuseaddr = 1;
	if(setsockopt(sockfd,SOL_SOCKET,SO_REUSEADDR,(const char*)&reuseaddr,sizeof(int)) < 0)
	{
		close(sockfd);
		return -1;
	}
	
	//  Bind the socket
	if(bind(sockfd,(struct sockaddr*)&addr,sizeof(addr)) < 0)
	{
		fprintf(stderr,"Failed to bind socket to the %d port!\n",port);
		return -1;
	}

	// 监听来自浏览器的请求
	listen(sockfd,10);

	printf("The Cgi Server is Listening the %d port.\n",port);

	// 捕捉终端CTRL+C产生的SIGINT信号，使用我们自己的处理函数
	signal(SIGINT,handleSignal);
	
	// 忽略SIGPIPE信号，否则会异常终止
	signal(SIGPIPE,SIG_IGN);

	// 使用多线程，循环处理每一个accept请求
	while(1)
	{
		struct socket_request *coming = calloc(sizeof(struct socket_request),1);
		unsigned int len;
		len = sizeof(coming->remote_addr);
		coming->fd = accept(sockfd,(struct sockaddr*)&(coming->remote_addr),&len);
		pthread_create(&(coming->thread),NULL,handleRequest,(void *)(coming));
	
	}

}

