#define main inherited_shim_main
#include "pd_shim_launch.c"
#undef main
#include <sched.h>
#include <sys/resource.h>
#include "../boot/pd_transaction.h"
#include "../request/pd_request.h"

/* The boot service runs one protected sibling as the waveform user. Neither
 * inherited environment nor arguments select a program, state path or port. */
static int primary_data(int root,const char* const* parts,size_t count,const char* leaf)
{
    struct stat st;
    int base=fcntl(root,F_DUPFD_CLOEXEC,3);
    if(base<0){return 0;}
    for(size_t i=0;i<count;++i){
        int next=openat(base,parts[i],O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);close(base);
        if(next<0){return 0;}base=next;
        if(fstat(base,&st) || !protected_directory(&st)){close(base);return 0;}
    }
    int valid=check_data(base,leaf);close(base);return valid;
}
static int primary_other(void)
{
    struct sched_param policy={0};
    if(syscall(SYS_sched_setscheduler,0,SCHED_OTHER,&policy) || setpriority(PRIO_PROCESS,0,10)){return 0;}
    errno=0;int priority=getpriority(PRIO_PROCESS,0);
    return !errno && priority==10 && syscall(SYS_sched_getscheduler,0)==SCHED_OTHER;
}
#ifndef PD_PRIMARY_MAIN
#define PD_PRIMARY_MAIN main
#endif
int PD_PRIMARY_MAIN(int argc,char** argv)
{
    if(argc==2 && !strcmp(argv[1],"--install-request")){return pd_request_inherited();}
    if(argc!=1 || getuid()!=0 || geteuid()!=0){return 2;}
    char path[PATH_MAX];int directory=executable_directory(path);
    if(directory<0){return 3;}
    int program=open_program(directory,kShim);close(directory);
    if(program<0){return 3;}
    /* On the qualified ARM64 host /user is an alias. Use its fixed canonical
     * location so every persistent path component remains NOFOLLOW-checked.
     * The ARMv7 package targets the reported 6300 /user mount, still awaiting
     * execution on that platform. Unknown layouts fail closed. */
#if defined(__aarch64__)
    const char* const persistent[]={"var","lib","smartsdr","aetherpd"};
    char state[]="/var/lib/smartsdr/aetherpd/state";
#else
    const char* const persistent[]={"user","aetherpd"};
    char state[]="/user/aetherpd/state";
#endif
    const char* const runtime[]={"run","aetherpd"};
    int root=open("/",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);struct stat st;
    int layout=root>=0 && !fstat(root,&st) && protected_directory(&st) &&
        primary_data(root,persistent,sizeof(persistent)/sizeof(persistent[0]),"state") &&
        primary_data(root,runtime,sizeof(runtime)/sizeof(runtime[0]),"primary");
    if(root>=0){close(root);}
    int installation=-1;
    if(!layout || !pd_transaction_runtime_open(&installation)){close(program);return 4;}
    if(installation>=0){close(installation);}
    int kept[6]={0,1,2,0,1,2};
    umask(077);
    if(chdir("/") || !close_unrelated(kept,program) || !primary_other() || !drop_root()){
        close(program);return 5;
    }
    /* Linux2.6 lacks NO_NEW_PRIVS. The bounding set and UID drop are required
     * on both generations; NNP is additional protection where available. */
    if(prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0) && errno!=EINVAL){close(program);return 5;}
    char* args[]={"flex-tailnet-shim","-state",state,
        "-run-dir","/run/aetherpd/primary","-radio","127.0.0.1","-api",":48992",NULL};
    char* env[]={"PATH=/usr/bin:/bin","TS_NO_LOGS_NO_SUPPORT=true",NULL};
    fexecve(program,args,env);close(program);return 6;
}
