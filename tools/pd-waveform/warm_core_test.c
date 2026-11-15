#include "comm.h"
#include "pd_core_fence.h"
#include "pd_core.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <math.h>
#include <stdio.h>

static void seed_curve(pd_core* core)
{
    IQC q=txa[0].iqc.p;
    for(int i=0;i<CURVE_EMA_PTS;++i) {
        q->m_calavg[0].xs[i]=q->c_calavg[0].xs[i]=q->s_calavg[0].xs[i]=(double)i/(CURVE_EMA_PTS-1);
        q->m_calavg[0].ys[i]=1.25;q->c_calavg[0].ys[i]=cos(0.15);q->s_calavg[0].ys[i]=sin(0.15);
    }
    q->m_calavg[0].count=q->c_calavg[0].count=q->s_calavg[0].count=1;
    q->state=0;q->cset=0;q->run=1;q->busy=0;
    pd_fit_request(0);assert(pd_fit_publish(0)>0);
    pd_core_set_keyed(core,1);
}
static void fill(double* data,int n)
{
    for(int i=0;i<n;++i){data[2*i]=0.1;data[2*i+1]=-0.2;}
}
int main(void)
{
    pd_core* core=pd_core_create(24000,64,0.7);assert(core);
    seed_curve(core);
    double reference[128];fill(reference,64);pd_core_correct(core,reference,64);
    pd_fit_request(0); // queued fit from the old burst must not publish later
    pd_core_pause(core,1);
    assert(pd_core_cached_solution(core) && !pd_core_current_solution(core));
    assert(pd_fit_publish(0)==0 && pd_core_rejected_fits(core)==1);
    IQC q=txa[0].iqc.p;
    for(int i=0;i<CURVE_EMA_PTS;++i){q->m_calavg[0].ys[i]=1.6;}
    double data[128];fill(data,64);pd_core_correct_cached(core,data,64);
    assert(fabs(data[0]-0.1)<1e-14); // unkeyed cache cannot apply
    pd_core_set_keyed(core,1);
    for(int block=0;block<3;++block) {
        fill(data,64);pd_core_correct_cached(core,data,64);
        if(!block){assert(fabs(data[0]-0.1)<1e-14);}
    }
    for(int i=0;i<128;++i){assert(fabs(data[i]-reference[i])<1e-14);}
    fill(data,64);pd_core_correct(core,data,64);assert(fabs(data[0]-0.1)<1e-14);
    // A new same-configuration solution fades from cached output, not clean input.
    pd_fit_request(0);assert(pd_fit_publish(0)>0);
    fill(data,64);pd_core_correct(core,data,64);
    assert(fabs(data[0]-reference[0])<1e-14);
    for(int block=0;block<2;++block){fill(data,64);pd_core_correct(core,data,64);}
    for(int i=0;i<128;++i){assert(fabs(data[i]-reference[i]*1.6/1.25)<1e-14);}
    // Cache survives another clean pause even before a replacement fit exists.
    pd_core_pause(core,1);assert(pd_core_cached_solution(core));
    pd_core_invalidate(core);assert(!pd_core_cached_solution(core));
    pd_core_set_keyed(core,1);fill(data,64);pd_core_correct_cached(core,data,64);
    assert(fabs(data[0]-0.1)<1e-14);
    seed_curve(core);q->m_calavg[0].ys[7]=NAN;
    pd_core_pause(core,1);assert(!pd_core_cached_solution(core));
    pd_core_destroy(core);
    printf("PASS: warm snapshot is independent, unkeyed bypass, 5 ms ramp, stale-fit rejection, hard invalidation; %zu coefficient bytes in RAM\n",3*sizeof(CurveEMA));
    return 0;
}
