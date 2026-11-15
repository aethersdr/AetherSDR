#ifndef AETHER_PD_PI_PROBE_H
#define AETHER_PD_PI_PROBE_H
#include <pthread.h>

struct pi_probe_result { int ok,error,child_ok; };
struct pi_probe_waiter { pthread_mutex_t* mutex;int notify,error,contended,reacquired; };

static void* pi_probe_wait(void* argument)
{
    struct pi_probe_waiter* waiter=argument;
    struct timespec deadline;
    if(clock_gettime(CLOCK_REALTIME,&deadline)){
        waiter->error=errno;
    }else{
        deadline.tv_nsec+=100000000;
        if(deadline.tv_nsec>=1000000000){++deadline.tv_sec;deadline.tv_nsec-=1000000000;}
        int result=pthread_mutex_timedlock(waiter->mutex,&deadline);
        waiter->contended=result==ETIMEDOUT;
        waiter->error=waiter->contended?0:(result?result:EPROTO);
        if(!result){(void)pthread_mutex_unlock(waiter->mutex);}
    }
    int signal_value=waiter->error;
    if(write(waiter->notify,&signal_value,sizeof(signal_value))!=(ssize_t)sizeof(signal_value)){
        waiter->error=EIO;return NULL;
    }
    if(waiter->error){return NULL;}
    int result=pthread_mutex_lock(waiter->mutex);
    if(result){waiter->error=result;return NULL;}
    result=pthread_mutex_lock(waiter->mutex);
    if(!result){result=pthread_mutex_unlock(waiter->mutex);}
    int released=pthread_mutex_unlock(waiter->mutex);
    waiter->error=result?result:released;
    waiter->reacquired=!waiter->error;
    return NULL;
}

/* The timeout is intentional: the owner holds both recursive acquisitions
 * until the second thread reports ETIMEDOUT. Attribute acceptance alone does
 * not establish that this kernel implements the contended PI futex path. */
static struct pi_probe_result pi_probe_child(void)
{
    struct pi_probe_result reply={0,0,0};
    pthread_mutexattr_t attributes;
    int result=pthread_mutexattr_init(&attributes);
    if(result){reply.error=result;return reply;}
    result=pthread_mutexattr_settype(&attributes,PTHREAD_MUTEX_RECURSIVE);
    if(!result){result=pthread_mutexattr_setprotocol(&attributes,PTHREAD_PRIO_INHERIT);}
    pthread_mutex_t mutex;
    if(!result){result=pthread_mutex_init(&mutex,&attributes);}
    int destroyed=pthread_mutexattr_destroy(&attributes);
    if(result || destroyed){reply.error=result?result:destroyed;return reply;}
    result=pthread_mutex_lock(&mutex);
    if(!result){result=pthread_mutex_lock(&mutex);}
    if(result){reply.error=result;return reply;}
    int ends[2];
    if(pipe(ends)){reply.error=errno;return reply;}
    struct pi_probe_waiter waiter={.mutex=&mutex,.notify=ends[1]};pthread_t thread;
    result=pthread_create(&thread,NULL,pi_probe_wait,&waiter);
    if(result){close(ends[0]);close(ends[1]);reply.error=result;return reply;}
    struct pollfd ready={ends[0],POLLIN,0};int polled;
    do{polled=poll(&ready,1,500);}while(polled<0 && errno==EINTR);
    int notification=EIO;
    if(polled<=0 || !(ready.revents&POLLIN) || read(ends[0],&notification,sizeof(notification))!=(ssize_t)sizeof(notification)){
        /* The enclosing child exits immediately; never join a stuck waiter. */
        reply.error=polled==0?ETIMEDOUT:EIO;return reply;
    }
    result=pthread_mutex_unlock(&mutex);
    if(!result){result=pthread_mutex_unlock(&mutex);}
    if(result){reply.error=result;return reply;}
    result=pthread_join(thread,NULL);close(ends[0]);close(ends[1]);
    if(!result){result=pthread_mutex_destroy(&mutex);}
    reply.error=notification?notification:(result?result:waiter.error);
    reply.ok=!reply.error && waiter.contended && waiter.reacquired;
    return reply;
}

static struct pi_probe_result pi_mutex_probe(void)
{
    struct pi_probe_result result={0,0,0};int ends[2];
    if(pipe(ends)){result.error=errno;return result;}
    pid_t child=fork();
    if(child<0){result.error=errno;close(ends[0]);close(ends[1]);return result;}
    if(!child){
        close(ends[0]);struct pi_probe_result reply=pi_probe_child();
        ssize_t sent=write(ends[1],&reply,sizeof(reply));
        _exit(sent==(ssize_t)sizeof(reply)?0:1);
    }
    close(ends[1]);struct pollfd ready={ends[0],POLLIN,0};int polled;
    do{polled=poll(&ready,1,2000);}while(polled<0 && errno==EINTR && !stopping);
    int got=polled>0 && (ready.revents&POLLIN) && read(ends[0],&result,sizeof(result))==(ssize_t)sizeof(result);
    if(!got){result=(struct pi_probe_result){0,polled==0?ETIMEDOUT:EIO,0};kill(child,SIGKILL);}
    close(ends[0]);int status=0;pid_t waited;
    do{waited=waitpid(child,&status,0);}while(waited<0 && errno==EINTR);
    result.child_ok=got && waited==child && WIFEXITED(status) && WEXITSTATUS(status)==0;
    result.ok=result.ok && result.child_ok;
    return result;
}
#endif
