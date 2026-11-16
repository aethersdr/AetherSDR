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
    puts("PASS: retire privileges before bounded cleanup; fixed constructor readiness grammar, bounded complete-line reads and owned-child cleanup exit classification; no radio or privileges");
    return 0;
}
