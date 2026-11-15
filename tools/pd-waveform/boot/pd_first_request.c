#define _GNU_SOURCE
/* Public one-attempt waveform. It can delegate only its pinned signed worker,
 * first-install plan and release; the transport secret exists only in RAM. */
#include "pd_bootstrap_artifact.h"
#include "pd_bundle.h"
#include "pd_identity.h"
#include "pd_migration.h"
#include "pd_release.h"
#include "pd_release_key.h"
#include "pd_transport.h"
#include "../request/pd_upload.h"
#include "monocypher.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/capability.h>
#include <poll.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int ordinary(void)
{
    if(getuid()!=78666 || geteuid()!=78666 || getgid()!=20 || getegid()!=20){return 0;}
    struct __user_cap_header_struct header={_LINUX_CAPABILITY_VERSION_3,0};
    struct __user_cap_data_struct caps[2];if(syscall(SYS_capget,&header,caps)){return 0;}
    for(unsigned i=0;i<2;++i){if(caps[i].effective || caps[i].permitted || caps[i].inheritable){return 0;}}
    return !prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0);
}
static int directory(void)
{
    char path[PATH_MAX];ssize_t n=readlink("/proc/self/exe",path,sizeof(path)-1);
    if(n<=0 || n>=(ssize_t)sizeof(path)-1){return -1;}path[n]=0;
    char* end=strrchr(path,'/');if(!end || strcmp(end,"/pd-first-request")){return -1;}*end=0;
    return open(path,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
}
static int snapshot(int source,int ram,const char* name,size_t minimum,size_t maximum)
{
    int input=openat(source,name,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);struct stat st;
    int ok=input>=0 && !fstat(input,&st) && S_ISREG(st.st_mode) && st.st_nlink==1 &&
        !(st.st_mode&0022) && st.st_size>=(off_t)minimum && st.st_size<=(off_t)maximum;
    int readonly=-1,writer=ok?pd_upload_memory_file(ram,&readonly):-1;ok=writer>=0;
    unsigned char bytes[16384];off_t offset=0;
    while(ok && offset<st.st_size){
        size_t count=(size_t)(st.st_size-offset);if(count>sizeof(bytes)){count=sizeof(bytes);}
        ssize_t n=pread(input,bytes,count,offset);if(n<0 && errno==EINTR){continue;}
        if(n<=0){ok=0;break;}
        size_t used=0;
        while(used<(size_t)n){ssize_t put=write(writer,bytes+used,(size_t)n-used);
            if(put<0 && errno==EINTR){continue;}if(put<=0){ok=0;break;}used+=(size_t)put;}
        offset+=n;
    }
    if(input>=0){close(input);}if(writer>=0){close(writer);}
    if(!ok){if(readonly>=0){close(readonly);}return -1;}return readonly;
}
static socklen_t endpoint(struct sockaddr_un* address,const char* prefix,const unsigned char digest[32])
{
    memset(address,0,sizeof(*address));address->sun_family=AF_UNIX;size_t n=strlen(prefix);
    memcpy(address->sun_path+1,prefix,n);const char hex[]="0123456789abcdef";
    for(unsigned i=0;i<32;++i){address->sun_path[1+n+2*i]=hex[digest[i]>>4];address->sun_path[2+n+2*i]=hex[digest[i]&15];}
    return (socklen_t)(offsetof(struct sockaddr_un,sun_path)+1+n+64);
}
static int broker_open(const unsigned char record[120])
{
    struct sockaddr_un address;socklen_t length=endpoint(&address,"aetherpd-config-",record+88);
    int fd=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0);if(fd<0){return -1;}
    if(bind(fd,(struct sockaddr*)&address,length) || listen(fd,2)){close(fd);return -1;}return fd;
}
static int broker_reply(int channel,int config,const unsigned char record[120],unsigned* roles)
{
    struct ucred peer;socklen_t length=sizeof(peer);struct timeval timeout={2,0};
    uid_t receiver=0;
#ifdef PD_FIRST_REQUEST_TEST
    receiver=getuid();
#endif
    if(getsockopt(channel,SOL_SOCKET,SO_PEERCRED,&peer,&length) || length!=sizeof(peer) || peer.uid!=receiver ||
       setsockopt(channel,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout)) ||
       setsockopt(channel,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout))){return 0;}
    unsigned char request[129];union {struct cmsghdr align;unsigned char bytes[CMSG_SPACE(16*sizeof(int))];} control={0};
    struct iovec iov={request,sizeof(request)};struct msghdr message={0};message.msg_iov=&iov;message.msg_iovlen=1;
    message.msg_control=control.bytes;message.msg_controllen=sizeof(control.bytes);
    ssize_t received=recvmsg(channel,&message,MSG_CMSG_CLOEXEC);int rights=0;
    if(received<0){return 0;}
    if(message.msg_controllen>sizeof(control.bytes)){message.msg_controllen=sizeof(control.bytes);rights=1;}
    for(size_t cursor=0;cursor+sizeof(struct cmsghdr)<=message.msg_controllen;){
        struct cmsghdr* c=(struct cmsghdr*)(control.bytes+cursor);
        if(c->cmsg_len<CMSG_LEN(0) || c->cmsg_len>message.msg_controllen-cursor){rights=1;break;}
        cursor+=CMSG_SPACE(c->cmsg_len-CMSG_LEN(0));
        rights=1;if(c->cmsg_level==SOL_SOCKET && c->cmsg_type==SCM_RIGHTS && c->cmsg_len>=CMSG_LEN(0)){
            size_t count=(c->cmsg_len-CMSG_LEN(0))/sizeof(int);
            for(size_t i=0;i<count;++i){int incoming;memcpy(&incoming,(unsigned char*)CMSG_DATA(c)+i*sizeof(int),sizeof(int));close(incoming);}
        }
    }
    if(received!=128 || rights || message.msg_flags&(MSG_TRUNC|MSG_CTRUNC) || memcmp(request+8,record,120)){return 0;}
    unsigned role=!memcmp(request,"PDCFGAC1",8)?1:!memcmp(request,"PDCFGDL1",8)?2:0;
    if(!role || (*roles&role)){return 0;}
    union {struct cmsghdr align;unsigned char bytes[CMSG_SPACE(sizeof(int))];} outgoing={0};
    struct iovec reply={(void*)"PDSECR01",8};memset(&message,0,sizeof(message));message.msg_iov=&reply;message.msg_iovlen=1;
    message.msg_control=outgoing.bytes;message.msg_controllen=sizeof(outgoing.bytes);
    struct cmsghdr* c=CMSG_FIRSTHDR(&message);c->cmsg_level=SOL_SOCKET;c->cmsg_type=SCM_RIGHTS;c->cmsg_len=CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c),&config,sizeof(int));
    if(sendmsg(channel,&message,MSG_NOSIGNAL)!=8){return 0;}*roles|=role;return 1;
}
static int sender(int root,const int inputs[3],const unsigned char key[32],const unsigned char receipt[80])
{
    struct sockaddr_un address;socklen_t length=endpoint(&address,"aetherpd-bootstrap-",receipt+16);
    int channel=-1;
    for(unsigned attempt=0;attempt<1200;++attempt){
        channel=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0);if(channel<0){return 6;}
        if(!connect(channel,(struct sockaddr*)&address,length)){break;}
        int error=errno;close(channel);channel=-1;
        if(error!=ECONNREFUSED && error!=ENOENT){return 6;}
        struct timespec delay={0,100000000};nanosleep(&delay,NULL);
    }
    if(channel<0){return 6;}
    int files[5]={root,inputs[0],inputs[1],inputs[2],-1},result=6;
    if(pd_transport_send(channel,0,key,receipt,files)){pd_transport_result_receive(channel,&result);}
    close(channel);return result;
}
int main(int argc,char** argv)
{
    (void)argv;alarm(400);
    if(argc!=1 || !ordinary()){return 2;}
    int source=directory(),ram=open("/dev/shm",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(source<0 || ram<0){return 3;}
    int public=snapshot(source,ram,"request",120,120);
    int inputs[3]={snapshot(source,ram,"worker",PD_BOOTSTRAP_PREFIX+1,PD_BOOTSTRAP_PREFIX+PD_BOOTSTRAP_MAX),
        snapshot(source,ram,"plan",PD_MIGRATION_SIZE,PD_MIGRATION_SIZE),snapshot(source,ram,"release",PD_RELEASE_PREFIX,PD_RELEASE_MAX)};
    close(source);unsigned char record[120],plan[PD_MIGRATION_SIZE],hash[32];struct pd_migration fresh;struct pd_approval target;
    int ok=public>=0 && inputs[0]>=0 && inputs[1]>=0 && inputs[2]>=0 && pread(public,record,120,0)==120 &&
        !memcmp(record,"PDFPUB01PDDELIV1",16) && !memcmp(record+16,"\0\0\0\100\0\0\0\1",8) &&
        pread(inputs[1],plan,sizeof(plan),0)==sizeof(plan) && pd_migration_parse(plan,sizeof(plan),&fresh) && fresh.fresh &&
        pd_approval_parse(fresh.approval,PD_APPROVAL_SIZE,&target) && !memcmp(record+88,fresh.nonce,32);
    if(public>=0){close(public);}
    if(ok){crypto_blake2b(hash,32,plan,sizeof(plan));ok=!crypto_verify32(hash,record+56);}
    struct pd_release release;struct pd_bootstrap_artifact worker={0};struct pd_identity live;
    ok=ok && pd_release_read(inputs[2],pd_release_key,64,1,target.firmware,target.digest,&release) &&
        pd_bootstrap_artifact_read(inputs[0],pd_release_key,64,1,release.manifest+16,target.digest,&worker) &&
        !crypto_verify32(worker.digest,record+24) && pd_identity_probe(&live) &&
        pd_identity_matches(&live,1,target.firmware,target.serial);
    pd_bootstrap_artifact_close(&worker);
    if(!ok){fputs("first request: public intent or signature refused\n",stderr);return 3;}
    unsigned char configuration[120];memcpy(configuration,"PDXFER01",8);memcpy(configuration+40,record+8,80);
    int random=open("/dev/urandom",O_RDONLY|O_CLOEXEC);
    if(random<0 || read(random,configuration+8,32)!=32){return 3;}close(random);
    int config=-1,writer=pd_upload_memory_file(ram,&config);close(ram);
    if(writer<0 || write(writer,configuration,120)!=120){return 3;}close(writer);
    int broker=broker_open(record),root=open("/",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(broker<0 || root<0){return 3;}
    pid_t parent=getpid(),child=fork();
    if(child<0){return 4;}
    if(!child){
        close(broker);close(config);
        if(prctl(PR_SET_PDEATHSIG,SIGKILL) || getppid()!=parent){_exit(6);}
        _exit(sender(root,inputs,configuration+8,record+8));
    }
    close(root);for(unsigned i=0;i<3;++i){close(inputs[i]);}crypto_wipe(configuration,sizeof(configuration));
    unsigned roles=0,connections=0;int status=0,done=0;
    for(unsigned tick=0;tick<2400;++tick){
        pid_t waited=waitpid(child,&status,WNOHANG);if(waited==child){done=1;break;}if(waited<0 && errno!=EINTR){break;}
        struct pollfd ready={broker,POLLIN,0};
        if(poll(&ready,1,100)==1 && (ready.revents&POLLIN)){
            int channel=accept4(broker,NULL,NULL,SOCK_CLOEXEC);
            if(channel>=0){broker_reply(channel,config,record,&roles);close(channel);if(++connections>=8){break;}}
        }
    }
    close(config);close(broker);
    if(!done){kill(child,SIGKILL);while(waitpid(child,&status,0)<0 && errno==EINTR){}}
    int result=done && WIFEXITED(status)?WEXITSTATUS(status):6;
    printf("{\"first_install_delivery_result\":%d,\"installation_committed\":false,\"rf_commands\":[]}\n",result);
    return result;
}
