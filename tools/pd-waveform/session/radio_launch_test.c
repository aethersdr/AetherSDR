#define PD_RADIO_MAIN radio_launch_program_main
#include "pd_radio_launch.c"
#undef PD_RADIO_MAIN
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
int main(void)
{
    const char arp[]="{\"event\":\"ready\",\"uid\":65534,\"owned\":true,\"success\":true,\"rf_commands\":[]}\n";
    assert(radio_arp_ready(arp));assert(!radio_arp_ready("{}\n"));assert(!radio_arp_ready("{\"event\":\"ready\",\"uid\":0,\"owned\":true,\"success\":true,\"rf_commands\":[]}\n"));
    const char ready[]="{\"event\":\"ready\",\"stream\":\"0x20000000\",\"client\":\"0x11111111\",\"pan\":\"0x40000001\",\"uid\":78666,\"success\":true,\"issued\":191,\"replies\":191,\"fields\":127,\"parser_failed\":false,\"rf_commands\":[]}\n";
    uint32_t sid=0,pan=0;assert(radio_stream_ready(ready,&sid,&pan) && sid==0x20000000 && pan==0x40000001);assert(!radio_stream_ready("{}\n",&sid,&pan));
    char altered[384];memcpy(altered,ready,sizeof(ready));char* bits=strstr(altered,"\"fields\":127");assert(bits);bits[9]='0';assert(!radio_stream_ready(altered,&sid,&pan));
    memcpy(altered,ready,sizeof(ready));char* identity=strstr(altered,"0x20000000");assert(identity);identity[2]='0';assert(!radio_stream_ready(altered,&sid,&pan));
    memcpy(altered,ready,sizeof(ready));identity=strstr(altered,"0x40000001");assert(identity);identity[2]='+';assert(!radio_stream_ready(altered,&sid,&pan));
    memcpy(altered,ready,sizeof(ready));identity=strstr(altered,"0x40000001");assert(identity);memset(identity+2,'0',8);assert(!radio_stream_ready(altered,&sid,&pan));
    snprintf(altered,sizeof(altered),"%s{\"extra\":1}",ready);assert(!radio_stream_ready(altered,&sid,&pan));
    int pipe_fds[2];assert(!pipe(pipe_fds));assert(write(pipe_fds[1],"ready\nextra",11)==11);char line[16];assert(radio_line(pipe_fds[0],line,sizeof(line),100) && !strcmp(line,"ready\n"));
    close(pipe_fds[1]);assert(!radio_line(pipe_fds[0],line,sizeof(line),100));close(pipe_fds[0]);
    assert(!pipe(pipe_fds));assert(write(pipe_fds[1],"bad\001\n",5)==5);assert(!radio_line(pipe_fds[0],line,sizeof(line),100));close(pipe_fds[0]);close(pipe_fds[1]);
    struct radio_child children[3];
    for(unsigned i=0;i<3;++i){children[i]=(struct radio_child){.pid=fork(),.log=-1};assert(children[i].pid>=0);if(!children[i].pid){_exit(0);}}
    assert(radio_reap(children));
    children[0]=(struct radio_child){.pid=fork(),.log=-1};assert(children[0].pid>=0);if(!children[0].pid){_exit(7);}
    assert(!radio_reap(children));
    puts("PASS: fixed constructor readiness grammar, bounded complete-line reads and owned-child cleanup exit classification; no radio or privileges");
    return 0;
}
