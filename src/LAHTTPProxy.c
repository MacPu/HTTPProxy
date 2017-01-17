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

#ifdef __ANDROID__

#include <android/log.h>
#define ANDROID_LOG_TAG "HTTPProxy"
#define ANDROID_LOG_D(...)  __android_log_print(ANDROID_LOG_DEBUG,ANDROID_LOG_TAG,__VA_ARGS__)

#endif

typedef int http_socket;

#define http_socket_failed -1
#define http_request_on 1
#define http_request_off 0

#define MAX_HOST_NAME 512
#define MAXSIZE 65535
#define DEFAULTPORT 80
#define HEADLEN 7    // http://

LAThreadPoolRef *_thpool;
http_socket _proxy_socket;

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
    UNKNOWN,
    HTTP_METHODS_COUNT    // 用来计数method的总数， 并不是真的method
};

enum http_versions_enum {
    HTTP_VERSION_1_0,
    HTTP_VERSION_1_1,
    HTTP_VERSION_INVALID
};

/**
 http request header
 */
typedef struct http_header
{
    enum http_methods_enum method;
    enum http_versions_enum version;
    const char *search_path;
    char *source;                       //http request header source.
    
    TAILQ_HEAD(METADATA_HEAD, http_metadata_item) metadata_head;
} http_header_t;

/**
 http request
 */
typedef struct http_request{
    http_socket client;
    http_socket server;
    http_header_t *header;
    LASemaphoreRef *semaphore;
    int8_t status;
}http_rquest_t;


typedef struct http_metadata_item
{
    const char *key;
    const char *value;
    
    TAILQ_ENTRY(http_metadata_item) entries;
} http_metadata_item_t;


#pragma mark - LOG 

static void proxy_log(enum log_level level,char *log_text)
{
#if DEBUG
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
#ifdef __ANDROID__
        ANDROID_LOG_D("[HTTPProxy] %s \r\n",log_text);
#else
        fprintf(file, "[HTTPProxy] %s \r\n",log_text);
#endif
    }
    else{
#ifdef __ANDROID__
        ANDROID_LOG_D("[HTTPProxy %s] %s \r\n",level_str,log_text);
#else
        fprintf(file, "[HTTPProxy %s] %s \r\n",level_str,log_text);
#endif
    }
#endif
}

#pragma mark - List

static const char *list_get_key(struct METADATA_HEAD *list, const char *key)
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

static void list_add_key(struct METADATA_HEAD *list, const char *key, const char *value)
{
    http_metadata_item_t *item = (http_metadata_item_t*)malloc(sizeof(http_metadata_item_t));
    item->key = key;
    item->value = value;
    
    TAILQ_INSERT_TAIL(list, item, entries);
}

#pragma mark - HTTP Header

static const char *http_methods[] =
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

static void http_header_init(http_header_t **hd)
{
    *hd = (http_header_t*)malloc(sizeof(http_header_t));
    if(hd == NULL) return;
    
    http_header_t *header = *hd;
    header->method = 0;
    header->search_path = NULL;
    header->source = NULL;
    
    TAILQ_INIT(&header->metadata_head);
}

static void http_header_destroy(http_header_t *header)
{
    struct http_metadata_item *item;
    
    TAILQ_FOREACH(item, &header->metadata_head, entries) {
        
        free((char*)item->key);
        free((char*)item->value);
        TAILQ_REMOVE(&header->metadata_head, item, entries);
        free(item);
    }
    free((char*)header->search_path);
    free((char*)header->source);
    free(header);
}

