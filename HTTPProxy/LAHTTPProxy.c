//
//  LAHTTPProxy.c
//  HTTPProxy
//
//  Created by MacPu on 2016/12/29.
//  Copyright © 2016年 MacPu. All rights reserved.
//

#include "LAHTTPProxy.h"
#include "LAThreadPool.h"
#include "LASemaphore.h"
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <string.h>
#include <math.h>
#include <sys/socket.h>
#include <time.h>
#include <signal.h>
#include <getopt.h>
#include <sys/ioctl.h>
#include <sys/un.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>

typedef int http_socket;

#define http_socket_failed -1

#define MAX_HOST_NAME 512
#define MAXSIZE 65535
#define DEFAULTPORT 80
#define HEADLEN 7    // http://

LAThreadPoolRef *_thpool;
http_socket _proxy_socket;

typedef struct http_request{
    http_socket client;
    http_socket server;
    LASemaphoreRef *semaphore;
}http_rquest_t;


void proxy_log(char *log_text)
{
#if DEBUG
//    fprintf(stderr, "[HTTPProxy] %s \r\n",log_text);
    printf("[HTTPProxy] %s \r\n",log_text);
#endif
}


/**
 初始化一个监听的代理socket

 @param port 监听的端口号
 @return socket
 */
http_socket proxy_socket_init(int port)
{
    http_socket fd = socket(AF_INET, SOCK_STREAM, 0);
    if(fd == http_socket_failed){
        proxy_log("proxy_socket_init(): create proxy socket failed");
        return http_socket_failed;
    }
    
    struct sockaddr_in addr;
    bzero((char *)&addr, sizeof(addr));
    addr.sin_len = sizeof(addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;
    
    socklen_t socklen = sizeof(addr);
    
    int on = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) == -1) {
        close(fd);
        return http_socket_failed;
    }
    
    if (bind(fd, (struct sockaddr*)&addr,socklen) != 0) {
        close(fd);
        proxy_log("proxy_socket_init(): bind proxy socket to addr failed.");
        return http_socket_failed;
    }
    
    if (listen(fd, 512) != 0) {
        close(fd);
        proxy_log("proxy_socket_init(): listen proxy socket failed.");
        return http_socket_failed;
    }
    
    return fd;
}


/**
 根据host名连接host

 @param host_name host的名称
 @param port host 的端口号
 @return host socket
 */
