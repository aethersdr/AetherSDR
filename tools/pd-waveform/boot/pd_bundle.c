#define _GNU_SOURCE
#include "pd_bundle.h"
#include "monocypher.h"
#include "monocypher-ed25519.h"
#include "zlib.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

const char* const pd_bundle_names[PD_BUNDLE_COUNT]={
    "flex-tailnet-shim","aether-pd","pd-primary-launch","pd-start-session",
    "pd-radio-launch","pd-fixed-arp","pd-iq-source","pd-capture-bootstrap",
    "pd-unkey-guard","pd-lease-guard","LICENSES"};
static uint32_t be32(const unsigned char* p)
{
    return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];
}
static int read_at(int fd,void* buffer,size_t count,off_t offset)
{
    unsigned char* out=buffer;
    while(count){
        ssize_t n=pread(fd,out,count,offset);
        if(n<0 && errno==EINTR){continue;}if(n<=0){return 0;}
        count-=(size_t)n;offset+=n;out+=n;
    }
    return 1;
}
static int write_all(int fd,const unsigned char* buffer,size_t count)
{
    while(count){
        ssize_t n=write(fd,buffer,count);
        if(n<0 && errno==EINTR){continue;}if(n<=0){return 0;}
        count-=(size_t)n;buffer+=n;
    }
    return 1;
}
static int hash_section(int fd,off_t offset,uint32_t size,unsigned char result[32])
{
    unsigned char buffer[16384];crypto_blake2b_ctx hash;crypto_blake2b_init(&hash,32);
    while(size){
        size_t count=size<sizeof(buffer)?size:sizeof(buffer);
        if(!read_at(fd,buffer,count,offset)){return 0;}
        crypto_blake2b_update(&hash,buffer,count);size-=(uint32_t)count;offset+=(off_t)count;
    }
    crypto_blake2b_final(&hash,result);return 1;
}
int pd_bundle_hash(int fd,unsigned char digest[32])
{
    struct stat st;
    return !fstat(fd,&st) && S_ISREG(st.st_mode) && st.st_size>=PD_BUNDLE_PREFIX &&
        st.st_size<=PD_BUNDLE_MAX && hash_section(fd,0,(uint32_t)st.st_size,digest);
}
int pd_bundle_read(int fd,const unsigned char key[32],uint32_t arch,struct pd_bundle* bundle)
{
    struct stat st;unsigned char signature[64];
    memset(bundle,0,sizeof(*bundle));
    if((arch!=7 && arch!=64) || fstat(fd,&st) || !S_ISREG(st.st_mode) ||
        st.st_size<PD_BUNDLE_PREFIX || st.st_size>PD_BUNDLE_MAX ||
        !read_at(fd,bundle->manifest,PD_BUNDLE_MANIFEST,0) ||
        !read_at(fd,signature,64,PD_BUNDLE_MANIFEST) ||
        crypto_ed25519_check(signature,key,bundle->manifest,PD_BUNDLE_MANIFEST) ||
        memcmp(bundle->manifest,"AEPDB001",8) || be32(bundle->manifest+8)!=arch ||
        be32(bundle->manifest+12)!=PD_BUNDLE_COUNT){return 0;}
    uint64_t offset=PD_BUNDLE_PREFIX,total=0;bundle->arch=arch;
    for(unsigned i=0;i<PD_BUNDLE_COUNT;++i){
        const unsigned char* record=bundle->manifest+PD_BUNDLE_HEADER+i*PD_BUNDLE_RECORD;
        uint32_t compressed=be32(record),expanded=be32(record+4);
        if(compressed<18 || compressed>PD_BUNDLE_MAX || expanded<1 || expanded>32*1024*1024 ||
            (i==PD_BUNDLE_COUNT-1 && expanded>4*1024*1024)){return 0;}
        bundle->offset[i]=(uint32_t)offset;bundle->compressed[i]=compressed;bundle->expanded[i]=expanded;
        offset+=compressed;total+=expanded;
        if(offset>(uint64_t)st.st_size || total>PD_BUNDLE_EXPANDED_MAX){return 0;}
    }
    if(offset!=(uint64_t)st.st_size){return 0;}
    /* Authenticate every compressed record before any decompressor or output
     * file sees it. Extraction rechecks its private input copy against this. */
    for(unsigned i=0;i<PD_BUNDLE_COUNT;++i){
        unsigned char digest[32];
        if(!hash_section(fd,bundle->offset[i],bundle->compressed[i],digest) ||
            crypto_verify32(digest,bundle->manifest+PD_BUNDLE_HEADER+i*PD_BUNDLE_RECORD+8)){return 0;}
    }
    return 1;
}
static int elf_header(const unsigned char* p,size_t size,uint32_t arch)
{
    unsigned machine=arch==7?40:183;
    return size>=(arch==7?52u:64u) && !memcmp(p,"\177ELF",4) &&
        p[4]==(arch==7?1:2) && p[5]==1 && p[6]==1 &&
        p[16]==2 && p[17]==0 && p[18]==machine && p[19]==0;
}
static int inflate_record(int fd,int output,const struct pd_bundle* bundle,unsigned index)
{
    uint32_t packed=bundle->compressed[index],expected=bundle->expanded[index];
    const unsigned char* record=bundle->manifest+PD_BUNDLE_HEADER+index*PD_BUNDLE_RECORD;
    unsigned char* input=malloc(packed);if(!input){return 0;}
    unsigned char digest[32];int ok=read_at(fd,input,packed,bundle->offset[index]);
    if(ok){crypto_blake2b(digest,32,input,packed);ok=!crypto_verify32(digest,record+8);}
    z_stream stream={0};
    if(!ok || inflateInit2(&stream,31)!=Z_OK){free(input);return 0;}
    stream.next_in=input;stream.avail_in=packed;uint64_t total=0;
    crypto_blake2b_ctx hash;crypto_blake2b_init(&hash,32);int result=Z_OK;
    unsigned char buffer[16384];int first=1;
    while(ok && result==Z_OK){
        stream.next_out=buffer;stream.avail_out=sizeof(buffer);
        uInt before=stream.avail_in;result=inflate(&stream,Z_NO_FLUSH);
        size_t count=sizeof(buffer)-stream.avail_out;total+=count;
        if((result!=Z_OK && result!=Z_STREAM_END) || total>expected ||
            (!count && stream.avail_in==before)){ok=0;break;}
        if(first && index<PD_BUNDLE_COUNT-1){ok=elf_header(buffer,count,bundle->arch);}
        first=0;
        if(ok){crypto_blake2b_update(&hash,buffer,count);ok=output<0 || write_all(output,buffer,count);}
    }
    ok=ok && result==Z_STREAM_END && !stream.avail_in && total==expected;
    crypto_blake2b_final(&hash,digest);ok=ok && !crypto_verify32(digest,record+40);
    inflateEnd(&stream);free(input);return ok;
}
int pd_bundle_validate(int fd,const struct pd_bundle* bundle)
{
    for(unsigned i=0;i<PD_BUNDLE_COUNT;++i){
        if(!inflate_record(fd,-1,bundle,i)){return 0;}
    }
    return 1;
}
int pd_bundle_extract(int fd,int directory,const struct pd_bundle* bundle)
{
    for(unsigned i=0;i<PD_BUNDLE_COUNT;++i){
        int out=openat(directory,pd_bundle_names[i],O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
        if(out<0){return 0;}
        int ok=inflate_record(fd,out,bundle,i) && fchmod(out,i==PD_BUNDLE_COUNT-1?0644:0755)==0 && fsync(out)==0;
        if(close(out)){ok=0;}if(!ok){return 0;}
    }
    return !fsync(directory);
}
int pd_bundle_matches(int directory,const struct pd_bundle* bundle)
{
    for(unsigned i=0;i<PD_BUNDLE_COUNT;++i){
        int fd=openat(directory,pd_bundle_names[i],O_RDONLY|O_NOFOLLOW|O_CLOEXEC);if(fd<0){return 0;}
        struct stat st;unsigned char digest[32];
        int ok=!fstat(fd,&st) && S_ISREG(st.st_mode) && st.st_uid==0 && st.st_nlink==1 &&
            (st.st_mode&07777)==(i==PD_BUNDLE_COUNT-1?0644:0755) && st.st_size==bundle->expanded[i] &&
            hash_section(fd,0,bundle->expanded[i],digest) &&
            !crypto_verify32(digest,bundle->manifest+PD_BUNDLE_HEADER+i*PD_BUNDLE_RECORD+40);
        close(fd);if(!ok){return 0;}
    }
    return 1;
}
