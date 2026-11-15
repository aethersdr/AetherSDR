#define main waveform_program_main
#include "pd_waveform.c"
#undef main
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>

int main(void)
{
    alarm(60);
    const double phases[]={0,0.5,1,1.5,2,2.5,3,-2.8};
    for (unsigned phase=0;phase<sizeof(phases)/sizeof(phases[0]);++phase) {
        wf_t w={0};assert(wf_init(&w));w.aligned=1;
        pd_align_apply_estimate(w.align,0,1,1,0,RATE);
        uint32_t seed=1357;int blocks=0;
        while (!pd_eq_ready(w.eq) && blocks<5000) {
            double tx[2*BLOCK],rx[2*BLOCK];
            for (int k=0;k<BLOCK;++k) {
                seed=seed*1664525u+1013904223u;double i=((seed>>8)/16777216.0-0.5)*0.3;
                seed=seed*1664525u+1013904223u;double q=((seed>>8)/16777216.0-0.5)*0.3;
                tx[2*k]=i;tx[2*k+1]=q;
                rx[2*k]=i*cos(phases[phase])-q*sin(phases[phase]);
                rx[2*k+1]=i*sin(phases[phase])+q*cos(phases[phase]);
            }
            for (int k=0;k<BLOCK;++k) { fifo_push(&w.ref,tx[2*k],tx[2*k+1]);fifo_push(&w.fbk,rx[2*k],rx[2*k+1]); }
            calibrate(&w);
            struct timespec wait={0,1000000};nanosleep(&wait,NULL);++blocks;
        }
        assert(pd_eq_ready(w.eq));
        double residual=pd_eq_residual(w.eq);
        printf("phase %.2f: residual %.2f dB, blocks %d\n",phases[phase],10*log10(residual+1e-30),blocks);fflush(stdout);
        assert(residual<0.001);
        pd_acquire_destroy(w.acquire);pd_core_destroy(w.core);pd_align_destroy(w.align);pd_eq_destroy(w.eq);pd_eq_destroy(w.trusted_eq);
        free(w.ref.buf);free(w.fbk.buf);free(w.acq_tx);free(w.acq_rx);
    }
    puts("PASS: actual waveform calibration fits a linear path across startup phases");return 0;
}
