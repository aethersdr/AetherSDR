#define _GNU_SOURCE
#include "pd_runtime_config.h"
#include "pd_transport.h"
#include "../request/pd_upload.h"
#include "monocypher.h"
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/vfs.h>
#include <unistd.h>

static unsigned number(const unsigned char* p)
{
    return (unsigned)p[0]<<24|(unsigned)p[1]<<16|(unsigned)p[2]<<8|p[3];
}
static int public_record(int input,unsigned char bytes[120])
{
    struct stat st;int flags=fcntl(input,F_GETFL);
    if(flags<0 || (flags&O_ACCMODE)!=O_RDONLY || fstat(input,&st) || !S_ISREG(st.st_mode) ||
       st.st_uid!=geteuid() || st.st_nlink!=1 || (st.st_mode&07022) || st.st_size!=120 ||
       pread(input,bytes,120,0)!=120 || memcmp(bytes+8,"PDDELIV1",8) || number(bytes+16)!=64){return 0;}
    int upgrade=!memcmp(bytes,"PDPUB001",8) && number(bytes+20)==2;
    int first=!memcmp(bytes,"PDFPUB01",8) && number(bytes+20)==1;
    if(!upgrade && !first){return 0;}
    unsigned nonzero=0;for(unsigned i=88;i<120;++i){nonzero|=bytes[i];}return nonzero!=0;
}
static int receive_file(int channel)
{
    unsigned char reply[9];union {struct cmsghdr align;unsigned char bytes[CMSG_SPACE(8*sizeof(int))];} control={0};
    struct iovec iov={reply,sizeof(reply)};struct msghdr message={0};message.msg_iov=&iov;message.msg_iovlen=1;
    message.msg_control=control.bytes;message.msg_controllen=sizeof(control.bytes);
    ssize_t size=recvmsg(channel,&message,MSG_CMSG_CLOEXEC);int fd=-1,count=0;
    int ok=size==8 && !memcmp(reply,"PDSECR01",8) && !(message.msg_flags&(MSG_TRUNC|MSG_CTRUNC));
    if(size<0){return -1;}
    if(message.msg_controllen>sizeof(control.bytes)){ok=0;message.msg_controllen=sizeof(control.bytes);}
    for(size_t cursor=0;cursor+sizeof(struct cmsghdr)<=message.msg_controllen;){
        struct cmsghdr* header=(struct cmsghdr*)(control.bytes+cursor);
        if(header->cmsg_len<CMSG_LEN(0) || header->cmsg_len>message.msg_controllen-cursor){ok=0;break;}
        size_t payload=header->cmsg_len-CMSG_LEN(0);cursor+=CMSG_SPACE(payload);
        if(header->cmsg_level!=SOL_SOCKET || header->cmsg_type!=SCM_RIGHTS){ok=0;continue;}
        if(payload%sizeof(int)){ok=0;}
        for(size_t offset=0;offset+sizeof(int)<=payload;offset+=sizeof(int)){
            int incoming;memcpy(&incoming,(unsigned char*)CMSG_DATA(header)+offset,sizeof(incoming));
            if(!count){fd=incoming;}else{close(incoming);}++count;
        }
    }
    if(!ok || count!=1){if(fd>=0){close(fd);}return -1;}return fd;
}
static int copy_config(int incoming,uid_t owner,const unsigned char public_bytes[120])
{
    unsigned char key[32]={0},receipt[80]={0},bytes[120]={0},digest[32];struct stat st;struct statfs fs;
    int ok=!fstat(incoming,&st) && !st.st_nlink && !fstatfs(incoming,&fs) &&
        (unsigned long)fs.f_type==0x01021994UL && pd_transport_config(incoming,owner,key,receipt) &&
        !memcmp(receipt,public_bytes+8,80) && pread(incoming,bytes,120,0)==120;
    crypto_blake2b_ctx hash;crypto_blake2b_init(&hash,32);
    crypto_blake2b_update(&hash,(const unsigned char*)"PDKEY001",8);crypto_blake2b_update(&hash,bytes,120);
    crypto_blake2b_final(&hash,digest);
    /* A fresh bootstrap generates its transport secret in RAM. The public
     * upload pins one exact worker/plan and nonce; same-UID peers can only
     * invoke that already approved transaction. Upgrades retain key binding. */
    ok=ok && (!memcmp(public_bytes,"PDFPUB01",8) || !crypto_verify32(digest,public_bytes+88));
    int directory=ok?open("/dev/shm",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC):-1,readonly=-1;
    int writer=directory>=0?pd_upload_memory_file(directory,&readonly):-1;if(directory>=0){close(directory);}
    ok=writer>=0;
    for(size_t done=0;ok && done<sizeof(bytes);){
        ssize_t n=write(writer,bytes+done,sizeof(bytes)-done);if(n<0 && errno==EINTR){continue;}
        if(n<=0){ok=0;break;}done+=(size_t)n;
    }
    if(writer>=0){close(writer);}crypto_wipe(key,sizeof(key));crypto_wipe(receipt,sizeof(receipt));crypto_wipe(bytes,sizeof(bytes));
    if(!ok){if(readonly>=0){close(readonly);}return -1;}return readonly;
}
int pd_runtime_config_fetch(int input,unsigned role)
{
#ifdef PD_RUNTIME_CONFIG_TEST
    uid_t issuer=getuid();
#else
    if(getuid()!=0 || geteuid()!=0){return -1;}uid_t issuer=78666;
#endif
    unsigned char public_bytes[120];
    if((role!=PD_CONFIG_ACCEPT && role!=PD_CONFIG_DELIVER) || !public_record(input,public_bytes)){return -1;}
    struct sockaddr_un address={0};address.sun_family=AF_UNIX;
    const char prefix[]="aetherpd-config-",hex[]="0123456789abcdef";
    memcpy(address.sun_path+1,prefix,sizeof(prefix)-1);
    for(unsigned i=0;i<32;++i){address.sun_path[sizeof(prefix)+i*2]=hex[public_bytes[88+i]>>4];address.sun_path[sizeof(prefix)+i*2+1]=hex[public_bytes[88+i]&15];}
    socklen_t length=(socklen_t)(offsetof(struct sockaddr_un,sun_path)+sizeof(prefix)+64);
    int channel=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC|SOCK_NONBLOCK,0);if(channel<0){return -1;}
    struct ucred peer;socklen_t peer_size=sizeof(peer);int incoming=-1,result=-1;
    struct timeval timeout={3,0};unsigned char request[128];
    memcpy(request,role==PD_CONFIG_ACCEPT?"PDCFGAC1":"PDCFGDL1",8);memcpy(request+8,public_bytes,120);
    int ok=!connect(channel,(struct sockaddr*)&address,length) &&
        !getsockopt(channel,SOL_SOCKET,SO_PEERCRED,&peer,&peer_size) && peer_size==sizeof(peer) && peer.uid==issuer &&
        !fcntl(channel,F_SETFL,fcntl(channel,F_GETFL)&~O_NONBLOCK) &&
        !setsockopt(channel,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout)) &&
        !setsockopt(channel,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout)) &&
        send(channel,request,sizeof(request),MSG_NOSIGNAL)==sizeof(request);
    if(ok){incoming=receive_file(channel);}
    if(incoming>=0){result=copy_config(incoming,issuer,public_bytes);close(incoming);}close(channel);return result;
}
