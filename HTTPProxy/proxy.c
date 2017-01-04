//
//  LAHTTPProxy.c
//  HTTPProxy
//
//  Created by MacPu on 2016/12/29.
//  Copyright © 2016年 MacPu. All rights reserved.
//

#include "LAHTTPProxy.h"
#include "LAThreadPool.h"
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

typedef int HTTPProxy_Socket;
typedef int HTTPProxy_Bool;

#define MAX_HOST_NAME 512
#define MAXSIZE 65535
#define DEFAULTPORT 80
#define HEADLEN 7    // http://

#define HTTPProxy_Invlid_Socket -1
#define HTTPProxy_True  1
#define HTTPProxy_False 0
#define HTTPProxy_Down  1

LAThreadPoolRef *thpool;

typedef struct
{
    HTTPProxy_Socket clientSocket;
    HTTPProxy_Socket serverSocket;
    pthread_mutex_t *mutex;
    pthread_cond_t *cond;
    int down;
    
}HTTPProxyRef;

HTTPProxy_Socket _proxySocket;

void HTTPProxyLog(char *logText)
{
#if 1
    printf("[HTTPProxy] %s \r\n",logText);
#endif
}


HTTPProxy_Socket InitSocket(void)
{
    HTTPProxy_Socket fd = socket(AF_INET, SOCK_STREAM, 0);
    if(fd == HTTPProxy_Invlid_Socket){
        HTTPProxyLog("create socket failed");
        return HTTPProxy_Invlid_Socket;
    }
    
    struct sockaddr_in addr;
    bzero((char *)&addr, sizeof(addr));
    addr.sin_len = sizeof(addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(8888);
    addr.sin_addr.s_addr = INADDR_ANY;
    
    socklen_t socklen = sizeof(addr);
    
    int on = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) == -1) {
        close(fd);
        return HTTPProxy_Invlid_Socket;
    }
    
    if (bind(fd, (struct sockaddr*)&addr,socklen) != 0) {
        close(fd);
        return HTTPProxy_Invlid_Socket;
    }
    
    if (listen(fd, 512) != 0) {
        close(fd);
        return HTTPProxy_Invlid_Socket;
    }
    
    HTTPProxyLog("init proxy socket success");
    return fd;
}

