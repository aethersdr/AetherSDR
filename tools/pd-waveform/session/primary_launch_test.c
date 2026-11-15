#define PD_PRIMARY_MAIN primary_program_main
#include "pd_primary_launch.c"
#undef PD_PRIMARY_MAIN
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
int main(void)
{
    /* Extra arguments always refuse before touching any fixed installation. */
    char* args[]={"pd-primary-launch","/tmp/untrusted",NULL};
    assert(primary_program_main(2,args)==2);
    if(getuid()!=0){assert(primary_program_main(1,args)==2);}
    const char* const parts[]={"user","aetherpd"};
    assert(!primary_data(-1,parts,2,"state"));
    struct stat st={.st_mode=S_IFDIR|0700,.st_uid=78666,.st_gid=20};
    assert(data_directory(&st));st.st_mode|=0020;assert(!data_directory(&st));
    st.st_mode=S_IFDIR|0700;st.st_uid=0;assert(!data_directory(&st));
    st.st_mode=S_IFDIR|0755;assert(protected_directory(&st));
    st.st_mode|=0002;assert(!protected_directory(&st));
    return 0;
}
