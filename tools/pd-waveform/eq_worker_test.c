#include "pd_acquire.h"
#include "pd_eq.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void tick(void) { struct timespec d={0,1000000};nanosleep(&d,NULL); }
static int take(pd_acquire* worker,uint64_t generation,pd_eq* destination)
{
    int result;while(!(result=pd_acquire_take_eq(worker,generation,destination))){tick();}return result;
}
int main(void)
{
    alarm(15);
    enum {N=24576,L=31};
    double* tx=calloc(2*N,sizeof(double));double* rx=calloc(2*N,sizeof(double));assert(tx&&rx);
    uint32_t seed=7;
    for(int i=0;i<2*N;++i){seed=seed*1664525u+1013904223u;tx[i]=(seed>>8)/16777216.0-0.5;rx[i]=0.7*tx[i]+(i>=2?0.1*tx[i-2]:0);}
    pd_eq* direct=pd_eq_create(L);pd_eq* imported=pd_eq_create(L);assert(direct&&imported);
    for(int i=0;i<N;i+=257){int n=N-i<257?N-i:257;pd_eq_train(direct,tx+2*i,rx+2*i,n);}
    assert(pd_eq_solve(direct));
    assert(!pd_eq_copy_solution(direct,direct));
    pd_acquire* worker=pd_acquire_create_with_eq(256,N,24000,L);assert(worker);
    assert(pd_acquire_submit_eq(worker,1,tx,rx,N));assert(!pd_acquire_submit_eq(worker,1,tx,rx,N));
    assert(take(worker,1,imported)==1 && pd_eq_ready(imported));
    assert(pd_eq_trained(direct)==pd_eq_trained(imported));
    assert(pd_eq_residual(direct)==pd_eq_residual(imported));
    double a[512],b[512],c[512],d[512];
    int count=pd_eq_apply(direct,tx,rx,256,a,b);
    assert(pd_eq_apply(imported,tx,rx,256,c,d)==count);
    assert(!memcmp(a,c,(size_t)count*2*sizeof(double)) && !memcmp(b,d,(size_t)count*2*sizeof(double)));
    assert(pd_acquire_submit_eq(worker,2,tx,rx,N));assert(take(worker,3,imported)==2);
    count=pd_eq_apply(direct,tx+512,rx+512,256,a,b);
    assert(pd_eq_apply(imported,tx+512,rx+512,256,c,d)==count);
    assert(!memcmp(a,c,(size_t)count*2*sizeof(double)));
    memset(tx,0,2*N*sizeof(double));memset(rx,0,2*N*sizeof(double));
    assert(pd_acquire_submit_eq(worker,4,tx,rx,N));assert(take(worker,4,imported)==3);
    assert(pd_acquire_submit_eq(worker,5,tx,rx,N));
    struct pd_acquire_result result;int rc;
    while(!(rc=pd_acquire_take(worker,5,&result))){tick();}assert(rc==2);
    assert(pd_acquire_submit_eq(worker,6,tx,rx,N));pd_acquire_destroy(worker);
    pd_eq_destroy(direct);pd_eq_destroy(imported);free(tx);free(rx);
    puts("PASS: background EQ bitwise equivalence, busy bound, stale/wrong-kind rejection, failed solve and in-flight shutdown");
    return 0;
}
