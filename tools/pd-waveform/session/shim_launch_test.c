#define main shim_launch_program_main
#include "pd_shim_launch.c"
#undef main
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>

int main(void)
{
    assert(launch_fd("3")==3 && launch_fd("1023")==1023);
    const char* bad[]={NULL,"","2","1024","-3","+3","3 "," 3","3x","00003","/tmp/x"};
    for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);++i){assert(launch_fd(bad[i])==-1);}
    uint32_t handle=0;assert(launch_handle("0x1234abcd",&handle) && handle==0x1234abcd);
    assert(launch_handle("0xABCDEF12",&handle) && handle==0xabcdef12);
    const char* bad_handles[]={NULL,"","0x00000000","0X12345678","12345678","0x123","0x123456789","0x1234567g","0x12345678 "};
    for(unsigned i=0;i<sizeof(bad_handles)/sizeof(bad_handles[0]);++i){assert(!launch_handle(bad_handles[i],&handle));}
    struct stat s={.st_mode=S_IFDIR|0755,.st_uid=0,.st_gid=0};assert(protected_directory(&s));
    struct stat input={.st_mode=S_IFCHR|0666,.st_rdev=makedev(1,3)};
    assert(null_input(O_RDONLY,&input) && null_input(O_RDWR,&input));
    assert(!null_input(O_WRONLY,&input) && !null_input(-1,&input));
    input.st_mode=S_IFSOCK|0600;assert(!null_input(O_RDONLY,&input));
    s.st_mode=S_IFDIR|0775;assert(!protected_directory(&s));
    s.st_mode=S_IFDIR|01777;assert(!protected_directory(&s));
    s.st_mode=S_IFLNK|0755;assert(!protected_directory(&s));
    s.st_mode=S_IFDIR|0755;s.st_uid=78666;assert(!protected_directory(&s));
    s=(struct stat){.st_mode=S_IFREG|0755,.st_uid=0,.st_nlink=1};assert(protected_program(&s));
    s.st_nlink=2;assert(!protected_program(&s));s.st_nlink=1;
    s.st_mode=S_IFREG|04755;assert(!protected_program(&s));
    s.st_mode=S_IFREG|02755;assert(!protected_program(&s));
    s.st_mode=S_IFREG|0775;assert(!protected_program(&s));
    s.st_mode=S_IFREG|0700;assert(!protected_program(&s));
    s.st_mode=S_IFREG|0755;s.st_uid=78666;assert(!protected_program(&s));
    s=(struct stat){.st_mode=S_IFDIR|0700,.st_uid=78666,.st_gid=20};assert(data_directory(&s));
    s.st_mode=S_IFDIR|0755;assert(!data_directory(&s));
    s.st_mode=S_IFDIR|0700;s.st_uid=0;assert(!data_directory(&s));
    int repeated[6]={3,3,5,6,7,8};assert(!handoff_roles(repeated));
    puts("fixed shim launcher: descriptor/handle grammar, protected parent/program and private data-directory policy; socket-free, no root or network mutation");
    return 0;
}
