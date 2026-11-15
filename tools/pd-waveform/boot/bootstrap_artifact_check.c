/* File/parameter interface for component tests only; never packaged as a helper. */
#define _GNU_SOURCE
#include "pd_bootstrap_artifact.h"
#include "monocypher.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static int read_digest(const char* path,unsigned char digest[32])
{
    int fd=open(path,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    if(fd<0){return 0;}
    unsigned char extra;ssize_t size=read(fd,digest,32),tail=read(fd,&extra,1);
    close(fd);return size==32 && tail==0;
}
int main(int argc,char** argv)
{
    if(argc!=7){return 2;}
    unsigned char key[32],source[32],release[32];
    if(!read_digest(argv[2],key) || !read_digest(argv[5],source) || !read_digest(argv[6],release)){return 2;}
    int fd=open(argv[1],O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    if(fd<0){return 2;}
    struct pd_bootstrap_artifact artifact;
    int ok=pd_bootstrap_artifact_read(fd,key,(uint32_t)strtoul(argv[3],NULL,10),
        (uint32_t)strtoul(argv[4],NULL,10),source,release,&artifact);
    close(fd);
    if(ok){
        unsigned char digest[32];crypto_blake2b(digest,sizeof(digest),artifact.executable,artifact.size);
        printf("%zu ",artifact.size);
        for(size_t i=0;i<sizeof(digest);++i){printf("%02x",digest[i]);}
        putchar(' ');
        for(size_t i=0;i<sizeof(artifact.digest);++i){printf("%02x",artifact.digest[i]);}
        putchar('\n');
    }
    pd_bootstrap_artifact_close(&artifact);return ok?0:1;
}
