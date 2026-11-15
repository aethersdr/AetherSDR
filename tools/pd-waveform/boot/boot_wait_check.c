#define _GNU_SOURCE
#include "pd_boot_wait.h"
#include "pd_upgrade_state.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <time.h>
#include <unistd.h>
static int64_t tick;
static unsigned probes;
static int owner=-1,store=-1;
static const char* scenario;
int pd_transaction_checkpoint(const char* point,unsigned phase){(void)point;(void)phase;return 1;}
int __wrap_clock_gettime(clockid_t clock,struct timespec* value)
{
    (void)clock;if(!strcmp(scenario,"clock-error")){errno=EIO;return -1;}
    value->tv_sec=(time_t)(tick/1000);value->tv_nsec=(long)((tick%1000)*1000000);return 0;
}
int __wrap_nanosleep(const struct timespec* value,struct timespec* remaining)
{
    (void)remaining;tick+=(int64_t)value->tv_sec*1000+value->tv_nsec/1000000;
    if(!strcmp(scenario,"sleep-error")){errno=EINVAL;return -1;}
    if(!strcmp(scenario,"interrupted")){errno=EINTR;return -1;}
    return 0;
}
int __wrap_pd_identity_probe(struct pd_identity* identity)
{
    ++probes;tick+=!strcmp(scenario,"slow-timeout")?5000:!strcmp(scenario,"overrun")?61000:100;
    if(!strcmp(scenario,"clock-backwards")){tick=-1000;}
    if(!strcmp(scenario,"worker-loss") || !strcmp(scenario,"worker-loss-ready")){close(owner);owner=-1;}
    if(!strcmp(scenario,"phase-change")){
        int fd=openat(store,"upgrade-transaction",O_WRONLY|O_CLOEXEC);unsigned char phase=5;
        if(fd>=0){pwrite(fd,&phase,1,PD_UPGRADE_STATE_SIZE-1);close(fd);}
    }
    int ready=!strcmp(scenario,"immediate") || !strcmp(scenario,"worker-loss-ready") || !strcmp(scenario,"overrun") ||
        ((!strcmp(scenario,"delayed") || !strcmp(scenario,"interrupted")) && probes==4);
    memset(identity,0,sizeof(*identity));strcpy(identity->model,"FLEX-8600");strcpy(identity->serial,"fixture-8600");
    return ready;
}
int main(int argc,char** argv)
{
    if(argc!=5){return 2;}scenario=argv[2];store=open(argv[1],O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    if(store<0){return 2;}
    if(atoi(argv[4])){
        owner=openat(store,"transaction.lock",O_RDWR|O_CLOEXEC);
        if(owner<0 || flock(owner,LOCK_EX|LOCK_NB)){return 2;}
    }
    struct pd_identity identity;int ok=pd_boot_identity_wait(store,&identity,atoi(argv[3]));
    if(owner>=0){close(owner);}close(store);
    printf("%d %u %lld %s\n",ok,probes,(long long)tick,identity.serial);return 0;
}
