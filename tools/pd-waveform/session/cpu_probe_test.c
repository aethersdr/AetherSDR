#define _GNU_SOURCE
#include <sys/resource.h>
/* ASan reserves a large virtual address space. Check the exact AS request
 * through this adapter; the real native probe retains its actual OS limit. */
static int memory_limit_requested;
static int fixture_limit(int resource,const struct rlimit* limit)
{
    if(resource==RLIMIT_AS){
        memory_limit_requested=limit->rlim_cur==256UL*1024*1024 && limit->rlim_max==limit->rlim_cur;
        return memory_limit_requested?0:-1;
    }
    return setrlimit(resource,limit);
}
#define setrlimit fixture_limit
#define main cpu_probe_program_main
#include "cpu_probe.c"
#undef main
#undef setrlimit
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
int pd_case_recovery_collection(void){assert(memory_limit_requested);puts("hello \"case\"");return 0;}
int pd_case_eq_phase_startup(void){return 7;}
int pd_case_warm_core(void){raise(SIGTERM);return 0;}
int pd_case_eq_worker(void){for(;;){pause();}return 0;}
int pd_case_dispatch_deadline(void){for(unsigned i=0;i<5000;++i){putchar('x');}return 0;}
int main(void)
{
    struct rlimit no_core={0,0};assert(!setrlimit(RLIMIT_CORE,&no_core));
    assert(signal(SIGCHLD,SIG_DFL)!=SIG_ERR);
    struct result good=run_case(0,UINT64_C(1000000000));assert(good.passed && good.exit_code==0);
    assert(strstr(good.output,"hello \"case\""));
    struct result failed=run_case(1,UINT64_C(1000000000));assert(!failed.passed && failed.exit_code==7);
    struct result signaled=run_case(2,UINT64_C(1000000000));assert(!signaled.passed && signaled.signal_number==SIGTERM);
    pid_t sibling=fork();assert(sibling>=0);if(!sibling){for(;;){pause();}}
    struct result timed=run_case(3,UINT64_C(30000000));assert(!timed.passed && timed.timed_out && timed.signal_number==SIGKILL);
    assert(!kill(sibling,0));assert(!kill(sibling,SIGKILL));assert(waitpid(sibling,NULL,0)==sibling);
    struct result truncated=run_case(4,UINT64_C(1000000000));assert(truncated.passed && truncated.truncated && truncated.used==kOutput-1);
    assert(waitpid(-1,NULL,WNOHANG)==-1 && errno==ECHILD);
    stopping=1;struct result canceled=run_case(0,UINT64_C(1000000000));assert(!canceled.passed && canceled.error==ECANCELED);stopping=0;
    struct result invalid=run_case(kCases,1);assert(!invalid.passed && invalid.error==EINVAL);
    struct result results[kCases]={good,failed,signaled,timed,truncated};struct utsname system={0};
    strcpy(system.release,"2.6.37-test\"kernel");strcpy(system.machine,"armv7l");char output[kReport];
    assert(report(output,sizeof(output),results,&system));assert(strstr(output,"\"passed\":false,\"rf_commands\":[]"));
    assert(strstr(output,"2.6.37-test\\\"kernel"));assert(!report(output,20,results,&system));
    assert(!escaped(output,1,"a"));assert(escaped(output,1,"") && !output[0]);
    puts("PASS: CPU probe result bounds, actual child failures/signals/timeout/reaping and foreign-child preservation");
    return 0;
}
