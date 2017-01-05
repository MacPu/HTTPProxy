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
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <string.h>
#include <math.h>
#include <sys/socket.h>
#include <sys/queue.h>
#include <time.h>
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

enum log_level{
    LOG_ERROR,
    LOG_WARNING,
    LOG_NOTICE,
    LOG_TRACE
};


enum http_methods_enum {
    OPTIONS,
    GET,
    HEAD,
    POST,
    PUT,
    DELETE,
    TRACE,
    CONNECT,
    UNKNOWN
};

enum http_versions_enum {
    HTTP_VERSION_1_0,
    HTTP_VERSION_1_1,
    HTTP_VERSION_INVALID
};

typedef struct http_header
{
    enum http_methods_enum method;
    enum http_versions_enum version;
    const char *search_path;
    
    TAILQ_HEAD(METADATA_HEAD, http_metadata_item) metadata_head;
} http_header_t;

typedef struct http_metadata_item
{
    const char *key;
    const char *value;
    
    TAILQ_ENTRY(http_metadata_item) entries;
} http_metadata_item_t;


#pragma mark - LOG 

void proxy_log(enum log_level level,char *log_text)
{
#if 1
    FILE *file = stdout;
    if (level == LOG_ERROR) {
        file = stderr;
    }
    
    char *level_str = "";
    switch (level) {
        case LOG_ERROR:
            level_str = "error";
            break;
        case LOG_NOTICE:
            level_str = "notice";
            break;
        case LOG_WARNING:
            level_str = "warning";
            break;
        default:
            break;
    }
    if(level == LOG_TRACE){
        fprintf(file, "[HTTPProxy] %s \r\n",log_text);
    }
    else{
        fprintf(file, "[HTTPProxy %s] %s \r\n",level_str,log_text);
    }
#endif
}

#pragma mark - HTTP Header

int http_methods_len = 9;

const char *http_methods[] =
{
    "OPTIONS",
    "GET",
    "HEAD",
    "POST",
    "PUT",
    "DELETE",
    "TRACE",
    "CONNECT",
    "INVALID"
};

void http_header_init(http_header_t **hd)
{
    *hd = (http_header_t*)malloc(sizeof(http_header_t));
    
    http_header_t *header = *hd;
    header->method = 0;
    header->search_path = NULL;
    
    TAILQ_INIT(&header->metadata_head);
}

void http_header_destroy(http_header_t *header)
{
    struct http_metadata_item *item;
    
    TAILQ_FOREACH(item, &header->metadata_head, entries) {
        
        printf("---- key:%s->%p value:%s->%p  --- \r\n",item->key,item->key,item->value,item->value);
        TAILQ_REMOVE(&header->metadata_head, item, entries);
        free((char*)item->key);
        free((char*)item->value);
        item->key = NULL;
        item->value = NULL;
        free(item);
    }
    free((char*)header->search_path);
    free(header);
}

void http_parse_method(http_header_t* header, const char* line)
{
    enum parser_states {
        METHOD,
        URL,
        VERSION,
        DONE
    };
    
    char* copy;
    char* p;
    copy = p = strdup(line);
    char* token = NULL;
    int s = METHOD;
    
    while ((token = strsep(&p, " \r\n")) != NULL) {
        switch (s) {
            case METHOD: {
                int found = 0;
                for (int i = 0; i < http_methods_len; i++) {
                    if (strcmp(token, http_methods[i]) == 0) {
                        found = 1;
                        header->method = i;
                        break;
                    }
                }
                if (found == 0) {
                    header->method = http_methods_len - 1;
                    free(copy);
                    return;
                }
                s++;
                break;
            }
            case URL:
                header->search_path = strdup(token);
                s++;
                break;
            case VERSION:
            {
                if(strcmp(token, "HTTP/1.0") == 0) {
                    header->version = HTTP_VERSION_1_0;
                } else if(strcmp(token, "HTTP/1.1") == 0) {
                    header->version = HTTP_VERSION_1_1;
                } else {
                    header->version = HTTP_VERSION_INVALID;
                }
                s++;
                break;
            }
            case DONE:
                break;
        }
    }
    free(copy);
    return;
}

