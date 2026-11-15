#define _GNU_SOURCE
#include "pd_readiness.h"
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
static const char* fault="";
int pd_readiness_checkpoint(const char* point)
{
    if(!strncmp(fault,"kill:",5) && !strcmp(fault+5,point)){raise(SIGKILL);_exit(99);}return 1;
}
int main(int argc,char** argv)
{
    if(argc!=3 && argc!=6){return 2;}struct pd_readiness value={0};int ok=0;
    if(argc==3 && !strcmp(argv[1],"probe")){ok=pd_readiness_probe(argv[2],&value);}
    else{
        int input=open(argv[2],O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
        unsigned char bytes[PD_READINESS_HTTP_MAX+1];size_t size=0;
        if(input<0){return 2;}
        while(size<sizeof(bytes)){ssize_t n=read(input,bytes+size,sizeof(bytes)-size);if(n<0){close(input);return 2;}if(!n){break;}size+=(size_t)n;}
        close(input);
        if(!strcmp(argv[1],"json")){ok=pd_readiness_parse(bytes,size,&value);}
        else if(!strcmp(argv[1],"http")){ok=pd_readiness_http(bytes,size,&value);}
        else if(argc==6){
            fault=argv[5];unsigned char plan[PD_MIGRATION_SIZE];struct pd_migration migration;
            input=open(argv[3],O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
            int store=open(argv[4],O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
            ok=pd_migration_read(input,plan,&migration);if(input>=0){close(input);}
            if(ok && !strcmp(argv[1],"prepare")){ok=pd_readiness_parse(bytes,size,&value) && pd_readiness_prepare(store,plan,&value);}
            else if(ok && !strcmp(argv[1],"read")){ok=pd_readiness_read(store,plan,&value);}
            else{ok=0;}
            if(store>=0){close(store);}
        }
    }
    if(ok){printf("%s\n%s\n%s\n",value.hostname,value.address,value.dns);}return ok?0:1;
}
