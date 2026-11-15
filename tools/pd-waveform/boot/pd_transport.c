#define _GNU_SOURCE
#include "pd_transport.h"
#include "pd_bootstrap_artifact.h"
#include "pd_release.h"
#include "pd_migration.h"
#include "pd_upgrade.h"
#include "monocypher.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/vfs.h>
#include <unistd.h>

static unsigned be32(const unsigned char* p)
{
    return (unsigned)p[0]<<24|(unsigned)p[1]<<16|(unsigned)p[2]<<8|p[3];
}
static int count(const unsigned char receipt[80])
{
    unsigned arch=be32(receipt+8),role=be32(receipt+12);
    if(memcmp(receipt,"PDDELIV1",8) || (arch!=64 && arch!=7) ||
       (role!=1 && role!=2) || (role==1 && arch!=64)){return 0;}
    return role==1?4:5;
}
int pd_transport_config(int fd,uid_t owner,unsigned char key[32],unsigned char receipt[80])
{
    struct stat st;unsigned char bytes[120];int flags=fcntl(fd,F_GETFL);
    int ok=flags>=0 && (flags&O_ACCMODE)==O_RDONLY && !fstat(fd,&st) && S_ISREG(st.st_mode) &&
        st.st_uid==owner && st.st_nlink<=1 && (st.st_mode&07777)==0600 && st.st_size==sizeof(bytes) &&
        pread(fd,bytes,sizeof(bytes),0)==(ssize_t)sizeof(bytes) && !memcmp(bytes,"PDXFER01",8) && count(bytes+40);
    unsigned nonzero=0;if(ok){for(unsigned i=8;i<40;++i){nonzero|=bytes[i];}ok=nonzero!=0;}
    if(ok){memcpy(key,bytes+8,32);memcpy(receipt,bytes+40,80);}
    crypto_wipe(bytes,sizeof(bytes));return ok;
}
static int peer(int fd,uid_t expected)
{
    struct sockaddr_un address;socklen_t length=sizeof(address);int type=0;
    if(getsockname(fd,(struct sockaddr*)&address,&length) || address.sun_family!=AF_UNIX){return 0;}
    length=sizeof(type);if(getsockopt(fd,SOL_SOCKET,SO_TYPE,&type,&length) || type!=SOCK_SEQPACKET){return 0;}
    struct ucred credentials;length=sizeof(credentials);
    if(getsockopt(fd,SOL_SOCKET,SO_PEERCRED,&credentials,&length) || length!=sizeof(credentials) || credentials.uid!=expected){return 0;}
    int flags=fcntl(fd,F_GETFL);
    if(flags<0 || fcntl(fd,F_SETFL,flags&~O_NONBLOCK)){return 0;}
    struct timeval timeout={3,0};
    return !setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout)) &&
        !setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
}
static void authenticate(unsigned char mac[32],const unsigned char key[32],
                         const char domain[8],const unsigned char challenge[32],const unsigned char receipt[80])
{
    unsigned char message[120];memcpy(message,domain,8);memcpy(message+8,challenge,32);memcpy(message+40,receipt,80);
    crypto_blake2b_keyed(mac,32,key,32,message,sizeof(message));crypto_wipe(message,sizeof(message));
}
static int random_challenge(unsigned char challenge[32])
{
    int fd=open("/dev/urandom",O_RDONLY|O_CLOEXEC);if(fd<0){return 0;}size_t done=0;
    while(done<32){ssize_t n=read(fd,challenge+done,32-done);if(n<0 && errno==EINTR){continue;}
        if(n<=0){close(fd);return 0;}done+=(size_t)n;}
    close(fd);return 1;
}
static int shape(const int files[5],int n)
{
    struct stat st;struct statfs fs;
    for(int i=0;i<n;++i){
        int flags=fcntl(files[i],F_GETFL);
        if(flags<0 || (flags&O_ACCMODE)!=O_RDONLY || fstat(files[i],&st)){return 0;}
        if(!i){if(!S_ISDIR(st.st_mode) || st.st_uid!=0 || (st.st_mode&0022)){return 0;}continue;}
        if(!S_ISREG(st.st_mode) || fstatfs(files[i],&fs) || (unsigned long)fs.f_type!=0x01021994UL){return 0;}
        off_t minimum=i==1?PD_BOOTSTRAP_PREFIX:i==2?(n==4?PD_MIGRATION_SIZE:PD_UPGRADE_APPROVAL_SIZE):PD_RELEASE_PREFIX;
        off_t maximum=i==1?PD_BOOTSTRAP_PREFIX+PD_BOOTSTRAP_MAX:i==2?minimum:PD_RELEASE_MAX;
        if(st.st_size<minimum || st.st_size>maximum){return 0;}
    }
    return 1;
}
/* Even an invalid packet can install SCM_RIGHTS descriptors. Collect and close
 * them before reporting failure, including truncated or unexpected messages. */