void http_parse_metadata(http_header_t *header, char *line)
{
    if(strlen(line) == 0) return;
    
    char *line_copy = strdup(line);
    char *key = strdup(strtok(line_copy, ":"));
    
    char *value = strtok(NULL, "\r");
    
    // remove whitespaces :)
    char *p = value;
    while(*p == ' ') p++;
    value = strdup(p);
    
    free(line_copy);
    line_copy = NULL;
    // create the http_metadata_item object and
    // put the data in it
    http_metadata_item_t *item = malloc(sizeof(http_metadata_item_t));
    bzero(item, sizeof(http_metadata_item_t));
    item->key = key;
    item->value = value;
    
    printf("** key:%s->%p value:%s->%p  **\r\n",key,key,value,value);
    // add the new item to the list of metadatas
    TAILQ_INSERT_TAIL(&header->metadata_head, item, entries);
}

char *http_build_header(http_header_t *req)
{
    const char *search_path = req->search_path;
    
    // construct the http request
    int size = strlen("GET ") + 1;
    //char *request_buffer = calloc(sizeof(char)*size);
    char *header_buffer = calloc(size, sizeof(char));
    strncat(header_buffer, "GET ", 4);
    
    size += strlen(search_path) + 1;
    header_buffer = realloc(header_buffer, size);
    strncat(header_buffer, search_path, strlen(search_path));
    
    // Check the actual HTTP version that is used, and if
    // 1.1 is used we should append:
    // 	Connection: close
    // to the header.
    switch(req->version)
    {
        case HTTP_VERSION_1_0:
            size += strlen(" HTTP/1.0\r\n\r\n");
            header_buffer = realloc(header_buffer, size);
            strncat(header_buffer, " HTTP/1.0\r\n", strlen(" HTTP/1.0\r\n"));
            break;
        case HTTP_VERSION_1_1:
            size += strlen(" HTTP/1.1\r\n\r\n");
            header_buffer = realloc(header_buffer, size);
            strncat(header_buffer, " HTTP/1.1\r\n", strlen(" HTTP/1.1\r\n"));
            break;
        default:
            //            LOG(LOG_ERROR, "Failed to retrieve the http version\n");
            return NULL;
    }
    
    http_metadata_item_t *item;
    TAILQ_FOREACH(item, &req->metadata_head, entries) {
        // Remove Connection properties in header in case
        // there are any
        if(strcmp(item->key, "Connection") == 0 ||
           strcmp(item->key, "Proxy-Connection") == 0)
        {
            continue;
        }
        
        size += strlen(item->key) + strlen(": ") + strlen(item->value) + strlen("\r\n");
        header_buffer = realloc(header_buffer, size);
        strncat(header_buffer, item->key, strlen(item->key));
        strncat(header_buffer, ": ", 2);
        strncat(header_buffer, item->value, strlen(item->value));
        strncat(header_buffer, "\r\n", 2);
    }
    
    if(req->version == HTTP_VERSION_1_1)
    {
        size += strlen("Connection: close\r\n");
        header_buffer = realloc(header_buffer, size);
        strncat(header_buffer, "Connection: close\r\n", strlen("Connection: close\r\n"));
    }
    
    
    size += strlen("\r\n");
    header_buffer = realloc(header_buffer, size);
    strncat(header_buffer, "\r\n", 2);
    
    return header_buffer;
}

#pragma mark - List

const char *list_get_key(struct METADATA_HEAD *list, const char *key)
{
    http_metadata_item_t *item;
    TAILQ_FOREACH(item, list, entries){
        if(strcmp(item->key, key) == 0)
        {
            return item->value;
        }
    }
    return NULL;
}

void list_add_key(struct METADATA_HEAD *list, const char *key, const char *value)
{
    http_metadata_item_t *item = (http_metadata_item_t*)malloc(sizeof(http_metadata_item_t));
    item->key = key;
    item->value = value;
    
    TAILQ_INSERT_TAIL(list, item, entries); 
}

#pragma mark - http

