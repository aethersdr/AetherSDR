/* Offline admission measurement. Synthetic statvfs values never touch a radio. */
#define _GNU_SOURCE
#define PD_UPGRADE_WORKER_TEST
#include "pd_upgrade_bootstrap.c"
#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/syscall.h>

static uint64_t block_size, available_bytes, available_inodes;
static int separate_units, units_fd;
static unsigned reports;
int __real_fstat(int fd,struct stat* st);
int __wrap_fstat(int fd,struct stat* st)
{
    int result=__real_fstat(fd,st);
    if(!result && separate_units && fd==units_fd){st->st_dev^=(dev_t)0x40000000;}
    return result;
}
int __wrap_fstatvfs(int fd,struct statvfs* st)
{
    (void)fd;memset(st,0,sizeof(*st));st->f_frsize=st->f_bsize=block_size;
    st->f_bavail=UINT64_C(1024)*1024*1024/block_size;
    st->f_files=st->f_favail=100000;return 0;
}
uint64_t pd_upgrade_test_free(uint64_t actual,int inodes)
{(void)actual;return inodes?available_inodes:available_bytes;}
void pd_upgrade_test_budget(uint64_t bytes,uint64_t inodes)
{
    printf("%s{\"required_bytes\":%" PRIu64 ",\"required_inodes\":%" PRIu64 "}",reports++?",":"",bytes,inodes);
}
int main(int argc,char** argv)
{
    if(argc!=14){return 2;}
    int artifact=open(argv[1],O_RDONLY),keyfd=open(argv[2],O_RDONLY),digestfd=open(argv[3],O_RDONLY);
    unsigned char key[32],digest[32];struct pd_upgrade approved={0};
    uint32_t arch=(uint32_t)strtoul(argv[4],NULL,10);
    if(artifact<0 || keyfd<0 || digestfd<0 || read(keyfd,key,32)!=32 || read(digestfd,digest,32)!=32 ||
        !pd_release_read(artifact,key,arch,arch==7?2:1,argv[5],digest,&approved.candidate_release)){return 3;}
    close(artifact);close(keyfd);close(digestfd);
    artifact=open(argv[11],O_RDONLY);keyfd=open(argv[12],O_RDONLY);digestfd=open(argv[13],O_RDONLY);
    if(artifact<0 || keyfd<0 || digestfd<0 || read(keyfd,key,32)!=32 || read(digestfd,digest,32)!=32 ||
        !pd_release_read(artifact,key,arch,arch==7?2:1,argv[5],digest,&approved.previous_release)){return 3;}
    close(artifact);close(keyfd);close(digestfd);
    int parent=open(argv[6],O_RDONLY|O_DIRECTORY),stage=open(argv[6],O_RDONLY|O_DIRECTORY);
    units_fd=open(argv[6],O_RDONLY|O_DIRECTORY);
    if(parent<0 || stage<0 || units_fd<0){return 4;}
    block_size=strtoull(argv[7],NULL,10);available_bytes=strtoull(argv[8],NULL,10);
    separate_units=atoi(argv[9]);available_inodes=strtoull(argv[10],NULL,10);
    if(block_size!=1024 && block_size!=4096){return 5;}
    printf("{\"volumes\":[");
    int accepted=space(parent,units_fd,stage,&approved,PD_UPGRADE_WORKER_MAX,arch);
    printf("],\"accepted\":%s}\n",accepted?"true":"false");
    close(parent);close(stage);close(units_fd);return accepted?0:1;
}
