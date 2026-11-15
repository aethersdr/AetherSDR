#define _GNU_SOURCE
#include "pd_endpoint.h"
#include <errno.h>
#include <poll.h>
#include <stddef.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

int pd_endpoint_listen(const unsigned char receipt[80],int relay)
{
    struct sockaddr_un address={0};address.sun_family=AF_UNIX;
    const char* prefix=relay?"aetherpd-relay-":"aetherpd-bootstrap-";
    size_t size=strlen(prefix);memcpy(address.sun_path+1,prefix,size);
    const char hex[]="0123456789abcdef";
    for(unsigned i=0;i<32;++i){
        address.sun_path[1+size+2*i]=hex[receipt[16+i]>>4];
        address.sun_path[2+size+2*i]=hex[receipt[16+i]&15];
    }
    int listener=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0);
    if(listener<0){return -1;}
    socklen_t length=(socklen_t)(offsetof(struct sockaddr_un,sun_path)+1+size+64);
    if(bind(listener,(struct sockaddr*)&address,length) || listen(listener,1)){close(listener);return -1;}
    struct pollfd wait={listener,POLLIN,0};int ready;
    do{ready=poll(&wait,1,relay?10000:60000);}while(ready<0 && errno==EINTR);
    int connected=ready==1 && (wait.revents&POLLIN)?accept4(listener,NULL,NULL,SOCK_CLOEXEC):-1;
    close(listener);return connected;
}
