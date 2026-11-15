/* Inert native fixture for permanent management entry tests; no radio API. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <unistd.h>
extern char** environ;
static int peer(const char* mode)
{
    int fd=socket(AF_UNIX,SOCK_STREAM,0);struct sockaddr_un address={.sun_family=AF_UNIX};
    strcpy(address.sun_path,"/run/aetherpd/peer.sock");
    if(fd<0 || connect(fd,(struct sockaddr*)&address,sizeof(address))){return 10;}
    unsigned char frame[48]="PDSTART1";
    memcpy(frame+8,"12345678-1234-1234-1234-123456789abc",36);frame[47]=42;
    if(!strcmp(mode,"bad-frame")){frame[0]='X';}
    if(!strcmp(mode,"zero-handle")){frame[47]=0;}
    if(send(fd,frame,sizeof(frame),MSG_NOSIGNAL)!=(ssize_t)sizeof(frame)){close(fd);return 11;}
    struct pollfd p={fd,POLLIN,0};char reply[2];
    if(poll(&p,1,5000)<=0){close(fd);return 12;}
    ssize_t n=recv(fd,reply,sizeof(reply),0);
    if(n==2 && !memcmp(reply,"OK",2)){
        usleep(800000);
        if(send(fd,"X",1,MSG_NOSIGNAL)!=1){close(fd);return 13;}
        close(fd);return 0;
    }
    close(fd);return n<=0?0:14;
}
static int status(void)
{
    FILE* file=fopen("/proc/self/status","r");if(!file){return 0;}
    char line[256];
    while(fgets(line,sizeof(line),file)){
        if(!strncmp(line,"Uid:",4) || !strncmp(line,"Gid:",4) || !strncmp(line,"Groups:",7) ||
           !strncmp(line,"Cap",3) || !strncmp(line,"NoNewPrivs:",11)){fputs(line,stdout);}
    }
    fclose(file);return 1;
}
int main(int argc,char** argv)
{
    if(argc==3 && !strcmp(argv[1],"peer")){return peer(argv[2]);}
    int constructor=!strcmp(argv[0],"pd-radio-launch");
    if(!status()){return 20;}
    printf("Role: %s\nNice: %d\nPolicy: %d\nSentinelClosed: %d\n",
           constructor?"constructor":"primary",getpriority(PRIO_PROCESS,0),(int)syscall(SYS_sched_getscheduler,0),
           fcntl(123,F_GETFD)<0 && errno==EBADF);
    for(int i=0;i<argc;++i){printf("Arg: %s\n",argv[i]);}
    for(char** p=environ;*p;++p){printf("Env: %s\n",*p);}
    fflush(stdout);
    if(constructor){
        int type=0;socklen_t size=sizeof(type);
        if(argc!=1 || getsockopt(3,SOL_SOCKET,SO_TYPE,&type,&size) || type!=SOCK_STREAM ||
           send(3,"OK",2,MSG_NOSIGNAL)!=2){return 21;}
        struct pollfd p={3,POLLIN,0};char byte=0;
        if(poll(&p,1,3000)<=0 || recv(3,&byte,1,0)!=1 || byte!='X'){return 22;}
    }
    return 0;
}
