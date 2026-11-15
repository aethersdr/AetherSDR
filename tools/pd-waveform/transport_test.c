#define main pd_waveform_program_main
#include "pd_waveform.c"
#undef main
#undef NDEBUG
#include <assert.h>
#include <complex.h>

static unsigned test_sequence=17;
static void model_feed(pd_core* core)
{
    double tx[720],rx[720];
    for(int i=0;i<360;++i){
        test_sequence=test_sequence*1664525u+1013904223u;
        double a=.7*((test_sequence>>8)+.5)/16777216.0;
        tx[2*i]=a*cos(i*.17);tx[2*i+1]=a*sin(i*.17);
    }
    pd_core_correct(core,tx,360);
    for(int i=0;i<360;++i){
        double complex x=tx[2*i]+I*tx[2*i+1];
        double power=creal(x*conj(x))/.49;
        double complex y=.02*x*(1-.12*power)*cexp(I*.07*power);
        rx[2*i]=creal(y);rx[2*i+1]=cimag(y);
    }
    pd_core_calibrate(core,tx,rx,360);
    struct timespec pause={0,1000000};nanosleep(&pause,NULL);
}
static void fit_eq(pd_eq* eq,double phase,double echo)
{
    double tx[8192],rx[8192];double complex previous=0;
    for(int i=0;i<4096;++i){
        test_sequence=test_sequence*1664525u+1013904223u;
        double a=((test_sequence>>8)+.5)/16777216.0-.5;
        double complex x=a*cexp(I*i*.193);
        double complex y=.02*(x+echo*previous)*cexp(I*phase);previous=x;
        tx[2*i]=creal(x);tx[2*i+1]=cimag(x);
        rx[2*i]=creal(y);rx[2*i+1]=cimag(y);
    }
    pd_eq_reset(eq);pd_eq_train(eq,tx,rx,4096);assert(pd_eq_solve(eq));
}
static void known_path_eq(pd_eq* eq,int voice,double echo,double phase)
{
    double tx[8192],rx[8192];double complex history[4]={0};cbp_t filter;
    cbp_init(&filter,200,2800);
    for(int i=0;i<4096;++i){
        test_sequence=test_sequence*1664525u+1013904223u;
        double u=((test_sequence>>8)+.5)/16777216.0-.5;
        double re,im;
        if(voice){cbp_step(&filter,u,&re,&im);}
        else{
            re=u;test_sequence=test_sequence*1664525u+1013904223u;
            im=((test_sequence>>8)+.5)/16777216.0-.5;
        }
        double complex x=re+I*im;
        double complex y=.02*(x+echo*history[3])*cexp(I*phase);
        for(int j=3;j>0;--j){history[j]=history[j-1];}history[0]=x;
        tx[2*i]=re;tx[2*i+1]=im;rx[2*i]=creal(y);rx[2*i+1]=cimag(y);
    }
    pd_eq_reset(eq);pd_eq_train(eq,tx,rx,4096);assert(pd_eq_solve(eq));
}
static void excitation_regression(void)
{
    pd_eq* broad=pd_eq_create(31);pd_eq* voice=pd_eq_create(31);assert(broad && voice);
    known_path_eq(broad,0,.2,0);known_path_eq(voice,1,.2,1.0);
    double same=pd_eq_solution_distance(broad,voice);
    known_path_eq(voice,1,.8,1.0);double changed=pd_eq_solution_distance(broad,voice);
    printf("broadband/voice prediction-shape error: same %.6f changed %.6f\n",same,changed);
    assert(same<.05 && changed>.05);
    pd_eq_reset(voice);assert(!isfinite(pd_eq_solution_distance(broad,voice)));
    pd_eq_destroy(broad);pd_eq_destroy(voice);
}
static void prepare(wf_t* w)
{
    local_feedback_invalidate(w,1);
    w->keyed=1;pd_core_set_keyed(w->core,1);
    int64_t end=pd_iq_monotonic_ns()+INT64_C(12000000000);
    while(!pd_core_current_solution(w->core) && pd_iq_monotonic_ns()<end){model_feed(w->core);}
    assert(pd_core_current_solution(w->core));
    w->transport_cache_enabled=1;w->local_iq_mode=1;w->local_iq_fault=0;
    w->correct=1;w->feedback_ok=1;w->aligned=1;w->tx_guard.seen=1;w->tx_guard.reset_pending=0;
    fit_eq(w->trusted_eq,0,0);assert(pd_eq_copy_solution(w->eq,w->trusted_eq));
    w->local_iq.last_valid_ns=pd_iq_monotonic_ns();
    w->trusted_fit_ns=pd_iq_monotonic_ns();
}
static void fresh_path(wf_t* w)
{
    assert(pd_eq_copy_solution(w->eq,w->trusted_eq));w->aligned=1;
    ++w->local_iq.frames;w->local_iq.last_valid_ns=pd_iq_monotonic_ns();
}
static void assert_bypass(wf_t* w)
{
    double original[720],output[720];
    for(int i=0;i<720;++i){original[i]=.1*cos(i*.13);}
    memcpy(output,original,sizeof(output));pd_core_correct_cached(w->core,output,360);
    for(int i=0;i<720;++i){assert(fabs(output[i]-original[i])<1e-14);}
    assert(!correction_ready(w));assert(!pd_core_current_solution(w->core));
    assert(!pd_core_cached_solution(w->core));
}
int main(void)
{
    alarm(120);excitation_regression();wf_t w;assert(wf_init(&w));
    prepare(&w);w.transport_cache_enabled=0;local_feedback_suspend_transport(&w);
    assert(!w.transport_pending && !pd_core_transport_pending(w.core));assert_bypass(&w);
    prepare(&w);local_feedback_suspend_transport(&w);
    assert(w.transport_pending && pd_core_transport_pending(w.core));assert_bypass(&w);
    fresh_path(&w);local_feedback_validate_transport(&w);
    assert(!w.transport_pending && !pd_core_transport_pending(w.core));
    assert(w.warm_start_armed && pd_core_cached_solution(w.core) && correction_ready(&w));
    assert(w.transport_recoveries==1);
    double corrected[720],change=0;
    for(int i=0;i<720;++i){corrected[i]=.1*cos(i*.13);}
    pd_core_correct_cached(w.core,corrected,360);
    for(int i=0;i<720;++i){
        assert(isfinite(corrected[i]) && fabs(corrected[i])<.3);
        change+=fabs(corrected[i]-.1*cos(i*.13));
    }
    assert(change>1e-5);
    local_feedback_suspend_transport(&w);fresh_path(&w);fit_eq(w.eq,.2,0);
    local_feedback_validate_transport(&w);assert(correction_ready(&w) && w.transport_recoveries==2);
    local_feedback_suspend_transport(&w);assert(w.transport_pending);assert_bypass(&w);
    local_feedback_invalidate(&w,1);assert(!pd_core_resume_transport(w.core));assert_bypass(&w);
    prepare(&w);local_feedback_suspend_transport(&w);fresh_path(&w);
    fit_eq(w.eq,0,.2);assert(pd_eq_solution_distance(w.trusted_eq,w.eq)>.05);
    local_feedback_validate_transport(&w);assert(!w.transport_pending);assert_bypass(&w);
    prepare(&w);local_feedback_suspend_transport(&w);fresh_path(&w);
    w.local_iq.last_valid_ns-=200000000;local_feedback_validate_transport(&w);assert_bypass(&w);
    prepare(&w);local_feedback_suspend_transport(&w);fresh_path(&w);
    w.local_iq.frames=w.key_iq_frames;local_feedback_validate_transport(&w);assert_bypass(&w);
    prepare(&w);local_feedback_suspend_transport(&w);fresh_path(&w);
    w.native_apd_enabled=1;local_feedback_validate_transport(&w);assert_bypass(&w);w.native_apd_enabled=0;
    prepare(&w);local_feedback_suspend_transport(&w);fresh_path(&w);
    set_keyed(&w,0,1);assert(!w.transport_pending && !pd_core_transport_pending(w.core));
    assert(!pd_core_resume_transport(w.core));assert_bypass(&w);
    prepare(&w);w.trusted_fit_ns-=INT64_C(31000000000);
    local_feedback_suspend_transport(&w);assert(!w.transport_pending);assert_bypass(&w);
    prepare(&w);local_feedback_suspend_transport(&w);fresh_path(&w);
    w.trusted_fit_ns-=INT64_C(31000000000);local_feedback_validate_transport(&w);assert_bypass(&w);
    prepare(&w);local_feedback_suspend_transport(&w);
    char power[]="S1|transmit rfpower=9";api_line(&w,power);
    assert(!w.transport_pending && !pd_core_transport_pending(w.core));assert_bypass(&w);
    // Owner death may end the process while the curve is still quarantined.
    // Destroy the active workers and retained curve without an intervening unkey.
    prepare(&w);local_feedback_suspend_transport(&w);
    assert(w.transport_pending && pd_core_transport_pending(w.core));assert_bypass(&w);
    pd_acquire_destroy(w.acquire);pd_core_destroy(w.core);
    pd_align_destroy(w.align);pd_eq_destroy(w.eq);pd_eq_destroy(w.trusted_eq);
    free(w.ref.buf);free(w.fbk.buf);free(w.acq_tx);free(w.acq_rx);
    puts("PASS: opt-in transport quarantine bypasses pending curves; fresh equivalent EQ resumes; configuration invalidation, changed EQ, stale/missing IQ, APD and unkey prevent reuse");return 0;
}
