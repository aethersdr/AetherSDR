#define _GNU_SOURCE
#include "pd_bootstrap_artifact.h"
#include "monocypher.h"
#include "monocypher-ed25519.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static uint32_t artifact_be32(const unsigned char* p)
{
    return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 | (uint32_t)p[2]<<8 | p[3];
}
static int artifact_read_at(int fd,unsigned char* data,size_t size,off_t offset)
{
    while(size){
        ssize_t n=pread(fd,data,size,offset);
        if(n<0 && errno==EINTR){continue;}
        if(n<=0){return 0;}
        data+=n;size-=(size_t)n;offset+=n;
    }
    return 1;
}
void pd_bootstrap_artifact_close(struct pd_bootstrap_artifact* artifact)
{
    free(artifact->executable);memset(artifact,0,sizeof(*artifact));
}
int pd_bootstrap_artifact_read(int fd,const unsigned char key[32],uint32_t arch,uint32_t role,
    const unsigned char source[32],const unsigned char release[32],struct pd_bootstrap_artifact* artifact)
{
    memset(artifact,0,sizeof(*artifact));
    if((arch!=7 && arch!=64) || (role!=PD_BOOTSTRAP_INSTALL && role!=PD_BOOTSTRAP_UPGRADE) ||
       (role==PD_BOOTSTRAP_INSTALL && arch!=64)){return 0;}
    struct stat before,after;unsigned char header[PD_BOOTSTRAP_PREFIX];
    if(fstat(fd,&before) || !S_ISREG(before.st_mode) || before.st_size<PD_BOOTSTRAP_PREFIX ||
       before.st_size>PD_BOOTSTRAP_PREFIX+PD_BOOTSTRAP_MAX ||
       !artifact_read_at(fd,header,sizeof(header),0)){return 0;}
    uint32_t size=artifact_be32(header+16);
    if(memcmp(header,"AEPDX001",8) || artifact_be32(header+8)!=arch ||
       artifact_be32(header+12)!=role || artifact_be32(header+20)!=0 ||
       !size || size>PD_BOOTSTRAP_MAX || (uint64_t)size+PD_BOOTSTRAP_PREFIX!=(uint64_t)before.st_size ||
       crypto_verify32(header+24,source) || crypto_verify32(header+56,release) ||
       crypto_ed25519_check(header+PD_BOOTSTRAP_HEADER,key,header,PD_BOOTSTRAP_HEADER)){return 0;}
    unsigned char* executable=malloc(size);if(!executable){return 0;}
    unsigned char digest[32];
    int ok=artifact_read_at(fd,executable,size,PD_BOOTSTRAP_PREFIX);
    if(ok){
        crypto_blake2b(digest,sizeof(digest),executable,size);
        ok=!crypto_verify32(digest,header+88) && size>=(arch==7?52U:64U) &&
           !memcmp(executable,"\177ELF",4) && executable[4]==(arch==7?1:2) &&
           executable[5]==1 && executable[6]==1 && executable[16]==2 && !executable[17] &&
           executable[18]==(arch==7?40:183) && !executable[19] &&
           !fstat(fd,&after) && after.st_dev==before.st_dev && after.st_ino==before.st_ino &&
           after.st_size==before.st_size;
    }
    if(!ok){free(executable);return 0;}
    crypto_blake2b_ctx envelope;crypto_blake2b_init(&envelope,32);
    crypto_blake2b_update(&envelope,header,sizeof(header));
    crypto_blake2b_update(&envelope,executable,size);
    crypto_blake2b_final(&envelope,artifact->digest);
    artifact->executable=executable;artifact->size=size;return 1;
}
