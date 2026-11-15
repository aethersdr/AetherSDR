#define _POSIX_C_SOURCE 200809L
#include "pd_core.h"
#undef NDEBUG
#include <assert.h>
#include <complex.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
static void learn(double phase,double feedback_gain,double result[720])
{
    pd_core* c=pd_core_create(24000,360,.7);assert(c);pd_core_set_keyed(c,1);
    uint32_t sequence=17;double tx[720],ctx[720],crx[720];
    struct timespec start,now;clock_gettime(CLOCK_MONOTONIC,&start);
    while(!pd_core_current_solution(c)){
        for(int i=0;i<360;++i){sequence=sequence*1664525u+1013904223u;double a=.7*((sequence>>8)+.5)/16777216.0;tx[2*i]=a*cos(i*.17);tx[2*i+1]=a*sin(i*.17);}
        pd_core_correct(c,tx,360);
        for(int i=0;i<360;++i){
            double complex x=tx[2*i]+I*tx[2*i+1];double power=creal(x*conj(x))/.49;
            double complex y=.02*x*(1-.12*power)*cexp(I*.07*power);
            x*=cexp(I*phase);y*=feedback_gain*cexp(I*phase);
            ctx[2*i]=creal(x);ctx[2*i+1]=cimag(x);crx[2*i]=creal(y);crx[2*i+1]=cimag(y);
        }
        pd_core_calibrate(c,ctx,crx,360);
        struct timespec pause={0,1000000};nanosleep(&pause,NULL);
        clock_gettime(CLOCK_MONOTONIC,&now);assert(now.tv_sec-start.tv_sec<12);
    }
    for(int repeat=0;repeat<3;++repeat){
        for(int i=0;i<360;++i){double a=.7*(i+.5)/360;result[2*i]=a*cos(i*.13);result[2*i+1]=a*sin(i*.13);}
        pd_core_correct(c,result,360);
    }
    pd_core_destroy(c);
}
int main(void)
{
    alarm(60);double baseline[720],rotated[720];learn(0,1,baseline);
    const double phases[]={.2,1.0,2.0,-2.0};
    for(unsigned p=0;p<sizeof(phases)/sizeof(phases[0]);++p){
        learn(phases[p],1,rotated);double error=0,energy=0;
        for(int i=0;i<720;++i){double d=rotated[i]-baseline[i];error+=d*d;energy+=baseline[i]*baseline[i];}
        double relative=sqrt(error/energy);printf("common calibration phase %.2f: curve RMS difference %.9g\n",phases[p],relative);assert(relative<1e-6);
    }
    const double gains[]={.5,2.0};
    for(unsigned g=0;g<sizeof(gains)/sizeof(gains[0]);++g){
        learn(0,gains[g],rotated);double error=0,energy=0;
        for(int i=0;i<720;++i){double d=rotated[i]-baseline[i];error+=d*d;energy+=baseline[i]*baseline[i];}
        double relative=sqrt(error/energy);printf("feedback gain %.2f: curve RMS difference %.9g\n",gains[g],relative);assert(relative<1e-6);
    }
    puts("PASS: WDSP PA curve invariant to common calibration phase and feedback gain");return 0;
}
