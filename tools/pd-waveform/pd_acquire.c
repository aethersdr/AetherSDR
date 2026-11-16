#define _POSIX_C_SOURCE 200809L
#include "pd_acquire.h"
#include "pd_align.h"
#include "pd_eq.h"
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>

enum { kIdle, kQueued, kRunning, kReady };
struct pd_acquire {
    pthread_t thread;
    sem_t wake;
    atomic_uint state, stop;
    pd_align *estimator;
    double *tx, *rx;
    int capacity, samples, job_eq;
    pd_eq *equalizer;
    double rate;
    uint64_t generation;
    int64_t submitted_ns;
    struct pd_acquire_result result;
};

static int64_t acquisition_clock(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC,&now)) { return 0; }
    return (int64_t)now.tv_sec*1000000000+now.tv_nsec;
}

static void *run(void *arg)
{
    pd_acquire *w=arg;
    /* Linux applies this to the calling worker thread. Permission failure
     * leaves ordinary scheduling; the packet service thread is unchanged. */
    (void)setpriority(PRIO_PROCESS,0,10);
    for (;;) {
        int rc;
        do { rc=sem_wait(&w->wake); } while (rc && errno==EINTR);
        if (rc || atomic_load_explicit(&w->stop,memory_order_acquire)) { break; }
        if (atomic_load_explicit(&w->state,memory_order_acquire)!=kQueued) { continue; }
        atomic_store_explicit(&w->state,kRunning,memory_order_release);
        struct pd_acquire_result r={.generation=w->generation,
            .submitted_ns=w->submitted_ns,.started_ns=acquisition_clock()};
        if (w->job_eq) {
            pd_eq_reset(w->equalizer);
            pd_eq_train(w->equalizer,w->tx,w->rx,w->samples);
            (void)pd_eq_solve(w->equalizer);
        } else {
            r.delay=pd_align_estimate_delay(w->estimator,w->tx,w->rx,w->samples);
            r.envelope_correlation=pd_align_estimate_corr(w->estimator);
            r.phase_correlation=pd_align_phase_corr(w->estimator);
            r.offset_hz=pd_align_offset_hz(w->estimator,w->rate);
        }
        r.completed_ns=acquisition_clock();
        w->result=r;
        atomic_store_explicit(&w->state,kReady,memory_order_release);
    }
    return NULL;
}

static pd_acquire *create_worker(int max_delay,int max_samples,double rate,int taps)
{
    if (max_delay<0 || max_samples<=max_delay || max_samples>65536 || !(rate>0)) { return NULL; }
    pd_acquire *w=calloc(1,sizeof(*w));
    if (!w) { return NULL; }
    atomic_init(&w->state,kIdle);atomic_init(&w->stop,0);
    if (!atomic_is_lock_free(&w->state) || !atomic_is_lock_free(&w->stop)) { free(w);return NULL; }
    w->capacity=max_samples;w->rate=rate;
    w->tx=calloc((size_t)max_samples*2,sizeof(double));
    w->rx=calloc((size_t)max_samples*2,sizeof(double));
    w->estimator=pd_align_create(max_delay,max_delay+4096);
    if (taps) { w->equalizer=pd_eq_create(taps); }
    if (!w->tx || !w->rx || !w->estimator || (taps && !w->equalizer)) {
        pd_eq_destroy(w->equalizer);pd_align_destroy(w->estimator);free(w->tx);free(w->rx);free(w);return NULL;
    }
    if (sem_init(&w->wake,0,0)) {
        pd_eq_destroy(w->equalizer);pd_align_destroy(w->estimator);free(w->tx);free(w->rx);free(w);return NULL;
    }
    pthread_attr_t attr;
    int rc=pthread_attr_init(&attr);
    if (!rc) {
        rc=pthread_attr_setstacksize(&attr,128*1024);
        if (!rc) { rc=pthread_create(&w->thread,&attr,run,w); }
        pthread_attr_destroy(&attr);
    }
    if (rc) {
        sem_destroy(&w->wake);pd_eq_destroy(w->equalizer);pd_align_destroy(w->estimator);
        free(w->tx);free(w->rx);free(w);return NULL;
    }
    return w;
}

pd_acquire *pd_acquire_create(int max_delay,int max_samples,double rate)
{
    return create_worker(max_delay,max_samples,rate,0);
}
pd_acquire *pd_acquire_create_with_eq(int max_delay,int max_samples,double rate,int taps)
{
    if (taps<=0) { return NULL; }
    return create_worker(max_delay,max_samples,rate,taps);
}

void pd_acquire_destroy(pd_acquire *w)
{
    if (!w) { return; }
    atomic_store_explicit(&w->stop,1,memory_order_release);
    sem_post(&w->wake);
    pthread_join(w->thread,NULL);
    sem_destroy(&w->wake);pd_eq_destroy(w->equalizer);pd_align_destroy(w->estimator);
    free(w->tx);free(w->rx);free(w);
}

static int submit_job(pd_acquire *w,int eq,uint64_t generation,const double *tx,const double *rx,int samples)
{
    if (!w || (eq && !w->equalizer) || !generation || !tx || !rx || samples<=0 || samples>w->capacity ||
        atomic_load_explicit(&w->state,memory_order_acquire)!=kIdle) { return 0; }
    memcpy(w->tx,tx,(size_t)samples*2*sizeof(double));
    memcpy(w->rx,rx,(size_t)samples*2*sizeof(double));
    w->samples=samples;w->generation=generation;w->job_eq=eq;
    w->submitted_ns=acquisition_clock();
    atomic_store_explicit(&w->state,kQueued,memory_order_release);
    if (sem_post(&w->wake)) {
        atomic_store_explicit(&w->state,kIdle,memory_order_release);return 0;
    }
    return 1;
}

int pd_acquire_submit(pd_acquire *w,uint64_t generation,const double *tx,const double *rx,int samples)
{
    return submit_job(w,0,generation,tx,rx,samples);
}
int pd_acquire_submit_eq(pd_acquire *w,uint64_t generation,const double *tx,const double *rx,int samples)
{
    return submit_job(w,1,generation,tx,rx,samples);
}
int pd_acquire_take_eq(pd_acquire *w,uint64_t generation,struct pd_eq *destination)
{
    if (!w || !destination || atomic_load_explicit(&w->state,memory_order_acquire)!=kReady) { return 0; }
    int rc=2;
    if (w->job_eq && w->result.generation==generation) {
        rc=pd_eq_copy_solution(destination,w->equalizer) ? 1 : 3;
    }
    atomic_store_explicit(&w->state,kIdle,memory_order_release);
    return rc;
}

int pd_acquire_take(pd_acquire *w,uint64_t generation,struct pd_acquire_result *result)
{
    if (!w || !result || atomic_load_explicit(&w->state,memory_order_acquire)!=kReady) { return 0; }
    int current=!w->job_eq && w->result.generation==generation;
    if (current) { *result=w->result; } else { memset(result,0,sizeof(*result)); }
    atomic_store_explicit(&w->state,kIdle,memory_order_release);
    return current ? 1 : 2;
}
