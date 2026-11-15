#define _GNU_SOURCE
#include "pd_runtime_config.h"
#include "pd_transport.h"
#include "monocypher.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>
static int descriptors(void)
{
    DIR* directory=opendir("/proc/self/fd");if(!directory){return -1;}int count=0;
    while(readdir(directory)){++count;}closedir(directory);return count;
}
int main(int argc,char** argv)
{
    if(argc!=3){return 2;}
    int input=open(argv[1],O_RDONLY|O_NOFOLLOW|O_CLOEXEC);if(input<0){return 3;}
    unsigned role=!strcmp(argv[2],"accept")?1:!strcmp(argv[2],"deliver")?2:0;
    int before=descriptors(),config=pd_runtime_config_fetch(input,role),result=3;struct stat st;struct statfs fs;
    unsigned char key[32]={0},receipt[80]={0};
    if(config>=0){
        int valid=!fstat(config,&st) && !st.st_nlink && st.st_uid==geteuid() &&
            !fstatfs(config,&fs) && (unsigned long)fs.f_type==0x01021994UL &&
            pd_transport_config(config,geteuid(),key,receipt) && write(config,"x",1)<0 && errno==EBADF;
        close(config);result=valid?0:4;
    }
    crypto_wipe(key,sizeof(key));crypto_wipe(receipt,sizeof(receipt));
    int no_leak=before>=0 && descriptors()==before;close(input);
    printf("{\"credential_verified\":%s,\"no_descriptor_leak\":%s}\n",result==0?"true":"false",no_leak?"true":"false");
    return no_leak?result:5;
}
