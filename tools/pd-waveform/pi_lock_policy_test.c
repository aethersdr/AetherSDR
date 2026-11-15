#define _GNU_SOURCE
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "wdsp_port.h"
#include <assert.h>
#include <errno.h>
#include <setjmp.h>
#include <time.h>
#include <unistd.h>

static jmp_buf failed;
static int fail_call, reached_failure, protocol_calls, recursive_PI, last_call;
static int reject(int call) { last_call=call; return fail_call == call; }
static int attr_init(pthread_mutexattr_t* a) { return reject(1) ? EINVAL : pthread_mutexattr_init(a); }
static int attr_type(pthread_mutexattr_t* a, int value) { return reject(2) ? EINVAL : pthread_mutexattr_settype(a,value); }
static int attr_protocol(pthread_mutexattr_t* a, int value)
{
    ++protocol_calls;
    assert(value == PTHREAD_PRIO_INHERIT);
    return reject(3) ? ENOTSUP : pthread_mutexattr_setprotocol(a,value);
}
static int mutex_init(pthread_mutex_t* m, const pthread_mutexattr_t* a)
{
    int type, protocol;
    assert(!pthread_mutexattr_gettype(a,&type) && !pthread_mutexattr_getprotocol(a,&protocol));
    recursive_PI = type == PTHREAD_MUTEX_RECURSIVE && protocol == PTHREAD_PRIO_INHERIT;
    return reject(4) ? EINVAL : pthread_mutex_init(m,a);
}
static int attr_destroy(pthread_mutexattr_t* a) { return reject(5) ? EINVAL : pthread_mutexattr_destroy(a); }
static int mutex_lock(pthread_mutex_t* m) { return reject(6) ? ENOSYS : pthread_mutex_lock(m); }
static int mutex_unlock(pthread_mutex_t* m) { return reject(7) ? EPERM : pthread_mutex_unlock(m); }
static int mutex_destroy(pthread_mutex_t* m) { return reject(8) ? EBUSY : pthread_mutex_destroy(m); }
_Noreturn static void lock_failure(void) { reached_failure = 1; longjmp(failed,1); }

#define AETHER_PD_PI_CRITICAL_SECTIONS
#define pthread_mutexattr_init attr_init
#define pthread_mutexattr_settype attr_type
#define pthread_mutexattr_setprotocol attr_protocol
#define pthread_mutexattr_destroy attr_destroy
#define pthread_mutex_init mutex_init
#define pthread_mutex_lock mutex_lock
#define pthread_mutex_unlock mutex_unlock
#define pthread_mutex_destroy mutex_destroy
#define abort lock_failure
#include "../../third_party/wdsp/port/wdsp_port.c"
#undef abort
#undef pthread_mutexattr_init
#undef pthread_mutexattr_settype
#undef pthread_mutexattr_setprotocol
#undef pthread_mutexattr_destroy
#undef pthread_mutex_init
#undef pthread_mutex_lock
#undef pthread_mutex_unlock
#undef pthread_mutex_destroy

int main(void)
{
    static CRITICAL_SECTION lock;
    InitializeCriticalSectionAndSpinCount(&lock,2500);
    assert(protocol_calls == 1 && recursive_PI);
    EnterCriticalSection(&lock);
    EnterCriticalSection(&lock);
    LeaveCriticalSection(&lock);
    LeaveCriticalSection(&lock);
    DeleteCriticalSection(&lock);
    for(int call=1;call<=8;++call) {
        fail_call=call;reached_failure=0;
        if(setjmp(failed)==0) {
            InitializeCriticalSection(&lock);
            EnterCriticalSection(&lock);
            LeaveCriticalSection(&lock);
            DeleteCriticalSection(&lock);
            assert(!"pthread failure must retire process before entering an unsafe critical section");
        }
        assert(reached_failure && last_call == call);
        if(call>=5) {
            if(call==7) { assert(!pthread_mutex_unlock(&lock)); }
            assert(!pthread_mutex_destroy(&lock));
        }
    }
    return 0;
}
