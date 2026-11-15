#define main inherited_shim_main
#include "../session/pd_shim_launch.c"
#undef main
#include "pd_store.h"
#include "pd_release_key.h"

/* This one-shot component is removed after the bootstrap transaction. It
 * accepts a regular bundle descriptor, never a source or destination path. */
int main(int argc,char** argv)
{
    int rollback=argc==2 && !strcmp(argv[1],"--rollback");
    if((argc!=1 && !rollback) || getuid()!=0 || geteuid()!=0){return 2;}
#if defined(__aarch64__)
    const uint32_t arch=64;const char* const parts[]={"var","lib","smartsdr","aetherpd"};
#elif defined(__arm__)
    const uint32_t arch=7;const char* const parts[]={"user","aetherpd"};
#else
    const uint32_t arch=0;const char* const parts[]={"user","aetherpd"};
#endif
    if(!arch){return 2;}umask(077);
    int store=open("/",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);struct stat st;
    if(store<0){return 3;}
    int ok=!fstat(store,&st) && protected_directory(&st);
    for(size_t i=0;ok && i<sizeof(parts)/sizeof(parts[0]);++i){
        int next=openat(store,parts[i],O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);close(store);store=next;
        ok=store>=0 && !fstat(store,&st) && protected_directory(&st);
    }
    if(!ok){if(store>=0){close(store);}return 3;}
    int lock=pd_store_lock(store);
    /* The outer installer must stop and retire the complete graph first.
     * Boot uses the same lock, so it cannot recreate RAM during this check. */
    int quiet=lstat("/run/aetherpd",&st)<0 && errno==ENOENT;
    ok=lock>=0 && quiet && (rollback?pd_store_rollback(store,pd_release_key,arch):
        pd_store_update(store,0,pd_release_key,arch));
    if(lock>=0){close(lock);}close(store);return ok?0:4;
}