char *read_line(http_socket socket)
{
    int buffer_size = 2;
    char *line = (char*)malloc(sizeof(char)*buffer_size+1);
    char c;
    ssize_t length = 0;
    int counter = 0;
    
    while(1)
    {
        length = recv(socket, &c, 1, 0);
        line[counter++] = c;
        
        //检测到‘\n’ 或者没有说到数据，就证明收完了。
        if(c == '\n' || length == 0)
        {
            line[counter] = '\0';
            if(length == 0){
                printf("scoket:%d",socket);
            }
            else{
                proxy_log(LOG_TRACE, line);
            }
            return line;
        }
        
        // 重新申请buffer
        if(counter == buffer_size)
        {
            buffer_size *= 2;
            
            // should probably allocate +1 for the null terminator,
            // but not sure.
            line = (char*)realloc(line, sizeof(char)*buffer_size);
        }
    }
    return NULL;
}

/**
 读取HTTP 请求头

 @param sockfd 需要读取的socket
 @return http header
 */
http_header_t *http_read_header(int sockfd)
{
//    proxy_log(LOG_TRACE, "Reading header\n");
    http_header_t *req;
    http_header_init(&req);
    
    char *line;
    line = read_line(sockfd);
    http_parse_method(req, line);
    
    while(1)
    {
        line = read_line(sockfd);
        if((line[0] == '\r' && line[1] == '\n') || line[0] == '\0')
        {
            // 收到了HTTP header的结束符
//            proxy_log(LOG_TRACE, "Received header\n");
            
            break;
        }
        
        if(strlen(line) != 0){
            http_parse_metadata(req, line);
        }
        
        free(line); 
    }
    
    return req;
}

#pragma mark - HTTP Proxy

/**
 初始化一个监听的代理socket

 @param port 监听的端口号
 @return socket
 */
