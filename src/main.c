//
//  main.c
//  HTTPProxy
//
//  Created by MacPu on 2016/12/28.
//  Copyright © 2016年 MacPu. All rights reserved.
//

#include <stdio.h>
#include <stdlib.h>
#include "LAHTTPProxy.h"

#define Version "1.0"

int main(int argc, const char * argv[]) {

    printf("****************************************\r\n");
    printf("HTTPProxy (v %s)\r\n", Version);
    printf("star on https://github.com/MacPu/HTTPProxy \r\n");
    printf("****************************************\r\n\r\n");
    
#ifdef XCODE
    start_http_proxy(8080);
#else
    
    if(argc != 2){
        printf("[HTTPProxy] cannot start proxy with invalied arg count\r\n");
        return 0;
    }

    const char *port_str = argv[1];
    int port = atoi(port_str);
    if(port == 0){
        printf("[HTTPProxy] cannot start proxy with invalied port\r\n");
        return 0;
    }
    
    start_http_proxy(port);
#endif
    return 0;
}
