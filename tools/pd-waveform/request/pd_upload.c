#define _GNU_SOURCE
#include "pd_upload.h"
#include "pd_bootstrap_artifact.h"
#include "pd_release.h"
#include "monocypher.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <time.h>
#include <unistd.h>
enum { kHeader=152,kBuffer=65536 };
static uint32_t number(const unsigned char* p)
{
    return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];
}
static int64_t now(void)
{
    struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t)){return -1;}
    return (int64_t)t.tv_sec*1000+t.tv_nsec/1000000;
}
static int transfer(int fd,unsigned char* bytes,size_t size,int sending,int64_t deadline)
{
    size_t done=0;
    while(done<size){
        int64_t current=now();if(current<0 || current>=deadline){return 0;}
        struct pollfd wait={fd,sending?POLLOUT:POLLIN,0};
        int ready=poll(&wait,1,(int)(deadline-current));
        if(ready<0 && errno==EINTR){continue;}if(ready!=1){return 0;}
        ssize_t n=sending?send(fd,bytes+done,size-done,MSG_NOSIGNAL|MSG_DONTWAIT):recv(fd,bytes+done,size-done,MSG_DONTWAIT);
        if(n<0 && (errno==EINTR || errno==EAGAIN)){continue;}if(n<=0){return 0;}done+=(size_t)n;
    }
    return 1;
}
static int random_bytes(unsigned char* bytes,size_t size)
{
    int fd=open("/dev/urandom",O_RDONLY|O_CLOEXEC);if(fd<0){return 0;}size_t done=0;
    while(done<size){ssize_t n=read(fd,bytes+done,size-done);if(n<0 && errno==EINTR){continue;}
        if(n<=0){close(fd);return 0;}done+=(size_t)n;}
    close(fd);return 1;
}
static void mac(unsigned char result[32],const unsigned char key[32],const char domain[8],
                const unsigned char challenge[32],const unsigned char receipt[80],const unsigned char* header,size_t size)
{
    crypto_blake2b_ctx ctx;crypto_blake2b_keyed_init(&ctx,32,key,32);
    crypto_blake2b_update(&ctx,(const unsigned char*)domain,8);
    crypto_blake2b_update(&ctx,challenge,32);crypto_blake2b_update(&ctx,receipt,80);
    if(header){crypto_blake2b_update(&ctx,header,size);}crypto_blake2b_final(&ctx,result);
}
static int shape(const unsigned char header[kHeader],const unsigned char receipt[80],unsigned* count)
{
    unsigned arch=number(receipt+8),role=number(receipt+12);
    if(memcmp(receipt,"PDDELIV1",8) || (arch!=64 && arch!=7) ||
       (role!=1 && role!=2) || (role==1 && arch!=64) || memcmp(header,"PDUPLD01",8)){return 0;}
    uint32_t sizes[4];for(unsigned i=0;i<4;++i){sizes[i]=number(header+8+4*i);}
    unsigned plan=role==1?308:352;
    if(sizes[0]<=PD_BOOTSTRAP_PREFIX || sizes[0]>PD_BOOTSTRAP_PREFIX+PD_BOOTSTRAP_MAX || sizes[1]!=plan ||
       sizes[2]<PD_RELEASE_PREFIX || sizes[2]>PD_RELEASE_MAX ||
       (role==2?(sizes[3]<PD_RELEASE_PREFIX || sizes[3]>PD_RELEASE_MAX):sizes[3]!=0) ||
       crypto_verify32(header+24,receipt+16) || crypto_verify32(header+56,receipt+48)){return 0;}
    if(role==1){unsigned nonzero=0;for(unsigned i=120;i<152;++i){nonzero|=header[i];}if(nonzero){return 0;}}
    *count=role==1?3:4;return 1;
}
/* Open both handles and unlink before receiving content. This works on Linux
 * 2.6 without memfd_create or O_TMPFILE. Death before unlink can leave an empty
 * tmpfs file, but no uploaded content is written until the name is gone. */
