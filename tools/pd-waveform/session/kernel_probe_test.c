#define main kernel_probe_program_main
#include "kernel_probe.c"
#undef main
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>

int main(void)
{
    char output[64];
    assert(escaped(output,sizeof(output),"6.0\"test\\kernel"));
    assert(!strcmp(output,"6.0\\\"test\\\\kernel"));
    assert(escaped(output,sizeof(output),"bad\n\177\200value"));
    assert(!strcmp(output,"bad???value"));
    assert(!escaped(output,1,"a"));
    assert(!escaped(output,2,"\""));
    assert(escaped(output,2,"a") && !strcmp(output,"a"));
    assert(escaped(output,1,"") && !output[0]);
    struct rlimit limits={1,99};char json[192];
    assert(limit_json(json,sizeof(json),&limits,0));
    assert(!strcmp(json,"{\"soft\":1,\"hard\":99,\"soft_unlimited\":false,\"hard_unlimited\":false,\"errno\":0}"));
    limits.rlim_cur=limits.rlim_max=RLIM_INFINITY;
    assert(limit_json(json,sizeof(json),&limits,0));
    assert(strstr(json,"\"soft\":0,\"hard\":0") && strstr(json,"\"soft_unlimited\":true,\"hard_unlimited\":true"));
    assert(!limit_json(json,1,&limits,0));
    assert(limit_json(json,sizeof(json),&limits,EINVAL) && strstr(json,"\"errno\":22"));
    puts("PASS: bounded JSON escaping and finite/unlimited scheduling limits");
    return 0;
}
