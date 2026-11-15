#define _GNU_SOURCE
#include "pd_endpoint.h"
#include "pd_transport.h"
#include "monocypher.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/capability.h>
#include <stdio.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

static int drop_caps(void)
{
    for(unsigned cap=0;cap<64;++cap){
        int present=prctl(PR_CAPBSET_READ,cap,0,0,0);
        if(present<0){if(errno==EINVAL){break;}return 0;}
        if(present && prctl(PR_CAPBSET_DROP,cap,0,0,0)){return 0;}
    }
    struct __user_cap_header_struct header={_LINUX_CAPABILITY_VERSION_3,0};
    struct __user_cap_data_struct empty[2]={{0},{0}};
    return !syscall(SYS_capset,&header,empty) && !prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0);
}
int main(int argc,char** argv)
{
    (void)argv;
#if !defined(__aarch64__)
    return 2;
#endif
    if(argc!=1 || getuid()!=0 || geteuid()!=0 || !drop_caps()){return 2;}
    alarm(75);
    int config=open("/pd/config",O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    unsigned char key[32],receipt[80];
    if(config<0 || !pd_transport_config(config,0,key,receipt)){return 3;}
    int channel=pd_endpoint_listen(receipt,0),files[5];
    int ok=channel>=0 && pd_transport_receive(channel,78666,key,receipt,files);
    crypto_wipe(key,sizeof(key));
    if(!ok){if(channel>=0){close(channel);}return 3;}
    int copies[7];const int original[7]={config,files[0],files[1],files[2],files[3],files[4],channel};
    for(unsigned i=0;i<7;++i){
        copies[i]=original[i]>=0?fcntl(original[i],F_DUPFD_CLOEXEC,128):-1;
        if(original[i]>=0 && copies[i]<0){return 3;}
    }
    for(unsigned i=0;i<7;++i){if(original[i]>=0){close(original[i]);}}
    const int targets[7]={3,8,0,4,5,6,7};
    for(unsigned i=0;i<7;++i){
        if(copies[i]>=0){if(dup2(copies[i],targets[i])<0){return 3;}close(copies[i]);}
        else{close(targets[i]);}
    }
    alarm(0);
    char* const arguments[]={"pd-carrier",NULL};char* const environment[]={"PATH=/usr/bin:/bin",NULL};
    execve("/pd/carrier",arguments,environment);
    fprintf(stderr,"{\"carrier_error\":\"exec\"}\n");return 3;
}
