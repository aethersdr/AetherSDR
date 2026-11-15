/* Host test adapter only. Paths and identity parameters are supplied by tests,
 * not accepted by the fixed production bootstrap interface. */
#define _GNU_SOURCE
#include "pd_release.h"
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
int main(int argc,char** argv)
{
    if(argc<7 || argc>9){return 2;}
    unsigned char key[33],approved[33];
    int keyfd=open(argv[2],O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    int digestfd=open(argv[6],O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    if(keyfd<0 || digestfd<0){return 2;}
    ssize_t kn=read(keyfd,key,sizeof(key)),dn=read(digestfd,approved,sizeof(approved));
    close(keyfd);close(digestfd);if(kn!=32 || dn!=32){return 2;}
    int input=open(argv[1],O_RDONLY|O_NOFOLLOW|O_CLOEXEC);if(input<0){return 2;}
    struct pd_release release;
    int ok=pd_release_read(input,key,(uint32_t)strtoul(argv[3],NULL,10),
        (uint32_t)strtoul(argv[4],NULL,10),argv[5],approved,&release);
    if(ok && argc==9){
        int replacement=open(argv[8],O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
        int changed=open(argv[1],O_WRONLY|O_TRUNC|O_NOFOLLOW|O_CLOEXEC);unsigned char data[4096];
        if(replacement<0 || changed<0){return 2;}
        ssize_t n;
        while((n=read(replacement,data,sizeof(data)))>0){if(write(changed,data,(size_t)n)!=n){return 2;}}
        close(replacement);close(changed);if(n<0){return 2;}
    }
    if(ok && argc>=8){
        int output=open(argv[7],O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        ok=output>=0 && pd_release_extract(input,output,key,&release);
        if(output>=0){close(output);}
    }
    close(input);return ok?0:1;
}