static int memory_file(int directory,int* readonly)
{
    unsigned char nonce[16];char name[49]=".aetherpd-upload-";
    if(!random_bytes(nonce,sizeof(nonce))){return -1;}const char hex[]="0123456789abcdef";
    for(unsigned i=0;i<16;++i){name[16+i*2]=hex[nonce[i]>>4];name[17+i*2]=hex[nonce[i]&15];}name[48]=0;
    int fd=openat(directory,name,O_CREAT|O_EXCL|O_NOFOLLOW|O_RDWR|O_CLOEXEC,0600);if(fd<0){return -1;}
    *readonly=openat(directory,name,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);struct stat a,b;
    int ok=*readonly>=0 && !fstat(fd,&a) && !fstat(*readonly,&b) && a.st_dev==b.st_dev && a.st_ino==b.st_ino &&
        S_ISREG(a.st_mode) && a.st_uid==geteuid() && a.st_nlink==1 && (a.st_mode&07777)==0600;
    int removed=unlinkat(directory,name,0)==0;
    if(!ok || !removed){if(*readonly>=0){close(*readonly);}*readonly=-1;close(fd);return -1;}return fd;
}
static int receive_file(int channel,int directory,uint32_t size,const unsigned char expected[32],int* readonly,int64_t deadline)
{
    int writer=memory_file(directory,readonly);if(writer<0){return 0;}
    unsigned char buffer[kBuffer],digest[32];crypto_blake2b_ctx hash;crypto_blake2b_init(&hash,32);int ok=1;
    for(uint32_t done=0;ok && done<size;){
        size_t amount=size-done;if(amount>sizeof(buffer)){amount=sizeof(buffer);}
        ok=transfer(channel,buffer,amount,0,deadline);
        size_t written=0;
        while(ok && written<amount){ssize_t n=write(writer,buffer+written,amount-written);
            if(n<0 && errno==EINTR){continue;}if(n<=0){ok=0;break;}written+=(size_t)n;}
        if(ok){crypto_blake2b_update(&hash,buffer,amount);done+=(uint32_t)amount;}
    }
    crypto_blake2b_final(&hash,digest);ok=ok && !crypto_verify32(digest,expected);close(writer);
    crypto_wipe(buffer,sizeof(buffer));crypto_wipe(digest,sizeof(digest));return ok;
}
static int ended(int channel,int64_t deadline)
{
    for(;;){
        int64_t current=now();if(current<0 || current>=deadline){return 0;}
        struct pollfd wait={channel,POLLIN,0};int ready=poll(&wait,1,(int)(deadline-current));
        if(ready<0 && errno==EINTR){continue;}if(ready!=1){return 0;}
        unsigned char byte;ssize_t n=recv(channel,&byte,1,MSG_DONTWAIT);
        if(n<0 && (errno==EINTR || errno==EAGAIN)){continue;}return n==0;
    }
}
int pd_upload_receive(int channel,int directory,const unsigned char key[32],const unsigned char receipt[80],int files[4],unsigned char challenge[32])
{
    memset(challenge,0,32);for(unsigned i=0;i<4;++i){files[i]=-1;}
    struct stat st;struct statfs fs;int type=0;socklen_t length=sizeof(type);unsigned nonzero=0;
    for(unsigned i=0;i<32;++i){nonzero|=key[i];}
    if(!nonzero || fstat(directory,&st) || !S_ISDIR(st.st_mode) || fstatfs(directory,&fs) ||
       (unsigned long)fs.f_type!=0x01021994UL || getsockopt(channel,SOL_SOCKET,SO_TYPE,&type,&length) || type!=SOCK_STREAM){return 0;}
    int64_t start=now();if(start<0){return 0;}
#ifdef PD_UPLOAD_TEST
    int64_t deadline=start+1000;
#else
    int64_t deadline=start+90000;
#endif
    unsigned char hello[72],header[kHeader]={0},reply[32]={0},expected[32];unsigned count=0;
    memcpy(hello,"PDUPCH01",8);if(!random_bytes(hello+8,32)){return 0;}
    mac(hello+40,key,"PDUSRV01",hello+8,receipt,NULL,0);
    int ok=transfer(channel,hello,sizeof(hello),1,deadline) && transfer(channel,header,sizeof(header),0,deadline) &&
        transfer(channel,reply,sizeof(reply),0,deadline) && shape(header,receipt,&count);
    mac(expected,key,"PDUCLI01",hello+8,receipt,header,kHeader);
    ok=ok && !crypto_verify32(expected,reply);
    for(unsigned i=0;ok && i<count;++i){ok=receive_file(channel,directory,number(header+8+4*i),header+24+32*i,&files[i],deadline);}
    ok=ok && ended(channel,deadline);
    if(ok){memcpy(challenge,hello+8,32);}
    crypto_wipe(hello,sizeof(hello));crypto_wipe(reply,sizeof(reply));crypto_wipe(expected,sizeof(expected));
    if(!ok){for(unsigned i=0;i<4;++i){if(files[i]>=0){close(files[i]);files[i]=-1;}}}return ok;
}

int pd_upload_reply(int channel,const unsigned char key[32],const unsigned char receipt[80],
                    const unsigned char challenge[32],unsigned stage,unsigned result)
{
    if((stage!=1 && stage!=2) || (stage==1 && result) ||
       (result!=0 && result!=3 && result!=4 && result!=5 && result!=6)){return 0;}
    unsigned char bytes[48]={0};memcpy(bytes,"PDUPACK1",8);bytes[11]=(unsigned char)stage;bytes[15]=(unsigned char)result;
    mac(bytes+16,key,"PDUACK01",challenge,receipt,bytes,16);
    int64_t current=now();int ok=current>=0 && transfer(channel,bytes,sizeof(bytes),1,current+3000);
    crypto_wipe(bytes,sizeof(bytes));return ok;
}
