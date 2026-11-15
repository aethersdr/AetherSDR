#include "comm.h"
#include "pd_core_fence.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <math.h>
/* Synthetic fixed transfer curve for testing waveform admission decisions. */
void warm_seed_curve(void)
{
    IQC q=txa[0].iqc.p;
    for(int i=0;i<CURVE_EMA_PTS;++i) {
        q->m_calavg[0].xs[i]=q->c_calavg[0].xs[i]=q->s_calavg[0].xs[i]=(double)i/(CURVE_EMA_PTS-1);
        q->m_calavg[0].ys[i]=1.25;q->c_calavg[0].ys[i]=cos(0.15);q->s_calavg[0].ys[i]=sin(0.15);
    }
    q->m_calavg[0].count=q->c_calavg[0].count=q->s_calavg[0].count=1;
    q->state=0;q->cset=0;q->run=1;q->busy=0;
    pd_fit_request(0);assert(pd_fit_publish(0)>0);
}