HTTPProxy_Socket InitHost(char *hostName, int port)
{
    struct hostent *host = gethostbyname(hostName);
    if(!host){
        return HTTPProxy_Invlid_Socket;
    }
    struct in_addr inad = *((struct in_addr*) *host->h_addr_list);
    struct sockaddr_in addr;
    bzero((char *)&addr, sizeof(addr));
    addr.sin_len = sizeof(addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inad.s_addr;
    
    
    HTTPProxy_Socket serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    if(HTTPProxy_Invlid_Socket == serverSocket){
        close(serverSocket);
        HTTPProxyLog("init server scoket failed");
        return HTTPProxy_Invlid_Socket;
    }
    
    int on = 1;
    setsockopt(serverSocket, IPPROTO_TCP, SO_NOSIGPIPE, &on, sizeof(on));
    
    if(connect(serverSocket, (const struct sockaddr *)&addr, addr.sin_len) == -1){
        close(serverSocket);
        HTTPProxyLog("conect server scoket failed");
        return HTTPProxy_Invlid_Socket;
    }
    
    char *logText = calloc(1,strlen(hostName) + 32);
    sprintf(logText, "Connected %s : %d",hostName,port);
    
    HTTPProxyLog(logText);
    free(logText);
    
    return serverSocket;
}

HTTPProxy_Socket ConnectServer(char* recvBuf,int len)
{
    char strHost[MAX_HOST_NAME] = {0};
    char strPort[8] = {0};	// < 65535
    int port = 80;
    char* sp = (char*)(memchr(recvBuf+8,' ',len-8));
    if (!sp) {return -1;}
    char* pt = (char*)(memchr(recvBuf+8,':',sp-recvBuf-8 ));
    if (pt)
    {
        int l = sp-pt-1;
        if (l >= 8) { return -1; }
        memcpy(strPort,pt+1,l);
        port = atoi(strPort);
        memcpy(strHost,recvBuf+8,pt-recvBuf-8);
    } else {
        memcpy(strHost,recvBuf+8,sp-recvBuf-8);
    }
    
    return InitHost(strHost,port);
}

ssize_t SendData(HTTPProxy_Socket s, const char* buf, int bufSize)
{
    ssize_t position = 0;
    while (position < bufSize)
    {
        ssize_t ret = send(s, buf+position,bufSize-position,0);
        if (ret > 0) {
            position += ret;
        } else {
            return ret;
        }
    }
    
    return position;
}

int RecvRequest(HTTPProxy_Socket s, char* buf, int bufSize)
{
    int len = 0;
    char * prf = buf;
    while (len<bufSize)
    {
        ssize_t ret = recv(s,buf,bufSize-len,0);
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

HTTPProxy_Bool PreResponse(HTTPProxyRef* svc)
{
    const char response[] = "HTTP/1.1 200 Connection established\r\n"
    "Proxy-agent: ThinCar HTTP Proxy Lite /0.2\r\n\r\n";
    ssize_t ret = SendData(svc->clientSocket,response,sizeof(response)-1);
    if (ret <= 0){ return HTTPProxy_False;}
    return HTTPProxy_Down;
}

void *ExcThread(void *arg)
{
    HTTPProxyRef param = *(HTTPProxyRef *)arg;
    HTTPProxy_Socket s1 = param.clientSocket;
    HTTPProxy_Socket s2 = param.serverSocket;
    char buf[MAXSIZE];
    
    while(1)
    {
        ssize_t ret = recv(s1,buf,MAXSIZE,0);
        if (ret <=0 ) {
            if(param.mutex){ pthread_mutex_lock(param.mutex); }
            param.down = 1;
            if(param.cond){
                pthread_cond_signal(param.cond);
            }
            if(param.mutex){ pthread_mutex_unlock(param.mutex); }
            
            return NULL;
        }
        ret = SendData(s2,buf,(int)ret);
        if (ret <=0 ) {
            if(param.mutex){ pthread_mutex_lock(param.mutex); }
            param.down = HTTPProxy_Down;
            if(param.cond){ pthread_cond_signal(param.cond); }
            if(param.mutex){ pthread_mutex_unlock(param.mutex); }
            return NULL;
        }
    }
    return NULL;
}

void *ExchangeData(void *arg)
{
    HTTPProxyRef th1 = *(HTTPProxyRef *)arg;
    HTTPProxyRef th2 = {th1.serverSocket,th1.clientSocket};
    
    //    pthread_t handle1;
    //    pthread_t handle2;
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
    th2.mutex = &mutex;
    th2.cond = &cond;
    th2.down = 0;
    
    //    pthread_create(&handle1, NULL, ExcThread, &th1);
    //    pthread_create(&handle2, NULL, ExcThread, &th2);
    LAThreadPoolAddJob(thpool, (void *)ExcThread, &th1);
    LAThreadPoolAddJob(thpool, (void *)ExcThread, &th2);
    
    pthread_mutex_lock(&mutex);
    if(th2.down != HTTPProxy_Down){
        
        //设置30秒超时
        struct timespec tv;
        clock_gettime(CLOCK_MONOTONIC, &tv);
        tv.tv_sec += 300;
        pthread_cond_wait(&cond, &mutex);
        //        pthread_cond_timedwait(, &tv);
    }
    if(th2.down != HTTPProxy_Down){
        printf("timeout\n");
    }
    else{
        printf("success\n");
    }
    pthread_mutex_unlock(&mutex);
    pthread_cond_destroy(&cond);
    pthread_mutex_destroy(&mutex);
    
    close(th1.serverSocket);
    close(th1.clientSocket);
    
    return NULL;
    
}

HTTPProxy_Bool SendWebRequest(HTTPProxyRef *lpParameter, char *SendBuf, char *RecvBuf, int DataLen)
{
    HTTPProxyLog(RecvBuf);
    
    char HostName[MAX_HOST_NAME] = {0};
    int Port = DEFAULTPORT;
    
    char * line = strstr(RecvBuf,"\r\n");
    
    char * UrlBegin = strchr(RecvBuf,' ');
    if (!UrlBegin) { return HTTPProxy_False; }
    
    char * PathBegin = strchr(UrlBegin+1+HEADLEN,'/');
    if (!PathBegin) { return HTTPProxy_False; }
    
    
    char * PortBegin = (char*)(memchr(UrlBegin+1+HEADLEN,':',PathBegin-UrlBegin-HEADLEN) );
    char * HostEnd = PathBegin;
    if (PortBegin)
    {
        HostEnd = PortBegin;
        char BufPort[64] = {0};
        memcpy(BufPort,PortBegin+1,PathBegin-PortBegin-1);
        Port = atoi(BufPort);
    }
    memcpy(HostName,UrlBegin+1+HEADLEN, HostEnd-UrlBegin-1-HEADLEN);
    
    //Trace
    char lineBuf[0x1000] = {0};
    int leng = line-RecvBuf;
    if (leng < sizeof(lineBuf) )
    {
        memcpy(lineBuf,RecvBuf,leng);
    } else {
        const static int lenc = 50;
        memcpy(lineBuf,RecvBuf,lenc);
        strcpy(lineBuf+lenc," ... ");
        memcpy(lineBuf+lenc+5,line-16,16);
    }
    
    lpParameter->serverSocket = InitHost(HostName, Port);
    if(lpParameter->serverSocket == HTTPProxy_Invlid_Socket) return HTTPProxy_False;
    
    memcpy(SendBuf,RecvBuf, UrlBegin-RecvBuf+1 );
    memcpy(SendBuf+(UrlBegin-RecvBuf)+1,PathBegin,RecvBuf+DataLen-PathBegin);
    
    char * HTTPTag = strstr(SendBuf+(UrlBegin-RecvBuf)+1," HTTP/1.1\r\n");
    if (HTTPTag) { HTTPTag[8] = '0'; }
    
    size_t TotalLen = UrlBegin+1+DataLen-PathBegin;
    
    
    if( SendData(lpParameter->serverSocket,SendBuf, (int)TotalLen) <= 0){ return HTTPProxy_False; }
    
    ExchangeData( lpParameter );
    
    return HTTPProxy_True;
}

void *ProxyThread(void *arg)
{
    HTTPProxyRef param = *(HTTPProxyRef *)arg;
    char recvBuf[MAXSIZE] = {0};
    char sendBuf[MAXSIZE] = {0};
    int retval = RecvRequest(param.clientSocket, recvBuf, MAXSIZE);
    if(retval == 0)
    {
        close(param.clientSocket);
        close(param.serverSocket);
        
        HTTPProxyLog("recieve incorrect data");
        
        return NULL;
    }
    
    if ( strncmp("CONNECT ",recvBuf,8) == 0)
    {
        param.serverSocket = ConnectServer(recvBuf, retval);
        if (param.serverSocket == HTTPProxy_Invlid_Socket)
        {
            
        }
        if (PreResponse(&param)< 0 ) {
            
        }
        ExchangeData(&param);
    }
    else
    {
        SendWebRequest(&param, sendBuf, recvBuf, retval);
    }
    
    
    free(arg);
    return NULL;
}


void start(void)
{
    thpool = LAThreadPoolCreate(50);
    _proxySocket = InitSocket();
    if(_proxySocket == HTTPProxy_Invlid_Socket){
        return;
    }
    
    HTTPProxy_Socket acceptSocket = HTTPProxy_Invlid_Socket;
    HTTPProxyRef *paramter;
    while (1) {
        
        struct sockaddr_in addr;
        socklen_t addrLen = sizeof(addr);
        acceptSocket = accept(_proxySocket,(struct sockaddr*)&addr, &addrLen);
        if(acceptSocket != HTTPProxy_Invlid_Socket){
            paramter = calloc(1, sizeof(HTTPProxyRef));
            paramter->clientSocket = acceptSocket;
            LAThreadPoolAddJob(thpool, (void *)ProxyThread, (void *)paramter);
        }
    }
    return;
}
