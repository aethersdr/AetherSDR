#define _POSIX_C_SOURCE 200809L
#include "pd_core.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <complex.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
static uint32_t sequence=13;
static void feed(pd_core* c,double ceiling,int blocks)
{
    double tx[1024],rx[1024];
    for (int block=0;block<blocks;++block) {
        for (int i=0;i<512;++i) {
            sequence=sequence*1664525u+1013904223u;
            double amplitude=.7*ceiling*((sequence>>8)+.5)/16777216.0;
            double phase=i*.17;tx[2*i]=amplitude*cos(phase);tx[2*i+1]=amplitude*sin(phase);
        }
        pd_core_correct(c,tx,512);
        for (int i=0;i<512;++i) {
            double complex x=tx[2*i]+I*tx[2*i+1];double power=creal(x*conj(x))/.49;
            double complex y=.02*x*(1-.12*power)*cexp(I*.07*power);
            rx[2*i]=creal(y);rx[2*i+1]=cimag(y);
        }
        pd_core_calibrate(c,tx,rx,512);
        struct timespec pause={0,1000000};nanosleep(&pause,NULL);
    }
}
static void acquire_solution(pd_core* c)
{
    struct timespec begin,now;clock_gettime(CLOCK_MONOTONIC,&begin);
    do {
        feed(c,1,64);if (pd_core_current_solution(c)) { return; }
        clock_gettime(CLOCK_MONOTONIC,&now);
    } while ((now.tv_sec-begin.tv_sec)+(now.tv_nsec-begin.tv_nsec)/1e9<12);
    int counts[16]={0};int full=pd_core_collection_counts(c,counts);
    fprintf(stderr,"timed out: state %d fits %d fullbins %d\n",pd_core_state(c),pd_core_fits(c),full);
    assert(pd_core_current_solution(c));
}
int main(void)
{
    alarm(40);pd_core* c=pd_core_create(24000,512,.7);assert(c);pd_core_set_keyed(c,1);
    acquire_solution(c);assert(pd_core_current_solution(c));int before=pd_core_fits(c);
    pd_core_invalidate(c);assert(!pd_core_current_solution(c) && !pd_core_cached_solution(c));
    int counts[16]={0};int full=0;struct timespec begin,now;clock_gettime(CLOCK_MONOTONIC,&begin);
    do {
        feed(c,.8,64);full=pd_core_collection_counts(c,counts);clock_gettime(CLOCK_MONOTONIC,&now);
    } while (full<13 && (now.tv_sec-begin.tv_sec)+(now.tv_nsec-begin.tv_nsec)/1e9<5);
    assert(full==13);
    printf("low-excitation coverage %d/16; upper counts %d/%d; current solution %d\n",full,counts[14],counts[15],pd_core_current_solution(c));fflush(stdout);
    assert(!pd_core_current_solution(c));assert(counts[14]==0 && counts[15]==0);
    acquire_solution(c);assert(pd_core_current_solution(c) && pd_core_fits(c)>before);
    printf("fresh calibration recovered with full amplitude coverage; fits %d -> %d\n",before,pd_core_fits(c));
    pd_core_destroy(c);puts("PASS: invalidation stays bypassed under incomplete amplitude coverage and recovers on fresh full-range data, with no stale-cache reuse");return 0;
}
