#define _GNU_SOURCE
#include "pd_service.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static const char* test_root;
static int input(const char* name,unsigned char* bytes,size_t* size)
{
    int fd=open(name,O_RDONLY|O_NONBLOCK|O_NOFOLLOW|O_CLOEXEC);if(fd<0){return 0;}*size=0;
    while(*size<PD_SERVICE_OUTPUT_MAX){ssize_t n=read(fd,bytes+*size,PD_SERVICE_OUTPUT_MAX-*size);if(n<0){close(fd);return 0;}if(!n){break;}*size+=(size_t)n;}
    close(fd);return 1;
}
#ifdef PD_SERVICE_TEST
int pd_service_test_root(void){return open(test_root,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);}
int pd_service_test_command(char* const* args,unsigned char* bytes,size_t* size,int timeout)
{
    (void)timeout;char path[4096];snprintf(path,sizeof(path),"%s/commands",test_root);FILE* log=fopen(path,"a");if(!log){return 0;}
    for(unsigned i=0;args[i];++i){fprintf(log,"%s%s",i?" ":"",args[i]);}fputc('\n',log);fclose(log);*size=0;
    if(!strcmp(args[1],"show")){
        snprintf(path,sizeof(path),"%s/%s",test_root,!strcmp(args[3],"aetherpd-start@*.service")?"sessions":args[3]);
        if(!input(path,bytes,size)){return 0;}
        const char marker[]="MainPID=@self";char* found=memmem(bytes,*size,marker,sizeof(marker)-1);
        if(found){
            char value[32];int n=snprintf(value,sizeof(value),"MainPID=%ld",(long)getpid());
            size_t before=(size_t)(found-(char*)bytes),after=*size-before-(sizeof(marker)-1);
            if(n<0 || before+(size_t)n+after>=PD_SERVICE_OUTPUT_MAX){return 0;}
            memmove(found+n,found+sizeof(marker)-1,after);memcpy(found,value,(size_t)n);*size=before+(size_t)n+after;
        }
        return 1;
    }
    snprintf(path,sizeof(path),"%s/command-fail",test_root);return access(path,F_OK)!=0;
}
#endif
int main(int argc,char** argv)
{
    if(argc<3){return 2;}struct pd_service_state state={0};int ok=0;
    if(!strcmp(argv[1],"upgrade-worker-parse") && argc==4){unsigned char bytes[PD_SERVICE_OUTPUT_MAX];size_t size;ok=input(argv[3],bytes,&size) && pd_service_upgrade_worker_parse(atoi(argv[2]),bytes,size,&state);}
    else if(!strcmp(argv[1],"worker-parse") && argc==4){unsigned char bytes[PD_SERVICE_OUTPUT_MAX];size_t size;ok=input(argv[3],bytes,&size) && pd_service_worker_parse(atoi(argv[2]),bytes,size,&state);}
    else if(!strcmp(argv[1],"parse") && argc==4){unsigned char bytes[PD_SERVICE_OUTPUT_MAX];size_t size;ok=input(argv[3],bytes,&size) && pd_service_parse((unsigned)strtoul(argv[2],NULL,10),bytes,size,&state);}
    else if(!strcmp(argv[1],"sessions") && argc==3){
        unsigned char bytes[PD_SERVICE_OUTPUT_MAX];size_t size;struct pd_service_sessions sessions;
        ok=input(argv[2],bytes,&size) && pd_service_sessions_parse(bytes,size,&sessions);
        if(ok){for(unsigned i=0;i<sessions.count;++i){printf("%s %ld %d %d\n",sessions.names[i],(long)sessions.states[i].pid,sessions.states[i].active,sessions.states[i].quiet);}}
    }else if(!strcmp(argv[1],"cgroup") && argc==3){
        unsigned char bytes[PD_SERVICE_OUTPUT_MAX];size_t size;int result=-1;
        if(input(argv[2],bytes,&size)){result=pd_service_owned_cgroup(bytes,size);}ok=result>=0;if(ok){printf("owned %d\n",result);}
    }else{
        test_root=argv[2];
        if((!strcmp(argv[1],"autostart") || !strcmp(argv[1],"upgrade-worker-ready") || !strcmp(argv[1],"upgrade-worker-launch")) && argc==4){
            int root=open(test_root,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
            int units=root>=0?openat(root,"etc/systemd/system",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC):-1;
            ok=units>=0 && (!strcmp(argv[1],"autostart")?pd_service_primary_autostart(units,atoi(argv[3])):
                !strcmp(argv[1],"upgrade-worker-ready")?pd_service_upgrade_worker_ready(units,atoi(argv[3])):
                pd_service_upgrade_worker_launch(units));
            if(units>=0){close(units);}if(root>=0){close(root);}
        }
        else if(!strcmp(argv[1],"files") && argc==3){ok=pd_service_files();}
        else if(!strcmp(argv[1],"upgrade-cleanup-idle") && argc==3){ok=pd_service_upgrade_cleanup_idle();}
        else if(!strcmp(argv[1],"reload") && argc==3){ok=pd_service_reload();}
        else if(!strcmp(argv[1],"read") && argc==4){ok=pd_service_read((unsigned)strtoul(argv[3],NULL,10),&state);}
        else if(!strcmp(argv[1],"stop") && argc==3){ok=pd_service_stop_primary();}
        else if(!strcmp(argv[1],"quiet") && argc==3){ok=pd_service_runtime_quiet();}
        else if(!strcmp(argv[1],"quiesce") && argc==3){ok=pd_service_quiesce();}
    }
    if(ok){printf("ok %ld %d %d\n",(long)state.pid,state.active,state.quiet);}return ok?0:1;
}
