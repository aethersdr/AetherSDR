#define _GNU_SOURCE
#include "pd_release.h"
#include "pd_bundle.h"
#include "pd_units.h"
#include "monocypher.h"
#include "monocypher-ed25519.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

const char* const pd_release_names[PD_RELEASE_COUNT]={"pd-boot","pd-update","payload.bundle",
    "BOOT-LICENSES.txt","aetherpd-boot.service","aetherpd-start.socket","aetherpd-start@.service","aetherpd-shim.service"};
static uint32_t release_be32(const unsigned char* p)
{
    return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];
}
static int release_read_at(int fd,unsigned char* data,size_t count,off_t offset)
{
    while(count){
        ssize_t n=pread(fd,data,count,offset);
        if(n<0 && errno==EINTR){continue;}if(n<=0){return 0;}
        data+=n;count-=(size_t)n;offset+=n;
    }
    return 1;
}
static int release_hash(int fd,uint32_t offset,uint32_t count,unsigned char digest[32])
{
    unsigned char data[16384];crypto_blake2b_ctx hash;crypto_blake2b_init(&hash,32);
    while(count){
        size_t n=count<sizeof(data)?count:sizeof(data);
        if(!release_read_at(fd,data,n,offset)){return 0;}
        crypto_blake2b_update(&hash,data,n);count-=(uint32_t)n;offset+=(uint32_t)n;
    }
    crypto_blake2b_final(&hash,digest);return 1;
}
static int release_unit(const unsigned char* data,uint32_t count,unsigned index,const struct pd_release* release)
{
    const char* const* definitions=release->arch==7?
        (release->managed_startup?pd_units_armv7_managed:pd_units_armv7):
        (release->managed_startup?pd_units_arm64_managed:pd_units_arm64);
    const char* expected=definitions[index];
    size_t length=strlen(expected);
    return count==length && !memcmp(data,expected,length);
}
static int release_elf(const unsigned char* data,uint32_t count,uint32_t arch)
{
    size_t minimum=arch==7?52:64;
    return count>=minimum && !memcmp(data,"\177ELF",4) &&
        data[4]==(arch==7?1:2) && data[5]==1 && data[6]==1 && data[16]==2 && !data[17] &&
        data[18]==(arch==7?40:183) && !data[19];
}
static int release_firmware(const unsigned char bytes[28],const char* expected)
{
    size_t size=strnlen(expected,28);if(!size || size>=28){return 0;}
    unsigned dots=0,digits=0;
    for(size_t i=0;i<size;++i){
        if(expected[i]>='0' && expected[i]<='9'){++digits;}
        else if(expected[i]=='.' && digits){++dots;digits=0;}
        else{return 0;}
    }
    if(dots!=3 || !digits || memcmp(bytes,expected,size)){return 0;}
    for(size_t i=size;i<28;++i){if(bytes[i]){return 0;}}return 1;
}
int pd_release_header_parse(const unsigned char* bytes,size_t size,const unsigned char key[32],
                            uint32_t arch,uint32_t profile,const char* firmware,struct pd_release* release)
{
    memset(release,0,sizeof(*release));
    if(size!=PD_RELEASE_PREFIX || !((arch==64 && profile==1) || (arch==7 && profile==2))){return 0;}
    memcpy(release->manifest,bytes,PD_RELEASE_MANIFEST);
    memcpy(release->signature,bytes+PD_RELEASE_MANIFEST,sizeof(release->signature));
    if(crypto_ed25519_check(release->signature,key,release->manifest,PD_RELEASE_MANIFEST) ||
        (memcmp(release->manifest,"AEPDR001",8) && memcmp(release->manifest,"AEPDR002",8) && memcmp(release->manifest,"AEPDR003",8) && memcmp(release->manifest,"AEPDR004",8) && memcmp(release->manifest,"AEPDR005",8)) || release_be32(release->manifest+8)!=arch ||
        release_be32(release->manifest+12)!=PD_RELEASE_COUNT || release_be32(release->manifest+48)!=profile ||
        !release_firmware(release->manifest+52,firmware)){return 0;}
    uint64_t offset=PD_RELEASE_PREFIX;release->arch=arch;release->profile=profile;
    release->boot_handoff=memcmp(release->manifest,"AEPDR001",8)!=0;
    release->generation_selection=!memcmp(release->manifest,"AEPDR005",8);
    release->origin_recovery=!memcmp(release->manifest,"AEPDR004",8) || release->generation_selection;
    release->managed_startup=!memcmp(release->manifest,"AEPDR003",8) || release->origin_recovery;
    for(unsigned i=0;i<PD_RELEASE_COUNT;++i){
        uint32_t count=release_be32(release->manifest+PD_RELEASE_HEADER+i*PD_RELEASE_RECORD);
        uint32_t limit=i==2?PD_BUNDLE_MAX:i>=4?16384:512*1024;
        if(!count || count>limit || offset+count>PD_RELEASE_MAX){return 0;}
        release->offset[i]=(uint32_t)offset;release->length[i]=count;offset+=count;
    }
    return 1;
}
int pd_release_record_validate(const unsigned char* bytes,size_t size,unsigned index,const struct pd_release* release)
{
    if(index>=PD_RELEASE_COUNT || size!=release->length[index]){return 0;}
    unsigned char digest[32];crypto_blake2b(digest,32,bytes,size);
    return !crypto_verify32(digest,release->manifest+PD_RELEASE_HEADER+index*PD_RELEASE_RECORD+4) &&
        (index>=2 || release_elf(bytes,(uint32_t)size,release->arch)) &&
        (index<4 || release_unit(bytes,(uint32_t)size,index-4,release));
}
int pd_release_read(int fd,const unsigned char key[32],uint32_t arch,uint32_t profile,
                    const char* firmware,const unsigned char approved_digest[32],struct pd_release* release)
{
    memset(release,0,sizeof(*release));struct stat st;unsigned char digest[32],header[PD_RELEASE_PREFIX];
    if(fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_size<PD_RELEASE_PREFIX || st.st_size>PD_RELEASE_MAX ||
        !release_read_at(fd,header,sizeof(header),0) ||
        !pd_release_header_parse(header,sizeof(header),key,arch,profile,firmware,release) ||
        (uint64_t)release->offset[PD_RELEASE_COUNT-1]+release->length[PD_RELEASE_COUNT-1]!=(uint64_t)st.st_size){return 0;}
    crypto_blake2b_ctx approved;crypto_blake2b_init(&approved,32);
    crypto_blake2b_update(&approved,release->manifest,PD_RELEASE_MANIFEST);
    crypto_blake2b_update(&approved,release->signature,sizeof(release->signature));
    for(unsigned i=0;i<PD_RELEASE_COUNT;++i){
        unsigned char* data=malloc(release->length[i]);if(!data){return 0;}
        int ok=release_read_at(fd,data,release->length[i],release->offset[i]);
        if(ok){
            ok=pd_release_record_validate(data,release->length[i],i,release);
            crypto_blake2b_update(&approved,data,release->length[i]);
        }
        free(data);if(!ok){return 0;}
    }
    crypto_blake2b_final(&approved,digest);return !crypto_verify32(digest,approved_digest);
}
static int release_copy(int source,int output,uint32_t offset,uint32_t size)
{
    unsigned char bytes[16384];
    while(size){
        size_t count=size<sizeof(bytes)?size:sizeof(bytes);
        if(!release_read_at(source,bytes,count,offset)){return 0;}
        size_t used=0;
        while(used<count){
            ssize_t n=write(output,bytes+used,count-used);
            if(n<0 && errno==EINTR){continue;}if(n<=0){return 0;}used+=(size_t)n;
        }
        size-=(uint32_t)count;offset+=(uint32_t)count;
    }
    return 1;
}
int pd_release_extract(int fd,int directory,const unsigned char key[32],const struct pd_release* release)
{
    for(unsigned i=0;i<PD_RELEASE_COUNT;++i){
        int output=openat(directory,pd_release_names[i],O_RDWR|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
        if(output<0){return 0;}
        unsigned char digest[32];
        int ok=release_copy(fd,output,release->offset[i],release->length[i]) &&
            release_hash(output,0,release->length[i],digest) &&
            !crypto_verify32(digest,release->manifest+PD_RELEASE_HEADER+i*PD_RELEASE_RECORD+4);
        if(ok && i==2){
            struct pd_bundle payload;ok=pd_bundle_read(output,key,release->arch,&payload) && pd_bundle_validate(output,&payload);
        }
        ok=ok && !fchmod(output,i<2?0755:0644) && !fsync(output);
        if(close(output)){ok=0;}if(!ok){return 0;}
    }
    return !fsync(directory);
}
