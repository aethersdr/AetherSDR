/* Disposable host fixture for real exec/descriptor transfer, never a radio entry. */
#define _GNU_SOURCE
#include "pd_handoff.h"
#include "pd_bundle.h"
#include "pd_release_key.h"
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

static int child(int argc,char** argv)
{
    char path[PATH_MAX],cwd[4];ssize_t length=readlink("/proc/self/fd/64",path,sizeof(path)-1);
    if(length<=0 || length>=(ssize_t)sizeof(path)-1){return 3;}path[length]=0;
    char* slash=strrchr(path,'/');if(!slash){return 3;}*slash=0;
    int control=open(path,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(control<0){return 3;}
    int fault=openat(control,"wrong-self",O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    if(fault>=0){if(dup2(fault,PD_HANDOFF_BOOT)<0){return 3;}close(fault);}
    int modern=getenv("AETHERPD_PAIRED_BOOT")!=NULL;
    int payload=modern?pd_handoff_receive(control,pd_release_key,64):fcntl(PD_HANDOFF_PAYLOAD,F_DUPFD_CLOEXEC,3);
    unsigned char digest[32];
    int ok=payload>=0 && pd_bundle_hash(payload,digest) && getcwd(cwd,sizeof(cwd)) && !strcmp(cwd,"/") &&
        !getenv("HANDOFF_TEST_SECRET") && getenv("PATH") && !strcmp(getenv("PATH"),"/usr/bin:/bin") &&
        fcntl(73,F_GETFD)<0 && (argc==1 || (argc==2 && !strcmp(argv[1],"--socket-owner")));
    if(!ok){if(payload>=0){close(payload);}close(control);return 1;}
    printf("{\"paired\":%d,\"socket_owner\":%d,\"payload\":\"",modern,argc==2);
    for(unsigned i=0;i<32;++i){printf("%02x",digest[i]);}puts("\"}");fflush(stdout);
    char byte;ok=read(STDIN_FILENO,&byte,1)==1;close(payload);close(control);return ok?0:1;
}
int main(int argc,char** argv)
{
    if(!strcmp(argv[0],"aetherpd-paired-boot")){return child(argc,argv);}
    if(argc!=4){return 2;}
    int root=open(argv[1],O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    int control=openat(root,"control",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    struct pd_pair pair={.lease=-1,.boot=-1,.payload=-1,.choice=0,.boot_handoff=!strcmp(argv[2],"paired")};
    pair.lease=openat(control,!strcmp(argv[3],"wrong-lease")?"other-lock":"lock",O_RDWR|O_NOFOLLOW|O_CLOEXEC);
    pair.payload=openat(root,"payload",(!strcmp(argv[3],"writable-payload")?O_RDWR:O_RDONLY)|O_NOFOLLOW|O_CLOEXEC);
    pair.boot=openat(root,"boot",O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    int sentinel=open("/dev/null",O_RDONLY|O_CLOEXEC);
    if(root<0 || control<0 || pair.lease<0 || pair.boot<0 || pair.payload<0 || sentinel<0 ||
        flock(pair.lease,LOCK_SH|LOCK_NB) || dup2(sentinel,73)<0){return 3;}
    (void)pd_handoff_exec(&pair,!strcmp(argv[3],"socket-owner"));return 4;
}
