#define _GNU_SOURCE
#include "pd_upload.h"
#include "pd_transport.h"
#include "monocypher.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/capability.h>
#include <netinet/in.h>
#include <poll.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/vfs.h>
#include <time.h>
#include <unistd.h>
enum { kPort=48995 };
static int identity(void)
{
#if defined(PD_REQUEST_TEST) || defined(PD_REQUEST_IDENTITY_TEST)
    uid_t uid=getuid();gid_t gid=getgid();
#else
    uid_t uid=78666;gid_t gid=20;
#endif
    if(getuid()!=uid || geteuid()!=uid || getgid()!=gid || getegid()!=gid){return 0;}
    struct __user_cap_header_struct header={_LINUX_CAPABILITY_VERSION_3,0};struct __user_cap_data_struct caps[2];
    if(syscall(SYS_capget,&header,caps)){return 0;}
    for(unsigned i=0;i<2;++i){if(caps[i].effective || caps[i].permitted || caps[i].inheritable){return 0;}}
    if(!prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0)){return 1;}
#if defined(__arm__) && !defined(__aarch64__)
    return errno==EINVAL;
#else
    return 0;
#endif
}
#ifndef PD_REQUEST_LIBRARY
static int configuration(void)
{
    char path[PATH_MAX];ssize_t n=readlink("/proc/self/exe",path,sizeof(path)-1);
    if(n<=0 || n>=(ssize_t)sizeof(path)-1){return -1;}path[n]=0;
    char* end=strrchr(path,'/');if(!end || strcmp(end,"/pd-request")){return -1;}*end=0;
    int directory=open(path,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(directory<0){return -1;}
    int fd=openat(directory,"config",O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);close(directory);return fd;
}
#endif
static int listen_one(int ready)
{
#ifdef PD_REQUEST_TEST
    const char* inherited=getenv("PD_TEST_LISTENER");
    if(!inherited){return -1;}int listener=atoi(inherited);
#else
    int listener=socket(AF_INET6,SOCK_STREAM|SOCK_CLOEXEC,0),zero=0;
    if(listener<0){return -1;}
    struct sockaddr_in6 address={0};address.sin6_family=AF_INET6;address.sin6_port=htons(kPort);
    if(setsockopt(listener,IPPROTO_IPV6,IPV6_V6ONLY,&zero,sizeof(zero)) ||
       bind(listener,(struct sockaddr*)&address,sizeof(address)) || listen(listener,1)){close(listener);return -1;}
#endif
    if(ready>=0){
        int sent=write(ready,"R",1)==1;close(ready);
        if(!sent){close(listener);return -1;}
    }
    struct pollfd wait={listener,POLLIN,0};int polled=poll(&wait,1,120000);
    int connected=polled==1 && (wait.revents&POLLIN)?accept4(listener,NULL,NULL,SOCK_CLOEXEC):-1;
    close(listener);return connected;
}
static int connect_carrier(const unsigned char receipt[80])
{
    struct sockaddr_un address={0};address.sun_family=AF_UNIX;
    const char prefix[]="aetherpd-bootstrap-",hex[]="0123456789abcdef";
    memcpy(address.sun_path+1,prefix,sizeof(prefix)-1);
    for(unsigned i=0;i<32;++i){address.sun_path[sizeof(prefix)+2*i]=hex[receipt[16+i]>>4];address.sun_path[sizeof(prefix)+2*i+1]=hex[receipt[16+i]&15];}
    socklen_t size=(socklen_t)(offsetof(struct sockaddr_un,sun_path)+sizeof(prefix)+64);
    struct timespec start;if(clock_gettime(CLOCK_MONOTONIC,&start)){return -1;}
    for(;;){
        int fd=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC|SOCK_NONBLOCK,0);if(fd<0){return -1;}
        if(!connect(fd,(struct sockaddr*)&address,size)){return fd;}
        int saved=errno;close(fd);if(saved!=ECONNREFUSED && saved!=ENOENT && saved!=EAGAIN){return -1;}
        struct timespec current;if(clock_gettime(CLOCK_MONOTONIC,&current) || current.tv_sec-start.tv_sec>=60){return -1;}
        struct timespec delay={0,50000000};nanosleep(&delay,NULL);
    }
}
static int run(int config,int ready)
{
    alarm(400);
    unsigned char key[32]={0},receipt[80]={0},challenge[32]={0};
    int configured=config>=0 && pd_transport_config(config,getuid(),key,receipt);if(config>=0){close(config);}
    if(!configured){crypto_wipe(key,sizeof(key));return 3;}
    int channel=listen_one(ready),directory=-1,root=-1,carrier=-1,inputs[4]={-1,-1,-1,-1};int result=6;
    if(channel>=0){directory=open("/dev/shm",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);}
    int uploaded=directory>=0 && pd_upload_receive(channel,directory,key,receipt,inputs,challenge);
    if(uploaded && pd_upload_reply(channel,key,receipt,challenge,1,0)){
        carrier=connect_carrier(receipt);root=open("/",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        int files[5]={root,inputs[0],inputs[1],inputs[2],inputs[3]};
#if defined(PD_REQUEST_TEST) || defined(PD_REQUEST_IDENTITY_TEST)
        uid_t peer=getuid();
#else
        uid_t peer=0;
#endif
        if(carrier>=0 && root>=0 && pd_transport_send(carrier,peer,key,receipt,files)){
            for(unsigned i=0;i<4;++i){if(inputs[i]>=0){close(inputs[i]);inputs[i]=-1;}}
            if(!pd_transport_result_receive(carrier,&result)){result=6;}
        }
        if(!pd_upload_reply(channel,key,receipt,challenge,2,(unsigned)result)){result=6;}
    }
    for(unsigned i=0;i<4;++i){if(inputs[i]>=0){close(inputs[i]);}}
    if(carrier>=0){close(carrier);}if(root>=0){close(root);}if(directory>=0){close(directory);}if(channel>=0){close(channel);}
    crypto_wipe(key,sizeof(key));crypto_wipe(receipt,sizeof(receipt));
    printf("{\"inputs_verified\":%s,\"delivery_result\":%d,\"installation_committed\":false}\n",uploaded?"true":"false",result);
    return result;
}
/* The installed unprivileged shim owns both inherited descriptors. The secret
 * must be an unlinked RAM file, never a file extracted from an upload archive. */
int pd_request_inherited(void)
{
    struct stat config,ready;struct statfs fs;
    if(!identity()){return 2;}
    if(fstat(3,&config) || config.st_nlink || fstatfs(3,&fs) ||
       (unsigned long)fs.f_type!=0x01021994UL || fstat(4,&ready) || !S_ISFIFO(ready.st_mode) ||
       (fcntl(4,F_GETFL)&O_ACCMODE)!=O_WRONLY){return 3;}
    return run(3,4);
}
#ifndef PD_REQUEST_LIBRARY
int main(int argc,char** argv)
{
    if(argc==2 && !strcmp(argv[1],"--install-request")){return pd_request_inherited();}
    if(argc!=1 || !identity()){return 2;}
    return run(configuration(),-1);
}
#endif