static void http_parse_method(http_header_t* header, const char* line)
{
    enum parser_states {
        METHOD,
        URL,
        VERSION,
        DONE
    };
    
    char* line_copy_first;
    char* line_copy;
    line_copy_first = line_copy = strdup(line);
    char* token = NULL;
    int state = METHOD;
    
    while ((token = strsep(&line_copy, " \r\n")) != NULL) {
        switch (state) {
            case METHOD:
            {
                int found = 0;
                for (int i = 0; i < HTTP_METHODS_COUNT; i++) {
                    if (strcmp(token, http_methods[i]) == 0) {
                        found = 1;
                        header->method = i;
                        break;
                    }
                }
                if (found == 0) {  //如果没有找到，
                    header->method = UNKNOWN;
                    free(line_copy_first);
                    return;
                }
                state++;
                break;
            }
            case URL:
            {
                header->search_path = strdup(token);
                state++;
                break;
            }
            case VERSION:
            {
                if(strcmp(token, "HTTP/1.0") == 0) {
                    header->version = HTTP_VERSION_1_0;
                } else if(strcmp(token, "HTTP/1.1") == 0) {
                    header->version = HTTP_VERSION_1_1;
                } else {
                    header->version = HTTP_VERSION_INVALID;
                }
                state++;
                break;
            }
            case DONE:
                break;
        }
    }
    free(line_copy_first);
    return;
}

static void http_parse_metadata(http_header_t *header, char *line)
{
    if(strlen(line) == 0) return;
    
    char *line_copy = strdup(line);
    char *last;
    char *key = strdup(strtok_r(line_copy, ":",&last));
    if(key == NULL){
        free(line_copy);
        return;
    }
    char *value = strtok_r(NULL, "\r",&last);
    if(value == NULL){
        free(key);
        free(line_copy);
        return;
    }
    
    // 删除空白字符
    char *p = value;
    while(*p == ' ') p++;
    value = strdup(p);
    
    // create the http_metadata_item object and
    // put the data in it
    list_add_key(&header->metadata_head, key, value);
    
    free(line_copy);
    line_copy = NULL;
}

