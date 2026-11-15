#define _GNU_SOURCE
#include "pd_identity.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stddef.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int identity_value(const char* begin,size_t size,char* output,size_t capacity)
{
    if(size>=2 && begin[0]=='"' && begin[size-1]=='"'){++begin;size-=2;}
    if(!size || size>=capacity){return 0;}
    for(size_t i=0;i<size;++i){
        unsigned char c=(unsigned char)begin[i];
        if(!((c>='0' && c<='9') || (c>='A' && c<='Z') || (c>='a' && c<='z') || c=='-' || c=='.')){return 0;}
    }
    memcpy(output,begin,size);output[size]=0;return 1;
}
int pd_identity_parse(const char* reply,struct pd_identity* identity)
{
    memset(identity,0,sizeof(*identity));
    if(strncmp(reply,"R970201|0|",10)){return 0;}const char* p=reply+10;unsigned fields=0;
    while(*p){
        const char* start=p;int quoted=0;
        while(*p && (*p!=',' || quoted)){if(*p=='"'){quoted=!quoted;}++p;}
        if(quoted){return 0;}size_t size=(size_t)(p-start);
        const char* equal=memchr(start,'=',size);if(!equal || equal==start){return 0;}
        size_t key=(size_t)(equal-start),value=size-key-1;unsigned bit=0;char* target=NULL;size_t capacity=0;
        if(key==5 && !memcmp(start,"model",5)){bit=1;target=identity->model;capacity=sizeof(identity->model);}
        else if(key==12 && !memcmp(start,"software_ver",12)){bit=2;target=identity->firmware;capacity=sizeof(identity->firmware);}
        else if(key==14 && !memcmp(start,"chassis_serial",14)){bit=4;target=identity->serial;capacity=sizeof(identity->serial);}
        else if(key==2 && !memcmp(start,"ip",2)){bit=8;target=identity->address;capacity=sizeof(identity->address);}
        if(bit){if(fields&bit || !identity_value(equal+1,value,target,capacity)){return 0;}fields|=bit;}
        if(*p){++p;if(!*p){return 0;}}
    }
    if(fields&8){
        struct in_addr address;char canonical[16];
        if(inet_pton(AF_INET,identity->address,&address)!=1 || !inet_ntop(AF_INET,&address,canonical,sizeof(canonical)) ||
            strcmp(identity->address,canonical)){return 0;}
    }
    return (fields&7)==7;
}
int pd_identity_matches(const struct pd_identity* identity,uint32_t profile,const char* firmware,const char* serial)
{
    const char* model=profile==1?"FLEX-8600":profile==2?"FLEX-6300":"";
    return *model && !strcmp(identity->model,model) && !strcmp(identity->firmware,firmware) &&
        *serial && !strcmp(identity->serial,serial);
}
static int64_t identity_ms(void)
{
    struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t)){return -1;}
    return (int64_t)t.tv_sec*1000+t.tv_nsec/1000000;
}
int pd_identity_probe(struct pd_identity* identity)
{
    int fd=socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0);if(fd<0){return 0;}
    struct sockaddr_in address={.sin_family=AF_INET,.sin_port=htons(4992),.sin_addr={.s_addr=htonl(INADDR_LOOPBACK)}};
    int64_t start=identity_ms();int ok=start>=0;
    if(ok && connect(fd,(struct sockaddr*)&address,sizeof(address))){ok=errno==EINPROGRESS;}
    struct pollfd event={fd,POLLOUT,0};int error=0;socklen_t size=sizeof(error);
    ok=ok && poll(&event,1,2000)>0 && !getsockopt(fd,SOL_SOCKET,SO_ERROR,&error,&size) && !error;
    static const char command[]="C970201|info\n";
    if(ok){ok=send(fd,command,sizeof(command)-1,MSG_NOSIGNAL)==(ssize_t)sizeof(command)-1;}
    char line[4097];size_t used=0,total=0;int found=0;
    while(ok && !found){
        int64_t now=identity_ms();if(now<start || now-start>=5000){break;}
        event.events=POLLIN;event.revents=0;int ready=poll(&event,1,100);
        if(ready<0 && errno==EINTR){continue;}if(ready<0){break;}if(!ready){continue;}
        unsigned char bytes[4096];ssize_t n=recv(fd,bytes,sizeof(bytes),0);
        if(n<0 && (errno==EINTR || errno==EAGAIN)){continue;}if(n<=0){break;}
        total+=(size_t)n;if(total>65536){break;}
        for(ssize_t i=0;ok && !found && i<n;++i){
            unsigned char c=bytes[i];
            if(c=='\n'){
                if(used && line[used-1]=='\r'){--used;}
                line[used]=0;
                if(!strncmp(line,"R970201|",8)){found=pd_identity_parse(line,identity);ok=found;}
                used=0;
            }else if((c<32 && c!='\r') || c>126 || used>=sizeof(line)-1){ok=0;}
            else{line[used++]=(char)c;}
        }
    }
    close(fd);return ok && found;
}
