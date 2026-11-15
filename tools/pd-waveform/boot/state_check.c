#define _GNU_SOURCE
#include "pd_state_transfer.h"
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
static const char* fault;
static const char* provision_path;
int pd_store_checkpoint(const char* point){(void)point;return 1;}
uint64_t pd_store_test_space(uint64_t actual){return actual;}
int pd_state_checkpoint(const char* point)
{
    if(!fault){return 1;}
    if(!strcmp(fault,point)){_exit(88);}
    if(!strcmp(fault,"mutate-source") && !strcmp(point,"sources-captured")){
        int fd=open(provision_path,O_WRONLY|O_NOFOLLOW|O_CLOEXEC);
        if(fd<0){return 0;}int ok=pwrite(fd,"!",1,0)==1;close(fd);return ok;
    }
    return 1;
}
/* Test adapter only; never included in an installable release. Radio builds
 * retain production ownership checks, host builds map them to the test UID. */
int main(int argc,char** argv)
{
    int restore=argc>1 && !strcmp(argv[argc-1],"--restore");if(restore){--argc;}
    if(argc!=5 && argc!=6){return 2;}
    unsigned char approval[PD_APPROVAL_SIZE+1];
    int record=open(argv[2],O_RDONLY|O_NOFOLLOW|O_CLOEXEC);if(record<0){return 2;}
    ssize_t size=read(record,approval,sizeof(approval));close(record);if(size!=PD_APPROVAL_SIZE){return 2;}
    int store=open(argv[1],O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    int provision=open(argv[3],O_RDONLY|O_NONBLOCK|O_NOFOLLOW|O_CLOEXEC);
    int tailscale=open(argv[4],O_RDONLY|O_NONBLOCK|O_NOFOLLOW|O_CLOEXEC);
    if(store<0){return 2;}
    provision_path=argv[3];fault=argc==6?argv[5]:NULL;
    int ok=restore?pd_state_restore_legacy(store,provision,tailscale,approval):pd_state_transfer(store,provision,tailscale,approval);
    if(provision>=0){close(provision);}if(tailscale>=0){close(tailscale);}close(store);
    return ok?0:1;
}
