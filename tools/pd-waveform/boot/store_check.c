/* Host-only transaction adapter. Arbitrary fixture paths and fault controls
 * are deliberately absent from the shipped pd-update entry point. */
#define _GNU_SOURCE
#include "pd_store.h"
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static const char* fault;
static int test_store=-1;
static uint64_t space_limit=UINT64_MAX;
int pd_store_checkpoint(const char* point)
{
    if(fault && !strncmp(fault,"close:",6) && !strcmp(fault+6,point)){
        if(test_store>=0){close(test_store);test_store=-1;}return 1;
    }
    if(fault && !strncmp(fault,"crash:",6) && !strcmp(fault+6,point)){_exit(87);}
    return !fault || strncmp(fault,"fail:",5) || strcmp(fault+5,point);
}
uint64_t pd_store_test_space(uint64_t actual){return space_limit<actual?space_limit:actual;}
int main(int argc,char** argv)
{
    if(argc<6 || argc>8){return 2;}
    int store=open(argv[1],O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    test_store=store;
    int keyfd=open(argv[2],O_RDONLY|O_NOFOLLOW|O_CLOEXEC);unsigned char key[33];
    if(store<0 || keyfd<0){return 2;}
    ssize_t count=read(keyfd,key,sizeof(key));close(keyfd);if(count!=32){close(store);return 2;}
    uint32_t arch=(uint32_t)strtoul(argv[3],NULL,10);fault=argc>6?argv[6]:NULL;
    if(argc>7){space_limit=strtoull(argv[7],NULL,10);}
    int lock=pd_store_lock(store);if(lock<0){close(store);return 1;}
    int ok=0;
    if(!strcmp(argv[4],"update")){
        int input=open(argv[5],O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
        if(input>=0){ok=pd_store_update(store,input,key,arch);close(input);}
    }else if(!strcmp(argv[4],"rollback")){ok=pd_store_rollback(store,key,arch);}
    else if(!strcmp(argv[4],"read")){
        struct pd_selection selected;ok=pd_selection_read(store,&selected)==1;
        if(ok){printf("%c %c\n",selected.current,selected.previous?selected.previous:'-');}
    }
    close(lock);close(store);return ok?0:1;
}
