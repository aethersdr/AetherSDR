#define _GNU_SOURCE
#include "pd_bundle.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
/* Test-only descriptor adapter; not part of the radio boot/install payload. */
int main(int argc,char** argv)
{
    if(argc!=4 && argc!=5){return 2;}
    unsigned char key[32];FILE* input=fopen(argv[2],"rb");if(!input){return 2;}
    size_t n=fread(key,1,32,input);int extra=fgetc(input);fclose(input);
    if(n!=32 || extra!=EOF){return 2;}
    uint32_t arch=(uint32_t)strtoul(argv[3],NULL,10);
    int fd=open(argv[1],O_RDONLY|O_NOFOLLOW|O_CLOEXEC);if(fd<0){return 2;}
    struct pd_bundle bundle;int ok=pd_bundle_read(fd,key,arch,&bundle);
    if(ok && argc==5){
        int out=open(argv[4],O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        ok=out>=0 && pd_bundle_extract(fd,out,&bundle);if(out>=0){close(out);}
    }
    close(fd);return ok?0:1;
}
