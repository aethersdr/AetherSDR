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
    struct statfs kind={0};struct statvfs space={0};char mount[kFilesystemCapacity];
    kind.f_type=0x01021994;space.f_frsize=4096;space.f_bavail=3;space.f_favail=8;
    assert(filesystem_json(mount,sizeof(mount),"/run",&kind,0,&space,0));
    assert(strstr(mount,"\"type\":\"0x01021994\"") && strstr(mount,"\"tmpfs\":true"));
    assert(strstr(mount,"\"available_bytes\":12288") && strstr(mount,"\"available_inodes\":8"));
    assert(!filesystem_json(mount,1,"/run",&kind,0,&space,0));
    assert(filesystem_json(mount,sizeof(mount),"/run",&kind,EACCES,&space,ENOENT));
    assert(strstr(mount,"\"type\":null") && strstr(mount,"\"tmpfs\":null"));
    assert(strstr(mount,"\"available_bytes\":null") && strstr(mount,"\"available_inodes\":null") && strstr(mount,"\"block_bytes\":null"));
    kind.f_type=0xef53;space.f_bavail=space.f_favail=0;
    assert(filesystem_json(mount,sizeof(mount),"/user",&kind,0,&space,0));
    assert(strstr(mount,"\"tmpfs\":false") && strstr(mount,"\"available_bytes\":0") && strstr(mount,"\"available_inodes\":0"));
    space.f_bavail=(fsblkcnt_t)UINT64_MAX;
    assert(filesystem_json(mount,sizeof(mount),"/run",&kind,0,&space,0));
    assert(strstr(mount,"\"available_bytes\":null"));
    space.f_bavail=3;space.f_frsize=0;
    assert(filesystem_json(mount,sizeof(mount),"/run",&kind,0,&space,0));
    assert(strstr(mount,"\"available_bytes\":null"));
    space.f_frsize=4096;
    assert(filesystem_json(mount,sizeof(mount),"/quoted\"path",&kind,0,&space,0));
    assert(strstr(mount,"/quoted\\\"path"));
    char long_path[160];memset(long_path,'x',sizeof(long_path));long_path[sizeof(long_path)-1]=0;
    assert(!filesystem_json(mount,sizeof(mount),long_path,&kind,0,&space,0));
    puts("PASS: bounded JSON escaping and finite/unlimited scheduling limits");
    return 0;
}