http_socket proxy_socket_init(int port)
{
    http_socket fd = socket(AF_INET, SOCK_STREAM, 0);
    if(fd == http_socket_failed){
        proxy_log(LOG_ERROR,"proxy_socket_init(): create proxy socket failed");
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
        proxy_log(LOG_ERROR,"proxy_socket_init(): bind proxy socket to addr failed.");
        return http_socket_failed;
    }
    
    if (listen(fd, 512) != 0) {
        close(fd);
        proxy_log(LOG_ERROR,"proxy_socket_init(): listen proxy socket failed.");
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
        proxy_log(LOG_ERROR,"init server scoket failed");
        return http_socket_failed;
    }
    
    int on = 1;
    setsockopt(host_socket, IPPROTO_TCP, SO_NOSIGPIPE, &on, sizeof(on));
    
    struct linger m_sLinger;
    m_sLinger.l_onoff = 1;  // (在closesocket()调用,但是还有数据没发送完毕的时候容许逗留)
    m_sLinger.l_linger = 0; // (容许逗留的时间为0秒)
    setsockopt(host_socket,SOL_SOCKET,SO_LINGER,(const char*)&m_sLinger, sizeof(struct linger));
    
    if(connect(host_socket, (const struct sockaddr *)&addr, addr.sin_len) == -1){
        close(host_socket);
        proxy_log(LOG_ERROR,"conect server scoket failed");
        return http_socket_failed;
    }
    
    char *logText = calloc(1,strlen(host_name) + 32);
    sprintf(logText, "Connected host %s : %d",host_name,port);
    proxy_log(LOG_TRACE,logText);
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
//            send_data(request->server,buf,(int)ret);
            if(ret == 0 && request->semaphore != NULL){
                printf("----finish   recv  %d_%d\r\n",request->client,request->server);
                LASemaphoreSignal(request->semaphore);
            }
            else if(ret < 0){
                printf("disconnect  recv  %d_%d\r\n",request->client,request->server);
                close(request->client);
//                LASemaphoreSignal(request->semaphore);
            }
            return NULL;
        }
        else{
            printf("success ret:%ld recv  %d_%d\r\n",ret,request->client,request->server);
        }
        ret = send_data(request->server,buf,(int)ret);
        if (ret <=0 ) {
            if(ret == 0 && request->semaphore != NULL){
                printf("---- finish   send   %d_%d\r\n",request->client,request->server);
                LASemaphoreSignal(request->semaphore);
            }
            else if(ret < 0){
                printf("disconnect    send  %d_%d\r\n",request->client,request->server);
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
    if(ETIMEDOUT ==  LASemaphoreTimedWait(request2->semaphore, 6000)){
        fprintf(stderr, " ---------- timeout -----------\r\n");
    }
   
    
    printf("free client:%d server %d\r\n",request2->client, request2->server);
    shutdown(request1->server, SHUT_RDWR);
    shutdown(request1->client, SHUT_RDWR);
    close(request1->server);
    close(request1->client);
//    close(request1->client);
    LASemaphoreDestroy(request2->semaphore);
    free(request2);
    request2 = NULL;
    
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
    printf("web %d-%d \r\n",request->client,request->server);
    
    memcpy(send_buf,recv_buf, url_begin-recv_buf+1 );
    memcpy(send_buf+(url_begin-recv_buf)+1,path_begin,recv_buf+data_len-path_begin);
    
    char * http_tag = strstr(send_buf+(url_begin-recv_buf)+1," HTTP/1.1\r\n");
    if (http_tag) { http_tag[8] = '0'; }
    
    size_t TotalLen = url_begin+1+data_len-path_begin;
    
    if( send_data(request->server,send_buf, (int)TotalLen) <= 0){ return 0; }
    
    exchange_data( request );
    
    return 1;
}

void *do_proxy_thread1(void *arg)
{
    http_rquest_t *request = (http_rquest_t *)arg;
    http_header_t *header = http_read_header(request->client);
    if(header == NULL){
        proxy_log(LOG_ERROR, "do_proxy_thread():Failed to parse header");
    }
    
    http_header_destroy(header);
    close(request->client);
    return NULL;
}

void *do_proxy_thread(void *arg)
{
    http_rquest_t *request = (http_rquest_t *)arg;
//    struct linger m_sLinger;
//    m_sLinger.l_onoff = 1;  // (在closesocket()调用,但是还有数据没发送完毕的时候容许逗留)
//    m_sLinger.l_linger = 0; // (容许逗留的时间为0秒)
//    setsockopt(request->client,SOL_SOCKET,SO_LINGER,(const char*)&m_sLinger, sizeof(struct linger));
//    
    
    char recv_buf[MAXSIZE] = {0};
    char send_buf[MAXSIZE] = {0};
    int retval = recv_request(request->client, recv_buf, MAXSIZE);
    if(retval == 0)
    {
        close(request->client);
        close(request->server);
        
        proxy_log(LOG_ERROR,"do_proxy_thread():recieve incorrect data");
        
        return NULL;
    }
    
    if ( strncmp("CONNECT ",recv_buf,8) == 0)
    {
        request->server = connect_server(recv_buf, retval);
        printf("connect %d-%d \r\n",request->client,request->server);
        if (request->server == http_socket_failed)
        {
            close(request->client);
            close(request->server);
            
            proxy_log(LOG_ERROR,"do_proxy_thread():cannot connect server");
            
            return NULL;
        }
        if (pre_response(request)< 0 ) {
            
            close(request->client);
            close(request->server);
            
            proxy_log(LOG_ERROR,"do_proxy_thread():send pre response failed");
            
            return NULL;
        }
        exchange_data(&request);
    }
    else
    {
       if(0 == send_web_request(request, send_buf, recv_buf, retval))
       {
           close(request->client);
           close(request->server);
           
           proxy_log(LOG_ERROR,"do_proxy_thread():send web request failed");
           
           return NULL;
       }
    }
    
    free(arg);
    return NULL;
}

void start(int port)
{
    if(_proxy_socket > 0){
        proxy_log(LOG_ERROR,"start():cannot start proxy,because it already running");
        return;
    }
    _thpool = LAThreadPoolCreate(10);
    _proxy_socket = proxy_socket_init(port);
    if(_proxy_socket == http_socket_failed){
        return;
    }
    
    proxy_log(LOG_TRACE,"start(): http proxy is running");
    
    http_socket acceptSocket = http_socket_failed;
    while (1) {
        struct sockaddr_in addr;
        socklen_t addrLen = sizeof(addr);
        acceptSocket = accept(_proxy_socket,(struct sockaddr*)&addr, &addrLen);
        if(acceptSocket != http_socket_failed){
            http_rquest_t *request = calloc(1, sizeof(http_rquest_t));
            request->client = acceptSocket;
            LAThreadPoolAddJob(_thpool, (void *)do_proxy_thread1, (void *)request);
//            sleep(2);
        }
    }
    return;
}
