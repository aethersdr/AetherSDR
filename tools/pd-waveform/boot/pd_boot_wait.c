#define _GNU_SOURCE
#include "pd_boot_wait.h"
#include "pd_transaction.h"
#include <errno.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int64_t now_ms(void)
{
    struct timespec time;if(clock_gettime(CLOCK_MONOTONIC,&time)){return -1;}
    return (int64_t)time.tv_sec*1000+time.tv_nsec/1000000;
}
static int admitted(int store)
{
    int lease=-1;int ok=pd_transaction_runtime_gate(store,&lease);
    if(lease>=0){close(lease);}return ok;
}
int pd_boot_identity_wait(int store,struct pd_identity* identity,int cold)
{
    memset(identity,0,sizeof(*identity));
    if(cold!=0 && cold!=1){return 0;}
    int64_t start=now_ms(),last=start;
    if(start<0 || start>INT64_MAX-60000){return 0;}int64_t deadline=start+(cold?60000:5000);
    for(;;){
        int64_t now=now_ms();
        if(now<last || (cold?deadline-now<5000:now>=deadline) || !admitted(store)){return 0;}last=now;
        struct pd_identity observed={0};int ready=pd_identity_probe(&observed);
        now=now_ms();
        if(now<last || now>deadline || !admitted(store)){return 0;}last=now;
        if(ready){*identity=observed;return 1;}
        if(!cold || deadline-now<5250){return 0;}
        struct timespec pause={0,250000000};
        /* An interrupted delay shortens the wait; the next iteration checks
         * the deadline and live authority before opening another API socket. */
        if(nanosleep(&pause,NULL) && errno!=EINTR){return 0;}
    }
}
