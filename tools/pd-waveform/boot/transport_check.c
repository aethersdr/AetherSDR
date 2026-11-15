/* Own-protocol socket fixture; never built into a production bootstrap. */
#define _GNU_SOURCE
#include "pd_transport.h"
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static unsigned descriptors(void)
{
    DIR* directory=opendir("/proc/self/fd");if(!directory){return 0;}
    unsigned count=0;struct dirent* entry;
    while((entry=readdir(directory))){if(entry->d_name[0]!='.'){++count;}}
    closedir(directory);return count;
}
int main(int argc,char** argv)
{
    if(argc!=5){return 2;}
    int config=atoi(argv[2]),socket=atoi(argv[3]);uid_t peer=(uid_t)strtoul(argv[4],NULL,10);
    if(!strcmp(argv[1],"config")){
        unsigned char key[32],receipt[80];unsigned before=descriptors();
        int ok=pd_transport_config(config,peer,key,receipt);unsigned after=descriptors();
        printf("{\"accepted\":%s,\"descriptors_before\":%u,\"descriptors_after\":%u}\n",ok?"true":"false",before,after);
        return before==after?(ok?0:3):4;
    }
    unsigned char bytes[112];if(pread(config,bytes,sizeof(bytes),0)!=(ssize_t)sizeof(bytes)){return 2;}
    int files[5]={-1,-1,-1,-1,-1},ok=0;unsigned before=descriptors();
    if(!strcmp(argv[1],"receive")){
        ok=pd_transport_receive(socket,peer,bytes,bytes+32,files);
        for(int i=0;i<5;++i){if(files[i]>=0){
            if(!(fcntl(files[i],F_GETFD)&FD_CLOEXEC)){return 4;}
            close(files[i]);
        }}
    }else if(!strcmp(argv[1],"send")){
        const char* list=getenv("PD_TEST_FDS");if(!list || sscanf(list,"%d,%d,%d,%d,%d",files,files+1,files+2,files+3,files+4)!=5){return 2;}
        ok=pd_transport_send(socket,peer,bytes,bytes+32,files);
    }else if(!strcmp(argv[1],"result-receive")){
        int result=-1;ok=pd_transport_result_receive(socket,&result);
    }else if(!strcmp(argv[1],"result-send")){
        ok=pd_transport_result_send(socket,(int)peer);
    }else{return 2;}
    unsigned after=descriptors();
    printf("{\"accepted\":%s,\"descriptors_before\":%u,\"descriptors_after\":%u}\n",ok?"true":"false",before,after);
    return before==after?(ok?0:3):4;
}