static char *http_build_header(http_header_t *header)
{
    if(header == NULL || header->search_path == NULL) return NULL;
    
    const char *search_path = header->search_path;
    
    // 创建http 请求
    
    const char *method = http_methods[header->method];
    size_t size = strlen(method) + 2;  //因为还有一个空格， 和 \0  etc："GET "
    char *header_buffer = calloc(size, sizeof(char));
    strncat(header_buffer, method, strlen(method));
    strncat(header_buffer, " ", 1);   // 空格
    
    size += strlen(search_path) + 1;
    header_buffer = realloc(header_buffer, size);
    strncat(header_buffer, search_path, strlen(search_path));
    
    // Check the actual HTTP version that is used, and if
    // 1.1 is used we should append:
    // 	Connection: close
    // to the header.
    switch(header->version)
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
    TAILQ_FOREACH(item, &header->metadata_head, entries) {
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
    
    if(header->version == HTTP_VERSION_1_1)
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

#pragma mark - http

static char *read_line(http_socket socket)
{
    int buffer_size = 2;
    char *line = (char*)malloc(sizeof(char)*buffer_size+1);
    char c;
    ssize_t length = 0;
    int counter = 0;
    
    while(1)
    {
        length = recv(socket, &c, 1, 0);
        if(length > 0){
            line[counter++] = c;
        }
        
        //检测到‘\n’ 或者没有说到数据，就证明收完了。
        if(c == '\n' || length <= 0)
        {
            line[counter] = '\0';
//            proxy_log(LOG_TRACE, line);
            return line;
        }
        
        // 重新申请buffer
        if(counter == buffer_size)
        {
            buffer_size *= 2;
            line = (char*)realloc(line, sizeof(char)*buffer_size + 1);
        }
    }
    return NULL;
}

/**
 读取HTTP 请求头

 @param sockfd 需要读取的socket
 @return http header
 */
static http_header_t *http_read_header(int sockfd)
{
//    proxy_log(LOG_TRACE, "Reading header\n");
    http_header_t *header;
    http_header_init(&header);
    if(header == NULL){
        return NULL;
    }
    
    char *line;
    line = read_line(sockfd);
    if(strlen(line)  == 0){
        //如果没有收到数据，证明数据不合法，返回空,之前这里会导致崩溃，所以加一个限制
        http_header_destroy(header);
        return NULL;
    }
    proxy_log(LOG_TRACE, line);
    http_parse_method(header, line);
    header->source = strdup(line);
    free(line);
    
    while(1)
    {
        line = read_line(sockfd);
        header->source = realloc(header->source, (strlen(header->source) + strlen(line) + 1) * sizeof(char));
        strcat(header->source, line);
        if((line[0] == '\r' && line[1] == '\n') || line[0] == '\0')
        {
            // 收到了HTTP header的结束符
            free(line);
            break;
        }
        
        if(strlen(line) != 0){
            http_parse_metadata(header, line);
        }
        
        free(line); 
    }
    
    return header;
}

/**
 根据受到的http_header_t连接服务器，如果找不到相关的消息，可能会返回-1（http_socket_failed）

 @param header 收到的http_header_t
 @return return socket on success，or return http_socket_failed。
 */
static http_socket connect_server(http_header_t *header)
{
    if(header == NULL || header->search_path == NULL) return http_socket_failed;
    char *host_copy = strdup((char*)list_get_key(&header->metadata_head, "Host"));
    if(host_copy == NULL) return http_socket_failed;
    char *host = host_copy;
    
    char *port = strstr(host_copy, ":");
    if(port == NULL){
        // 当没有端口号是就要从searchPath里面分析，
        char *port_str = strstr(header->search_path, host_copy);
        if(port_str != NULL){
            port_str += strlen(host_copy);
            char *port_str_temp = strstr(port_str, ":");
            // 存在":" 并且还是第一个字符，因为 “:8080/dasf”
            if(port_str == port_str_temp){
                port_str_temp ++;
                port = strsep(&port_str_temp, "/\r");
            }
        }
        //如果没有指定就是默认的端口。
        if(port == NULL){
            port = "80";
        }
    }
    else{
        host = strtok(host_copy, ":");  // 去掉端口号
        port++; //
    }
    
    if(host == NULL || port == NULL){
        free(host_copy);
        return http_socket_failed;
    }
    
    //开始根据host 和port 获取地址信息
    struct addrinfo hints, *servinfo = NULL;
    
    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    
    if((getaddrinfo(host, port, &hints, &servinfo) != 0 ) || servinfo == NULL){
        //没有获取到地址信息
        char errlog[1024] = {0};
        sprintf(errlog, "connect_server():Could not connect server with host:%s port:%s",host_copy,port);
        proxy_log(LOG_ERROR, errlog);
        free(host_copy);
        return http_socket_failed;
    }
    free(host_copy);
    
    http_socket server_socket = http_socket_failed;
    for (struct addrinfo *i = servinfo; i != NULL; i = i->ai_next) {
        if ((server_socket = socket(i->ai_family, i->ai_socktype, i->ai_protocol)) == -1) {
            proxy_log(LOG_ERROR, "connect_server(): failed to create server socket");
            continue;
        }
        
        if (connect(server_socket, i->ai_addr, i->ai_addrlen) == -1) {
            close(server_socket);
            proxy_log(LOG_ERROR, "connect_server(): failed to connect server socket");
            continue;
        }
        break;
    }
    freeaddrinfo(servinfo);
    return server_socket;
}

#pragma mark - HTTP Proxy

/**
 初始化一个监听的代理socket

 @param port 监听的端口号
 @return socket
 */
static http_socket proxy_socket_init(int port)
{
    http_socket fd = socket(AF_INET, SOCK_STREAM, 0);
    if(fd == http_socket_failed){
        proxy_log(LOG_ERROR,"proxy_socket_init(): create proxy socket failed");
        return http_socket_failed;
    }
    
    struct sockaddr_in addr;
    bzero((char *)&addr, sizeof(addr));
#ifdef __APPLE__
    addr.sin_len = sizeof(addr);
#endif
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
 发送数据
 
 @param socket 往哪个通道发送
 @param buf 数据缓存
 @param buf_size 大小
 @return 发送的大小
 */
static ssize_t send_data(http_socket socket, const char* buf, size_t buf_size)
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

/**
 交换数据，把client里的数据发送给server, 这是一个线程方法

 @param arg http_request_t
 @return NULL
 */
static void * do_exchange(void *arg)
{
    http_rquest_t *request = (http_rquest_t *)arg;
    while(1)
    {
        char buffer[MAXSIZE] = {0};
        ssize_t ret = recv(request->client,buffer,MAXSIZE,0);
        if (ret <= 0 ) {
            if(request->semaphore != NULL){
                LASemaphoreSignal(request->semaphore);
            }
            return NULL;
        }
        
        //if socket did close, return null
        if(request->status == http_request_off) return NULL;
        
        ret = send_data(request->server,buffer,ret);
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
static void *exchange_data(void *arg)
{
    http_rquest_t *request1 = (http_rquest_t *)arg;
    request1->semaphore = LASemaphoreCreate(0);
    request1->status = http_request_on;
    http_rquest_t *request2 = calloc(1, sizeof(http_rquest_t));
    request2->client = request1->server;
    request2->server = request1->client;
    request2->semaphore = request1->semaphore;
    request2->header = request1->header;
    request2->status = request1->status;
    
    //开始交换数据
    // run two thread to exchagne data from server and client socket
    LAThreadPoolAddJob(_thpool, (void *)do_exchange, request2);
    LAThreadPoolAddJob(_thpool, (void *)do_exchange, request1);
    
    // 先等一个信号，等到一端先关了socket，
    LASemaphoreWait(request2->semaphore);
    
    // 在关闭该程序连接起来的socket。
    shutdown(request1->server, SHUT_RDWR);
    shutdown(request1->client, SHUT_RDWR);
    close(request1->server);
    close(request1->client);
    request1->status = http_request_off;
    request2->status = request1->status;
    
    //再等待另外一端关闭socket。
    LASemaphoreWait(request1->semaphore);
    
    //释放资源
    LASemaphoreDestroy(request1->semaphore);
    request1->semaphore = NULL;
    request2->semaphore = NULL;
    free(request2);
    request2 = NULL;
    
    return NULL;
}

/**
 用于CONNECT 请求的response 数据

 @param request request
 @return 发送数据的大小
 */
static ssize_t pre_response(http_rquest_t* request)
{
    const char response[] = "HTTP/1.1 200 Connection established\r\n"
    "Proxy-agent: ThinCar HTTP Proxy V1.0. By MacPu.\r\n\r\n";
    ssize_t ret = send_data(request->client,response,sizeof(response)-1);
    if (ret <= 0){ return 0;}
    return ret;
}

/**
 每收到一个socket请求之后就会到这个方法里面来。这个是网络数据交换的核心代码

 @param req http request
 */
static void *do_proxy_thread(void *req)
{
    http_rquest_t *request = (http_rquest_t *)req;
    
    //先获取到header.
    //recieve header
    request->header = http_read_header(request->client);
    if(request->header == NULL){  //获取失败
        proxy_log(LOG_ERROR, "do_proxy_thread():Failed to parse header");
        free(request);
        close(request->client);
        return NULL;
    }
    
    //连接服务器
    // connect to server
    request->server = connect_server(request->header);
    if(request->server == http_socket_failed){
        free(request);
        close(request->client);
        http_header_destroy(request->header);
        return NULL;
        
    }
    
    if(request->header->method == CONNECT){
        // 如果是CONNECT请求的话，先发送response数据，创建连接。
        // if request is a CONNECT request, so send pre response to connect the link
        pre_response(request);
    }
    else{
        //发送http request
        // send http request.
        send_data(request->server, request->header->source, strlen(request->header->source));
    }
    
    // 交换数据
    // start exchange data
    exchange_data(request);
    
    // 释放资源
    // free memory
    http_header_destroy(request->header);
    free(request);
    return NULL;
}

#pragma mark - start_http_proxy

void start_http_proxy(int port)
{
    if(_proxy_socket > 0){
        proxy_log(LOG_ERROR,"start():cannot start proxy,because it already running");
        return;
    }
    _thpool = LAThreadPoolCreate(60);
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
            LAThreadPoolAddJob(_thpool, (void *)do_proxy_thread, (void *)request);
        }
    }
    return;
}
