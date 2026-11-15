#define _GNU_SOURCE
#include "pd_install.h"
#include "pd_approval.h"
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static const char* fault;
int pd_store_checkpoint(const char* point){(void)point;return 1;}
uint64_t pd_store_test_space(uint64_t actual){return actual;}
int pd_install_checkpoint(const char* point)
{
    if(fault && !strcmp(fault,point)){_exit(88);}return 1;
}
int main(int argc,char** argv)
{
    if(argc!=8 && argc!=9){return 2;}
    unsigned char key[33],digest[33];
    int keyfd=open(argv[2],O_RDONLY|O_NOFOLLOW|O_CLOEXEC),digestfd=open(argv[3],O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    if(keyfd<0 || digestfd<0){return 2;}
    ssize_t kn=read(keyfd,key,sizeof(key)),dn=read(digestfd,digest,sizeof(digest));close(keyfd);close(digestfd);
    if(kn!=32 || dn!=32){return 2;}
    int input=open(argv[1],O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    int parent=open(argv[4],O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC),units=open(argv[5],O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    int stage=open(argv[6],O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(input<0 || parent<0 || units<0 || (stage<0 && strcmp(argv[7],"discard"))){return 2;}
    struct pd_release release;
    int ok=pd_release_read(input,key,64,1,"4.3.16.42034",digest,&release);
    if(ok && !strcmp(argv[7],"extract")){ok=pd_release_extract(input,stage,key,&release);}
    fault=argc==9?argv[8]:NULL;
    if(ok && !strcmp(argv[7],"discard") && argc==9){
        unsigned char record[PD_APPROVAL_SIZE];int target=open(argv[8],O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
        ok=target>=0 && pread(target,record,sizeof(record),0)==sizeof(record) && pd_install_stage_discard(parent,&release,record);
        if(target>=0){close(target);}
    }else if(ok){ok=pd_install_prepare(parent,units,stage,key,&release,digest);}
    if(stage>=0){close(stage);}close(units);close(parent);close(input);return ok?0:1;
}
