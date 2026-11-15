#define _GNU_SOURCE
#include "pd_primary.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static int input(const char* name,unsigned char bytes[65536],size_t* size)
{
    int fd=open(name,O_RDONLY|O_NONBLOCK|O_NOFOLLOW|O_CLOEXEC);if(fd<0){return 0;}*size=0;int ok=1;
    while(*size<65536){ssize_t n=read(fd,bytes+*size,65536-*size);if(n<0){ok=0;break;}if(!n){break;}*size+=(size_t)n;}
    close(fd);return ok;
}
int main(int argc,char** argv)
{
    if(argc<3){return 2;}int ok=0;unsigned char bytes[65536];size_t size=0;uint64_t number=0;
    if(!strcmp(argv[1],"open") || !strcmp(argv[1],"hold") || !strcmp(argv[1],"wait")){
        if(!strcmp(argv[1],"wait")?argc!=6:argc!=4){return 2;}
        int executable=open(argv[3],O_RDONLY|O_NOFOLLOW|O_CLOEXEC);struct pd_primary primary;
        ok=pd_primary_open((pid_t)strtol(argv[2],NULL,10),executable,&primary);if(executable>=0){close(executable);}
        if(ok && !strcmp(argv[1],"hold")){puts("admitted");fflush(stdout);char stop;ok=read(0,&stop,1)==1 && pd_primary_check(&primary);}
        else if(ok && !strcmp(argv[1],"wait")){
            struct pd_readiness expected;ok=input(argv[5],bytes,&size) && pd_readiness_parse(bytes,size,&expected) && pd_primary_wait(&primary,argv[4],&expected);
        }else if(ok){ok=pd_primary_check(&primary);}
        pd_primary_close(&primary);
    }else{
        if(!input(argv[2],bytes,&size)){return 2;}
        if(argc==5 && !strcmp(argv[1],"status")){ok=pd_primary_status(bytes,size,(pid_t)strtol(argv[3],NULL,10),(unsigned)strtoul(argv[4],NULL,10));}
        else if(argc==4 && !strcmp(argv[1],"stat")){ok=pd_primary_stat(bytes,size,(pid_t)strtol(argv[3],NULL,10),&number);}
        else if(argc==3 && !strcmp(argv[1],"cgroup")){ok=pd_primary_cgroup(bytes,size);}
        else if(argc==5 && !strcmp(argv[1],"tcp")){number=strtoull(argv[4],NULL,10);ok=pd_primary_tcp(bytes,size,atoi(argv[3]),&number);}
    }
    if(ok){printf("ok %llu\n",(unsigned long long)number);}return ok?0:1;
}
