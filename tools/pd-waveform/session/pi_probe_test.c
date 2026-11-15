#define _GNU_SOURCE
#include <pthread.h>
#include <errno.h>
#include <unistd.h>
static int injected;
static int test_protocol(pthread_mutexattr_t* attributes,int protocol)
{
    if(injected==4){for(;;){pause();}}
    return injected==1?ENOTSUP:pthread_mutexattr_setprotocol(attributes,protocol);
}
static int test_timedlock(pthread_mutex_t* mutex,const struct timespec* deadline)
{
    if(injected==2){return 0;}
    if(injected==3){for(;;){pause();}}
    return pthread_mutex_timedlock(mutex,deadline);
}
#define pthread_mutexattr_setprotocol test_protocol
#define pthread_mutex_timedlock test_timedlock
#define main kernel_probe_program_main
#include "kernel_probe.c"
#undef main
#undef pthread_mutexattr_setprotocol
#undef pthread_mutex_timedlock
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
int main(void)
{
    struct pi_probe_result result=pi_mutex_probe();
    assert(result.ok && !result.error && result.child_ok);
    injected=1;result=pi_mutex_probe();
    assert(!result.ok && result.error==ENOTSUP && result.child_ok);
    injected=2;result=pi_mutex_probe();
    assert(!result.ok && result.error==EPROTO && result.child_ok);
    struct timespec before,after;assert(!clock_gettime(CLOCK_MONOTONIC,&before));
    injected=3;result=pi_mutex_probe();
    assert(!clock_gettime(CLOCK_MONOTONIC,&after));
    double elapsed=after.tv_sec-before.tv_sec+(after.tv_nsec-before.tv_nsec)/1e9;
    /* The inner 500ms notify deadline exits the entire child, including its
     * blocked thread. The outer two-second watchdog remains a backstop. */
    assert(!result.ok && result.error==ETIMEDOUT && result.child_ok && elapsed<3);
    assert(!clock_gettime(CLOCK_MONOTONIC,&before));
    injected=4;result=pi_mutex_probe();
    assert(!clock_gettime(CLOCK_MONOTONIC,&after));
    elapsed=after.tv_sec-before.tv_sec+(after.tv_nsec-before.tv_nsec)/1e9;
    assert(!result.ok && result.error==ETIMEDOUT && !result.child_ok && elapsed<3);
    puts("PASS: real contended recursive PI, unsupported protocol, false uncontended success and stuck waiter rejection");
    return 0;
}