http_socket connect_host(char *host_name, int port)
{
    //先找到host的IP地址
    struct hostent *host = gethostbyname(host_name);
    if(!host){
        return http_socket_failed;
    }
    struct in_addr inad = *((struct in_addr*) *host->h_addr_list);
    struct sockaddr_in addr;
    bzero((char *)&addr, sizeof(addr));
    addr.sin_len = sizeof(addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inad.s_addr;
    
    //然后连接
    http_socket host_socket = socket(AF_INET, SOCK_STREAM, 0);
    if(http_socket_failed == host_socket){
        close(host_socket);
        proxy_log("init server scoket failed");
        return http_socket_failed;
    }
    
    int on = 1;
    setsockopt(host_socket, IPPROTO_TCP, SO_NOSIGPIPE, &on, sizeof(on));
    
    if(connect(host_socket, (const struct sockaddr *)&addr, addr.sin_len) == -1){
        close(host_socket);
        proxy_log("conect server scoket failed");
        return http_socket_failed;
    }
    
    char *logText = calloc(1,strlen(host_name) + 32);
    sprintf(logText, "Connected host %s : %d",host_name,port);
    proxy_log(logText);
    free(logText);
    
    return host_socket;
}


/**
 根据接收到的http request数据包，解析host的名称和端口号

 @param recv_buf http request data buffer
 @param len 数据长度
 @return server scoket
 */
http_socket connect_server(char *recv_buf, int len)
{
    char str_host[MAX_HOST_NAME] = {0};
    char str_port[8] = {0};	// < 65535
    int port = 80;
    char* sp = (char*)(memchr(recv_buf+8,' ',len-8));
    if (!sp) {return -1;}
    char* pt = (char*)(memchr(recv_buf+8,':',sp-recv_buf-8 ));
    if (pt)
    {
        long l = sp-pt-1;
        if (l >= 8) { return -1; }
        memcpy(str_port,pt+1,l);
        port = atoi(str_port);
        memcpy(str_host,recv_buf+8,pt-recv_buf-8);
    } else {
        memcpy(str_host,recv_buf+8,sp-recv_buf-8);
    }
    
    return connect_host(str_host,port);
}

ssize_t send_data(http_socket socket, const char* buf, int buf_size)
{
    ssize_t position = 0;
    while (position < buf_size)
    {
        ssize_t ret = send(socket, buf+position,buf_size-position,0);
        if (ret > 0) {
            position += ret;
        } else {
            return ret;
        }
    }
    
    return position;
}

int recv_request(http_socket socket, char* buf, int buf_size)
{
    int len = 0;
    char * prf = buf;
    while (len<buf_size)
    {
        ssize_t ret = recv(socket,buf,buf_size-len,0);
        if (ret > 0) {
            len += ret;
        } else {
            return (int)ret;
        }
        // Find End Tag: \r\n\r\n
        if ( len > 4 )
        {
            if ( strstr(prf,"\r\n\r\n") )
            {
                break;
            } else {
                prf = buf + len - 4;
            }
        }
    }
    return len;
}

int pre_response(http_rquest_t* request)
{
    const char response[] = "HTTP/1.1 200 Connection established\r\n"
    "Proxy-agent: ThinCar HTTP Proxy Lite /0.2\r\n\r\n";
    ssize_t ret = send_data(request->client,response,sizeof(response)-1);
    if (ret <= 0){ return 0;}
    return 1;
}


/**
 交换数据，把client里的数据发送给server, 这是一个线程方法

 @param arg http_request_t
 @return NULL
 */
void * do_exchange(void *arg)
{
    http_rquest_t *request = (http_rquest_t *)arg;
    char buf[MAXSIZE];
    
    while(1)
    {
        ssize_t ret = recv(request->client,buf,MAXSIZE,0);
        if (ret <=0 ) {
            printf("client:%d server:%d\r\n",request->client,request->server);
            if(request->semaphore != NULL){
                LASemaphoreSignal(request->semaphore);
            }
            return NULL;
        }
        ret = send_data(request->server,buf,(int)ret);
        if (ret <=0 ) {
            if(request->semaphore != NULL){
                LASemaphoreSignal(request->semaphore);
            }
            return NULL;
        }
    }
    return NULL;
}


/**
 交换client和server里面的数据

 @param arg http_request_t
 @return NULL
 */
void *exchange_data(void *arg)
{
    http_rquest_t *request1 = (http_rquest_t *)arg;
    http_rquest_t *request2 = calloc(1, sizeof(http_rquest_t));
//    {request1->server,request1->client,NULL};
    request2->client = request1->server;
    request2->server = request1->client;
    request2->semaphore = LASemaphoreCreate(0);
    
    //开始交换数据
    LAThreadPoolAddJob(_thpool, (void *)do_exchange, request1);
    LAThreadPoolAddJob(_thpool, (void *)do_exchange, request2);
    
    //等待一分钟如果还没有完成，怎认为是超时。
    LASemaphoreTimedWait(request2->semaphore, 60);
    
    printf("free client:%d server %d\r\n",request2->client, request2->server);
    close(request1->server);
    close(request1->client);
    LASemaphoreDestroy(request2->semaphore);
    free(request2);
    
    return NULL;
}

int send_web_request(http_rquest_t *request, char *send_buf, char *recv_buf, int data_len)
{
    char host_name[MAX_HOST_NAME] = {0};
    int port = DEFAULTPORT;
    
    char * line = strstr(recv_buf,"\r\n");
    
    char * url_begin = strchr(recv_buf,' ');
    if (!url_begin) { return 0; }
    
    char * path_begin = strchr(url_begin+1+HEADLEN,'/');
    if (!path_begin) { return 0; }
    
    
    char * port_begin = (char*)(memchr(url_begin+1+HEADLEN,':',path_begin-url_begin-HEADLEN) );
    char * host_end = path_begin;
    if (port_begin)
    {
        host_end = port_begin;
        char BufPort[64] = {0};
        memcpy(BufPort,port_begin+1,path_begin-port_begin-1);
        port = atoi(BufPort);
    }
    memcpy(host_name,url_begin+1+HEADLEN, host_end-url_begin-1-HEADLEN);
    
    char lineBuf[0x1000] = {0};
    long leng = line-recv_buf;
    if (leng < sizeof(lineBuf) )
    {
        memcpy(lineBuf,recv_buf,leng);
    } else {
        const static int lenc = 50;
        memcpy(lineBuf,recv_buf,lenc);
        strcpy(lineBuf+lenc," ... ");
        memcpy(lineBuf+lenc+5,line-16,16);
    }
    
    request->server = connect_host(host_name, port);
    if(request->server == http_socket_failed) return 0;
    
    memcpy(send_buf,recv_buf, url_begin-recv_buf+1 );
    memcpy(send_buf+(url_begin-recv_buf)+1,path_begin,recv_buf+data_len-path_begin);
    
    char * http_tag = strstr(send_buf+(url_begin-recv_buf)+1," HTTP/1.1\r\n");
    if (http_tag) { http_tag[8] = '0'; }
    
    size_t TotalLen = url_begin+1+data_len-path_begin;
    
    if( send_data(request->server,send_buf, (int)TotalLen) <= 0){ return 0; }
    
    exchange_data( request );
    
    return 1;
}


void *do_proxy_thread(void *arg)
{
    http_rquest_t request = *(http_rquest_t *)arg;
    char recv_buf[MAXSIZE] = {0};
    char send_buf[MAXSIZE] = {0};
    int retval = recv_request(request.client, recv_buf, MAXSIZE);
    if(retval == 0)
    {
        close(request.client);
        close(request.server);
        
        proxy_log("do_proxy_thread():recieve incorrect data");
        
        return NULL;
    }
    
    if ( strncmp("CONNECT ",recv_buf,8) == 0)
    {
        request.server = connect_server(recv_buf, retval);
        if (request.server == http_socket_failed)
        {
            close(request.client);
            close(request.server);
            
            proxy_log("do_proxy_thread():cannot connect server");
            
            return NULL;
        }
        if (pre_response(&request)< 0 ) {
            
            close(request.client);
            close(request.server);
            
            proxy_log("do_proxy_thread():send pre response failed");
            
            return NULL;
        }
        exchange_data(&request);
    }
    else
    {
       if(0 == send_web_request(&request, send_buf, recv_buf, retval))
       {
           close(request.client);
           close(request.server);
           
           proxy_log("do_proxy_thread():send web request failed");
           
           return NULL;
       }
    }
    
    free(arg);
    return NULL;
}

void start(int port)
{
    if(_proxy_socket > 0){
        proxy_log("start():cannot start proxy,because it already running");
        return;
    }
    _thpool = LAThreadPoolCreate(50);
    _proxy_socket = proxy_socket_init(port);
    if(_proxy_socket == http_socket_failed){
        return;
    }
    
    proxy_log("start(): http proxy is running");
    
    http_socket acceptSocket = http_socket_failed;
    while (1) {
        struct sockaddr_in addr;
        socklen_t addrLen = sizeof(addr);
        acceptSocket = accept(_proxy_socket,(struct sockaddr*)&addr, &addrLen);
        if(acceptSocket != http_socket_failed){
            http_rquest_t *paramter = calloc(1, sizeof(http_rquest_t));
            paramter->client = acceptSocket;
            LAThreadPoolAddJob(_thpool, (void *)do_proxy_thread, (void *)paramter);
        }
    }
    return;
}