static int packet(int fd,unsigned char* bytes,size_t size,int files[5],int expected)
{
    for(int i=0;i<5;++i){files[i]=-1;}
    union {struct cmsghdr align;unsigned char bytes[CMSG_SPACE(sizeof(int)*8)];} control={0};
    struct iovec iov={bytes,size};struct msghdr message={0};message.msg_iov=&iov;message.msg_iovlen=1;
    message.msg_control=control.bytes;message.msg_controllen=sizeof(control.bytes);
    ssize_t length=recvmsg(fd,&message,MSG_CMSG_CLOEXEC);int found=0,ok=length==(ssize_t)size && !(message.msg_flags&(MSG_TRUNC|MSG_CTRUNC));
    if(length<0){return 0;}
    size_t control_size=message.msg_controllen;
    if(control_size>sizeof(control.bytes)){ok=0;control_size=sizeof(control.bytes);}
    for(size_t cursor=0;cursor+sizeof(struct cmsghdr)<=control_size;){
        struct cmsghdr* c=(struct cmsghdr*)(control.bytes+cursor);
        if(c->cmsg_len<CMSG_LEN(0) || c->cmsg_len>control_size-cursor){ok=0;break;}
        size_t payload=c->cmsg_len-CMSG_LEN(0);cursor+=CMSG_SPACE(payload);
        if(c->cmsg_level!=SOL_SOCKET || c->cmsg_type!=SCM_RIGHTS){ok=0;continue;}
        if(payload%sizeof(int)){ok=0;}
        for(size_t offset=0;offset+sizeof(int)<=payload;offset+=sizeof(int)){
            int received;memcpy(&received,(unsigned char*)CMSG_DATA(c)+offset,sizeof(received));
            if(found<5){files[found]=received;}else{close(received);ok=0;}++found;
        }
    }
    ok=ok && found==expected;
    if(!ok){for(int i=0;i<5;++i){if(files[i]>=0){close(files[i]);files[i]=-1;}}}
    return ok;
}
int pd_transport_send(int fd,uid_t receiver,const unsigned char key[32],const unsigned char receipt[80],const int files[5])
{
    int n=count(receipt);if(!n || !peer(fd,receiver) || !shape(files,n)){return 0;}
    unsigned char hello[72],mac[32],reply[40];int extra[5];
    if(!packet(fd,hello,sizeof(hello),extra,0) || memcmp(hello,"PDCARR01",8)){return 0;}
    authenticate(mac,key,"PDSERV01",hello+8,receipt);
    if(crypto_verify32(mac,hello+40)){crypto_wipe(mac,sizeof(mac));return 0;}
    memcpy(reply,"PDROOT01",8);authenticate(reply+8,key,"PDSEND01",hello+8,receipt);
    union {struct cmsghdr align;unsigned char bytes[CMSG_SPACE(sizeof(int)*5)];} control={0};
    struct iovec iov={reply,sizeof(reply)};struct msghdr message={0};message.msg_iov=&iov;message.msg_iovlen=1;
    message.msg_control=control.bytes;message.msg_controllen=CMSG_SPACE(sizeof(int)*(size_t)n);
    struct cmsghdr* c=CMSG_FIRSTHDR(&message);c->cmsg_level=SOL_SOCKET;c->cmsg_type=SCM_RIGHTS;
    c->cmsg_len=CMSG_LEN(sizeof(int)*(size_t)n);memcpy(CMSG_DATA(c),files,sizeof(int)*(size_t)n);
    int ok=sendmsg(fd,&message,MSG_NOSIGNAL)==(ssize_t)sizeof(reply);
    crypto_wipe(reply,sizeof(reply));crypto_wipe(mac,sizeof(mac));return ok;
}
int pd_transport_receive(int fd,uid_t sender,const unsigned char key[32],const unsigned char receipt[80],int files[5])
{
    for(int i=0;i<5;++i){files[i]=-1;}
    int n=count(receipt);if(!n || !peer(fd,sender)){return 0;}
    unsigned char hello[72],reply[40],mac[32];memcpy(hello,"PDCARR01",8);
    if(!random_challenge(hello+8)){return 0;}authenticate(hello+40,key,"PDSERV01",hello+8,receipt);
    if(send(fd,hello,sizeof(hello),MSG_NOSIGNAL)!=(ssize_t)sizeof(hello) || !packet(fd,reply,sizeof(reply),files,n)){return 0;}
    authenticate(mac,key,"PDSEND01",hello+8,receipt);
    int ok=!memcmp(reply,"PDROOT01",8) && !crypto_verify32(mac,reply+8) && shape(files,n);
    crypto_wipe(mac,sizeof(mac));crypto_wipe(reply,sizeof(reply));
    if(!ok){for(int i=0;i<5;++i){if(files[i]>=0){close(files[i]);files[i]=-1;}}}
    return ok;
}
int pd_transport_result_send(int fd,int result)
{
    if(result!=0 && result!=3 && result!=4 && result!=5){return 0;}
    unsigned char bytes[12]="PDDONE01";bytes[11]=(unsigned char)result;
    return send(fd,bytes,sizeof(bytes),MSG_NOSIGNAL)==(ssize_t)sizeof(bytes);
}
int pd_transport_result_receive(int fd,int* result)
{
    int flags=fcntl(fd,F_GETFL);
    if(flags<0 || fcntl(fd,F_SETFL,flags&~O_NONBLOCK)){return 0;}
    struct timeval timeout={120,0};
    if(setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout))){return 0;}
    unsigned char bytes[12];int extra[5];
    if(!packet(fd,bytes,sizeof(bytes),extra,0) || memcmp(bytes,"PDDONE01",8)){return 0;}
    unsigned value=be32(bytes+8);if(value!=0 && value!=3 && value!=4 && value!=5){return 0;}
    *result=(int)value;return 1;
}
