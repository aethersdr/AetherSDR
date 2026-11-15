#define _GNU_SOURCE
#include "pd_transaction.h"
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int pd_transaction_checkpoint(const char* point,unsigned phase){(void)point;(void)phase;return 1;}
int main(int argc,char** argv)
{
    if(argc!=2 && !(argc==3 && !strcmp(argv[2],"runtime"))){return 2;}
    int store=open(argv[1],O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    int lock=-1,ok=0;
    if(store>=0){
        if(argc==3){ok=pd_transaction_runtime_gate(store,&lock);}
        else{lock=pd_transaction_session_lock(store);ok=lock>=0;}
        close(store);
    }
    if(!ok){return 3;}
    if(lock<0){puts("activating");return 0;}
    if((fcntl(lock,F_GETFL)&O_ACCMODE)!=O_RDONLY || !(fcntl(lock,F_GETFD)&FD_CLOEXEC)){close(lock);return 4;}
    puts("admitted");fflush(stdout);char stop;
    ssize_t count=read(0,&stop,1);(void)count;close(lock);return 0;
}
