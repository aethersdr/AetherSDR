#define _GNU_SOURCE
#include "pd_upload.h"
#include "monocypher.h"
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
static int descriptors(void)
{
    DIR* directory=opendir("/proc/self/fd");if(!directory){return -1;}
    int n=0;while(readdir(directory)){++n;}closedir(directory);return n;
}
int main(int argc,char** argv)
{
    if(argc!=4){return 2;}int config=atoi(argv[1]),channel=atoi(argv[2]),directory=atoi(argv[3]);
    unsigned char data[112];if(pread(config,data,sizeof(data),0)!=(ssize_t)sizeof(data)){return 2;}
    int before=descriptors(),files[4];unsigned char challenge[32];int ok=pd_upload_receive(channel,directory,data,data+32,files,challenge);
    printf("{\"accepted\":%s,\"files\":[",ok?"true":"false");unsigned count=0;
    for(unsigned i=0;i<4;++i){
        if(files[i]<0){continue;}struct stat st;int flags=fcntl(files[i],F_GETFL);
        if(fstat(files[i],&st) || st.st_nlink || (flags&O_ACCMODE)!=O_RDONLY){return 4;}
        unsigned char buffer[65536],hash[32];crypto_blake2b_ctx ctx;crypto_blake2b_init(&ctx,32);ssize_t n;
        while((n=read(files[i],buffer,sizeof(buffer)))>0){crypto_blake2b_update(&ctx,buffer,(size_t)n);}
        if(n<0){return 4;}crypto_blake2b_final(&ctx,hash);
        printf("%s{\"size\":%lld,\"hash\":\"",count++?",":"",(long long)st.st_size);
        for(unsigned j=0;j<32;++j){printf("%02x",hash[j]);}printf("\"}");close(files[i]);
    }
    printf("],\"descriptors_before\":%d,\"descriptors_after\":%d}\n",before,descriptors());
    return ok?0:3;
}
