#define PD_RADIO_MAIN radio_launch_program_main
#include "pd_radio_launch.c"
#undef PD_RADIO_MAIN
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
static int cleanup_privileged, cleanup_drop_ok, cleanup_reap_ok;
static unsigned cleanup_drop_calls, cleanup_reap_calls;
static struct radio_child* cleanup_expected;
static int retire_test_privileges(void)
{
    ++cleanup_drop_calls;
    if(!cleanup_drop_ok){return 0;}
    cleanup_privileged=0;return 1;
}
static int collect_test_children(struct radio_child children[3])
{
    assert(!cleanup_privileged && children==cleanup_expected);
    ++cleanup_reap_calls;return cleanup_reap_ok;
}
static void check_retirement_order(void)
{
    struct radio_child children[3]={{.pid=-1,.log=-1},{.pid=-1,.log=-1},{.pid=-1,.log=-1}};
    cleanup_expected=children;cleanup_privileged=1;cleanup_drop_ok=cleanup_reap_ok=1;
    cleanup_drop_calls=cleanup_reap_calls=0;
    assert(radio_retire(children,0,retire_test_privileges,collect_test_children));
    assert(cleanup_drop_calls==1 && cleanup_reap_calls==1 && !cleanup_privileged);
    cleanup_privileged=1;cleanup_drop_ok=0;cleanup_drop_calls=cleanup_reap_calls=0;
    assert(!radio_retire(children,0,retire_test_privileges,collect_test_children));
    assert(cleanup_drop_calls==1 && !cleanup_reap_calls && cleanup_privileged);
    cleanup_privileged=0;cleanup_drop_calls=cleanup_reap_calls=0;
    assert(radio_retire(children,78666,retire_test_privileges,collect_test_children));
    assert(!cleanup_drop_calls && cleanup_reap_calls==1);
    cleanup_privileged=1;cleanup_drop_ok=1;cleanup_reap_ok=0;cleanup_drop_calls=cleanup_reap_calls=0;
    assert(!radio_retire(children,0,retire_test_privileges,collect_test_children));
    assert(cleanup_drop_calls==1 && cleanup_reap_calls==1 && !cleanup_privileged);
}
static int child_inspection_mode;
static unsigned child_inspection_calls;
static int inspect_child(idtype_t type,id_t child,siginfo_t* state,int options)
{
    assert(type==P_PID && child==42 && options==(WEXITED|WNOHANG|WNOWAIT));
    ++child_inspection_calls;
    if(child_inspection_mode==1){state->si_pid=42;state->si_code=CLD_EXITED;return 0;}
    if(child_inspection_mode==2){errno=ECHILD;return -1;}
    if(child_inspection_mode==3 && child_inspection_calls==1){errno=EINTR;return -1;}
    if(child_inspection_mode==4){errno=EINVAL;return -1;}
    return 0;
}
static void check_prepared_child_lifetime(void)
{
    for(child_inspection_mode=0;child_inspection_mode<=4;++child_inspection_mode){
        child_inspection_calls=0;
        assert(prepared_child_running(42,inspect_child)==(child_inspection_mode==0 || child_inspection_mode==3));
        assert(child_inspection_calls==(child_inspection_mode==3?2u:1u));
    }
    child_inspection_calls=0;
    assert(!prepared_child_running(0,inspect_child) && !prepared_child_running(1,inspect_child));
    assert(!child_inspection_calls);
    // This is our own inherited readiness gate, not a simulated radio peer.
    int gate[2];assert(!socketpair(AF_UNIX,SOCK_DGRAM|SOCK_CLOEXEC,0,gate));
    pid_t child=fork();assert(child>=0);if(!child){_exit(23);}
    siginfo_t state={0};assert(!waitid(P_PID,(id_t)child,&state,WEXITED|WNOWAIT) && state.si_pid==child);
    int64_t start=prepared_ms();assert(start>=0 && !prepared_wait_ready(child,gate[0]));
    assert(prepared_ms()-start<1000);
    memset(&state,0,sizeof(state));
    assert(!waitid(P_PID,(id_t)child,&state,WEXITED|WNOHANG|WNOWAIT) && state.si_pid==child);
    int status;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==23);
    close(gate[0]);close(gate[1]);
    child=fork();assert(child>=0);
    if(!child){
        int lifetime[2];assert(!pipe(lifetime));
        prepared_stop_child(getpid(),lifetime[1]);
        close(lifetime[0]);_exit(0);
    }
    assert(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
    int lifetime[2];assert(!pipe(lifetime));child=fork();assert(child>=0);
    if(!child){close(lifetime[1]);char byte;(void)read(lifetime[0],&byte,1);_exit(0);}
    close(lifetime[0]);prepared_stop_child(child,lifetime[1]);
    assert(waitpid(child,&status,WNOHANG)==-1 && errno==ECHILD);
}
int main(void)
{
    const char arp[]="{\"event\":\"ready\",\"uid\":65534,\"owned\":true,\"success\":true,\"rf_commands\":[]}\n";
    assert(radio_arp_ready(arp));assert(!radio_arp_ready("{}\n"));assert(!radio_arp_ready("{\"event\":\"ready\",\"uid\":0,\"owned\":true,\"success\":true,\"rf_commands\":[]}\n"));
    const char ready[]="{\"event\":\"ready\",\"stream\":\"0x20000000\",\"client\":\"0x11111111\",\"pan\":\"0x40000001\",\"channel\":1,\"uid\":78666,\"success\":true,\"issued\":191,\"replies\":191,\"fields\":127,\"parser_failed\":false,\"rf_commands\":[]}\n";
    struct pd_iq_selection selection={1,0};
    uint32_t sid=0,pan=0;assert(radio_stream_ready(ready,&sid,&pan,selection) && sid==0x20000000 && pan==0x40000001);assert(!radio_stream_ready("{}\n",&sid,&pan,selection));
    char altered[384];memcpy(altered,ready,sizeof(ready));char* bits=strstr(altered,"\"fields\":127");assert(bits);bits[9]='0';assert(!radio_stream_ready(altered,&sid,&pan,selection));
    memcpy(altered,ready,sizeof(ready));char* identity=strstr(altered,"0x20000000");assert(identity);identity[2]='0';assert(!radio_stream_ready(altered,&sid,&pan,selection));
    memcpy(altered,ready,sizeof(ready));identity=strstr(altered,"0x40000001");assert(identity);identity[2]='+';assert(!radio_stream_ready(altered,&sid,&pan,selection));
    memcpy(altered,ready,sizeof(ready));identity=strstr(altered,"0x40000001");assert(identity);memset(identity+2,'0',8);assert(!radio_stream_ready(altered,&sid,&pan,selection));
    snprintf(altered,sizeof(altered),"%s{\"extra\":1}",ready);assert(!radio_stream_ready(altered,&sid,&pan,selection));
    for(unsigned channel=1;channel<=4;++channel){
        memcpy(altered,ready,sizeof(ready));char* value=strstr(altered,"\"channel\":1");assert(value);value[10]=(char)('0'+channel);
        selection=(struct pd_iq_selection){channel,0x40000001};
        assert(radio_stream_ready(altered,&sid,&pan,selection));
        selection.pan=0x40000002;assert(!radio_stream_ready(altered,&sid,&pan,selection));
        selection.pan=0x40000001;selection.channel=channel==4?1:channel+1;
        assert(!radio_stream_ready(altered,&sid,&pan,selection));
    }
    selection=(struct pd_iq_selection){1,0};
    memcpy(altered,ready,sizeof(ready));char* channel=strstr(altered,"\"channel\":1");assert(channel);channel[10]='0';
    assert(!radio_stream_ready(altered,&sid,&pan,selection));
    channel[10]='5';assert(!radio_stream_ready(altered,&sid,&pan,selection));
    const char* bad_channels[]={"", "01", "+1", " 1", "1 ", "4294967297", "-1", "1.0"};
    for(unsigned i=0;i<sizeof(bad_channels)/sizeof(bad_channels[0]);++i){
        const char* field=strstr(ready,"\"channel\":1");assert(field);
        int prefix=(int)(field-ready)+10;
        snprintf(altered,sizeof(altered),"%.*s%s%s",prefix,ready,bad_channels[i],field+11);
        assert(!radio_stream_ready(altered,&sid,&pan,selection));
    }
    int pipe_fds[2];assert(!pipe(pipe_fds));assert(write(pipe_fds[1],"ready\nextra",11)==11);char line[16];assert(radio_line(pipe_fds[0],line,sizeof(line),100) && !strcmp(line,"ready\n"));
    close(pipe_fds[1]);assert(!radio_line(pipe_fds[0],line,sizeof(line),100));close(pipe_fds[0]);
    assert(!pipe(pipe_fds));assert(write(pipe_fds[1],"bad\001\n",5)==5);assert(!radio_line(pipe_fds[0],line,sizeof(line),100));close(pipe_fds[0]);close(pipe_fds[1]);
    struct radio_child children[3];
    for(unsigned i=0;i<3;++i){children[i]=(struct radio_child){.pid=fork(),.log=-1};assert(children[i].pid>=0);if(!children[i].pid){_exit(0);}}
    assert(radio_reap(children));
    children[0]=(struct radio_child){.pid=fork(),.log=-1};assert(children[0].pid>=0);if(!children[0].pid){_exit(7);}
    assert(!radio_reap(children));
    check_retirement_order();
    check_prepared_child_lifetime();
    puts("PASS: retire privileges before bounded cleanup; promptly reject exited prepared child without reaping, signal only a waitable child; fixed readiness grammar and owned cleanup; no radio or privileges");
    return 0;
}
