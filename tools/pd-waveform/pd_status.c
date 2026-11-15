#define _GNU_SOURCE
#include "pd_status.h"
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>

int pd_status_open(const char* text)
{
    if (!text || !*text) { return -1; }
    unsigned value=0;
    for (const char* p=text;*p;++p) {
        if (*p<'0' || *p>'9' || value>102) { return -2; }
        value=value*10+(unsigned)(*p-'0');
    }
    if (value<3 || value>1023) { return -2; }
    int fd=(int)value,type=0;socklen_t size=sizeof(type);
    struct sockaddr_un address={0};socklen_t length=sizeof(address);
    if (getsockopt(fd,SOL_SOCKET,SO_TYPE,&type,&size) || type!=SOCK_DGRAM ||
        getsockname(fd,(struct sockaddr*)&address,&length) || address.sun_family!=AF_UNIX ||
        length!=offsetof(struct sockaddr_un,sun_path)) { return -2; }
    length=sizeof(address);
    if (getpeername(fd,(struct sockaddr*)&address,&length) || address.sun_family!=AF_UNIX ||
        length!=offsetof(struct sockaddr_un,sun_path)) { return -2; }
    int flags=fcntl(fd,F_GETFL),descriptor=fcntl(fd,F_GETFD);
    if (flags<0 || descriptor<0 || fcntl(fd,F_SETFL,flags|O_NONBLOCK) ||
        fcntl(fd,F_SETFD,descriptor|FD_CLOEXEC)) { return -2; }
    return fd;
}

int pd_status_request(int fd,unsigned char nonce[8])
{
    struct pd_status_query query;
    int result=pd_status_query(fd,&query);
    if (result<=0) { return result; }
    if (query.operation!='?') { return -1; }
    memcpy(nonce,query.nonce,8);
    return 1;
}

int pd_status_query(int fd,struct pd_status_query* query)
{
    unsigned char request[22];
    ssize_t size=recv(fd,request,sizeof(request),MSG_DONTWAIT);
    if (size<0) { return 0; }
    if (!((size==9 && request[0]=='?') || (size==17 && request[0]=='B') ||
          (size==21 && request[0]=='R'))) { return -1; }
    memset(query,0,sizeof(*query));
    query->operation=request[0];memcpy(query->nonce,request+1,8);
    if (size>=17) {
        for (unsigned i=9;i<17;++i) { query->capture_id=(query->capture_id<<8)|request[i]; }
        if (!query->capture_id) { return -1; }
    }
    if (size==21) {
        for (unsigned i=17;i<21;++i) { query->capture_offset=(query->capture_offset<<8)|request[i]; }
    }
    return 1;
}

int pd_status_reply(int fd,const unsigned char nonce[8],const char* json,size_t size)
{
    if (!json || !size || size>16384) { return 0; }
    struct iovec vectors[2]={{(void*)nonce,8},{(void*)json,size}};
    struct msghdr message={.msg_iov=vectors,.msg_iovlen=2};
    return sendmsg(fd,&message,MSG_DONTWAIT|MSG_NOSIGNAL)==(ssize_t)(size+8);
}
