/* Host test adapter; fixture paths and input mutation are not a shipped API. */
#define _GNU_SOURCE
#include "pd_upgrade.h"
#include "pd_pair.h"
#include "pd_bundle.h"
#include "pd_upgrade_origin.h"
#include <fcntl.h>
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
int pd_store_checkpoint(const char* point){(void)point;return 1;}
uint64_t pd_store_test_space(uint64_t actual){return actual;}
int pd_transaction_checkpoint(const char* point,unsigned phase){(void)point;(void)phase;return 1;}
static int test_stage=-1;
static int test_candidate=-1;
static int test_control=-1;
uint64_t pd_upgrade_test_free(uint64_t actual,int inodes)
{
    const char* value=getenv(inodes?"PD_UPGRADE_TEST_INODES":"PD_UPGRADE_TEST_SPACE");
    if(!value || !*value){return actual;}
    errno=0;char* end=NULL;unsigned long long result=strtoull(value,&end,10);
    return errno || !end || *end || *value=='-'?0:(uint64_t)result;
}
int pd_upgrade_checkpoint(const char* point,unsigned index)
{
    const char* fault=getenv("PD_UPGRADE_TEST_FAULT");char expected[96];
    snprintf(expected,sizeof(expected),"crash:%s:%u",point,index);
    if(fault && !strcmp(fault,expected)){raise(SIGKILL);}
    if(fault && !strcmp(fault,"hold:pair-reader") && !strcmp(point,"pair-reader-held")){
        puts("READY");fflush(stdout);char byte;return read(STDIN_FILENO,&byte,1)==1;
    }
    if(fault && !strcmp(fault,"close:pair-sync") &&
        (!strcmp(point,"pair-renamed") || !strcmp(point,"pair-retry"))){
        if(test_control>=0){close(test_control);test_control=-1;}
    }
    if(fault && !strcmp(fault,"replace-input-after-snapshot") && !strcmp(point,"resume-snapshots-verified")){
        return !ftruncate(test_candidate,0) && pwrite(test_candidate,"changed",7,0)==7;
    }
    if(fault && !strcmp(fault,"close:reopen-verified") && !strcmp(point,"reopen-verified")){
        if(test_stage>=0){close(test_stage);test_stage=-1;}
    }
    return 1;
}
int main(int argc,char** argv)
{
    if(argc!=9 && argc!=10){return 2;}
    int root=open(argv[1],O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    int store=openat(root,"store",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    int units=openat(root,"units",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    int previous=open(argv[2],O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    int candidate=open(argv[3],O_RDWR|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    test_candidate=candidate;
    int approved=open(argv[4],O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    int keyfd=open(argv[5],O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    unsigned char key[33];struct pd_identity identity;struct pd_upgrade upgrade;
    int ok=keyfd>=0 && read(keyfd,key,sizeof(key))==32 && pd_identity_parse(argv[6],&identity);
    int reopening=!strcmp(argv[7],"reopen");
    int pairing=!strcmp(argv[7],"select-previous") || !strcmp(argv[7],"select-candidate") || !strcmp(argv[7],"pair-read");
    if(ok && pairing){
        test_control=openat(root,"pair",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        int stage=open(argv[8],O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        if(!strcmp(argv[7],"pair-read")){
            struct pd_pair pair;unsigned char boot[32],payload[32];
            ok=pd_pair_acquire(test_control,stage,key,64,&identity,&pair);
            ok=ok && pd_upgrade_checkpoint("pair-reader-held",pair.choice) && pd_bundle_hash(pair.boot,boot) && pd_bundle_hash(pair.payload,payload);
            if(ok){
                printf("{\"choice\":%u,\"handoff\":%u,\"boot\":\"",pair.choice,pair.boot_handoff);
                for(unsigned i=0;i<32;++i){printf("%02x",boot[i]);}printf("\",\"payload\":\"");
                for(unsigned i=0;i<32;++i){printf("%02x",payload[i]);}printf("\",\"payload_key\":\"");
                for(unsigned i=0;i<32;++i){printf("%02x",pair.payload_key[i]);}puts("\"}");
            }
            pd_pair_close(&pair);
        }else{ok=pd_pair_publish(test_control,stage,key,64,&identity,!strcmp(argv[7],"select-candidate")?1:0);}
        if(stage>=0){close(stage);}if(test_control>=0){close(test_control);}test_control=-1;
        int descriptors[]={root,store,units,previous,candidate,approved,keyfd};
        for(unsigned i=0;i<sizeof(descriptors)/sizeof(descriptors[0]);++i){if(descriptors[i]>=0){close(descriptors[i]);}}
        return ok?0:1;
    }else if(ok && reopening){
        test_stage=open(argv[8],O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        ok=argc==9 && pd_upgrade_reopen(test_stage,key,64,&identity,&upgrade);
        if(test_stage>=0){close(test_stage);}test_stage=-1;
    }else if(ok && !strcmp(argv[7],"origin")){
        struct pd_upgrade_origin origin;
        ok=pd_upgrade_inspect_origin(store,units,approved,previous,candidate,key,64,&identity,&upgrade,&origin);
        if(ok){
            printf("{\"origin\":\"");
            for(unsigned i=0;i<sizeof(origin.record);++i){printf("%02x",origin.record[i]);}puts("\"}");
        }
    }else if(ok){ok=pd_upgrade_inspect(store,units,approved,previous,candidate,key,64,&identity,&upgrade);}
    if(ok && argc==10){
        int swap=open(argv[9],O_RDONLY|O_NOFOLLOW|O_CLOEXEC);unsigned char bytes[16384];ssize_t n;
        ok=swap>=0 && !ftruncate(candidate,0) && lseek(candidate,0,SEEK_SET)==0;
        while(ok && (n=read(swap,bytes,sizeof(bytes)))>0){ok=write(candidate,bytes,(size_t)n)==n;}
        if(swap>=0){close(swap);}
    }
    if(ok && !reopening && !strcmp(argv[7],"budget")){
        int stage=open(argv[8],O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);struct pd_upgrade_budget budget;
        ok=pd_upgrade_space(stage,&upgrade.previous_release,&upgrade.candidate_release,&budget);
        printf("{\"required_bytes\":%" PRIu64 ",\"available_bytes\":%" PRIu64 ",\"required_inodes\":%" PRIu64 ",\"available_inodes\":%" PRIu64 "}\n",
            budget.required_bytes,budget.available_bytes,budget.required_inodes,budget.available_inodes);
        if(stage>=0){close(stage);}
    }else if(ok && !reopening && !strcmp(argv[7],"resume")){
        test_stage=open(argv[8],O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        ok=pd_upgrade_resume(test_stage,previous,candidate,key,64,&identity,&upgrade);
        if(test_stage>=0){close(test_stage);}test_stage=-1;
    }else if(ok && !reopening && !strcmp(argv[7],"stage")){
        int stage=open(argv[8],O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        ok=pd_upgrade_stage(stage,previous,candidate,&upgrade);if(stage>=0){close(stage);}
    }else if(!reopening && strcmp(argv[7],"inspect") && strcmp(argv[7],"origin")){ok=0;}
    int fds[]={root,store,units,previous,candidate,approved,keyfd};
    for(unsigned i=0;i<sizeof(fds)/sizeof(fds[0]);++i){if(fds[i]>=0){close(fds[i]);}}
    return ok?0:1;
}
