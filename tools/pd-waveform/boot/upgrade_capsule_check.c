#define _GNU_SOURCE
#include "pd_upgrade_capsule.h"
#include "pd_upgrade_units.h"
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static const char* fault;
int pd_transaction_checkpoint(const char* point,unsigned phase){(void)point;(void)phase;return 1;}
int pd_capsule_checkpoint(const char* point)
{
    if(!strcmp(fault,point)){_exit(99);}
    if(!strncmp(fault,"hold:",5) && !strcmp(fault+5,point)){
        puts("ready");fflush(stdout);char byte;ssize_t count=read(0,&byte,1);(void)count;
    }
    return 1;
}
int pd_worker_units_checkpoint(const char* point){return pd_capsule_checkpoint(point);}
int __wrap_pd_service_upgrade_cleanup_idle(void){return strcmp(fault,"cleanup-busy")!=0;}
uint64_t pd_capsule_test_space(uint64_t actual){return !strcmp(fault,"no-space")?0:actual;}
static int read_fixed(int parent,const char* name,unsigned char* bytes,size_t size)
{
    int fd=openat(parent,name,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    if(fd<0){return 0;}unsigned char extra;
    int ok=read(fd,bytes,size)==(ssize_t)size && read(fd,&extra,1)==0;close(fd);return ok;
}
int main(int argc,char** argv)
{
    if(argc!=4){return 2;}fault=argv[3];
    int parent=open(argv[1],O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    unsigned char key[32];int ok=parent>=0 && read_fixed(parent,"key",key,sizeof(key));
    if(!ok){return 2;}
    int units=openat(parent,"units",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(!strcmp(argv[2],"publish") || !strcmp(argv[2],"publish-origin") || !strcmp(argv[2],"publish-armed")){
        unsigned char approved[PD_UPGRADE_APPROVAL_SIZE],expected[336];
        struct pd_readiness identity={0};
        ok=read_fixed(parent,"approved",approved,sizeof(approved)) && read_fixed(parent,"expected",expected,sizeof(expected));
        if(ok){memcpy(identity.hostname,expected,64);memcpy(identity.address,expected+64,16);memcpy(identity.dns,expected+80,256);}
        int worker=openat(parent,"worker-input",O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
        if(strcmp(argv[2],"publish")){
            unsigned char bytes[PD_UPGRADE_ORIGIN_SIZE];struct pd_upgrade parsed;struct pd_upgrade_origin origin;
            ok=ok && worker>=0 && read_fixed(parent,"origin",bytes,sizeof(bytes)) &&
                pd_upgrade_parse(approved,sizeof(approved),&parsed) &&
                pd_upgrade_origin_parse(bytes,sizeof(bytes),&parsed,&origin) &&
                (!strcmp(argv[2],"publish-armed")?
                    pd_upgrade_capsule_publish_armed(parent,units,worker,key,approved,&identity,&origin):
                    pd_upgrade_capsule_publish_origin(parent,worker,key,approved,&identity,&origin));
        }else{ok=ok && worker>=0 && pd_upgrade_capsule_publish(parent,worker,key,approved,&identity);}
        if(worker>=0){close(worker);}
    }else if(!strcmp(argv[2],"cleanup") || !strcmp(argv[2],"cleanup-units")){
        int store=openat(parent,"store",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        ok=store>=0 && (!strcmp(argv[2],"cleanup-units")?pd_upgrade_finish_cleanup(parent,units,store,key):
            pd_upgrade_capsule_cleanup_retired(parent,store,key));if(store>=0){close(store);}
    }else if(!strcmp(argv[2],"verify-units")){
        ok=pd_upgrade_units_verify(units);
    }else{
        struct pd_upgrade_capsule capsule;
        ok=pd_upgrade_capsule_open(parent,key,&capsule);
        if(ok && !strcmp(argv[2],"self")){
            int executable=open("/proc/self/exe",O_RDONLY|O_CLOEXEC);
            ok=pd_upgrade_capsule_self(&capsule,executable);if(executable>=0){close(executable);}
        }else if(ok && !strcmp(argv[2],"hold")){
            puts("ready");fflush(stdout);char byte;ssize_t count=read(0,&byte,1);(void)count;
        }else if(ok && (!strcmp(argv[2],"begin") || !strcmp(argv[2],"begin-retry"))){
            ok=pd_upgrade_capsule_begin(&capsule)==(!strcmp(argv[2],"begin")?1:0);
        }else if(ok && !strcmp(argv[2],"retire")){
            int store=openat(parent,"store",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
            ok=store>=0 && pd_upgrade_capsule_retire(&capsule,store);if(store>=0){close(store);}
        }else if(strcmp(argv[2],"open")){ok=0;}
        pd_upgrade_capsule_close(&capsule);
    }
    if(units>=0){close(units);}close(parent);return ok?0:1;
}
