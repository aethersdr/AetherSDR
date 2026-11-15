#define _GNU_SOURCE
#include "pd_upgrade_units.h"
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
static const char* fault;
int pd_worker_units_checkpoint(const char* point)
{
    if(!strncmp(fault,"kill:",5) && !strcmp(fault+5,point)){raise(SIGKILL);_exit(99);}
    return strncmp(fault,"fail:",5) || strcmp(fault+5,point);
}
int pd_transaction_checkpoint(const char* point,unsigned phase){(void)point;(void)phase;return 1;}
int main(int argc,char** argv)
{
    if(argc!=6){return 2;}fault=argv[5];
    int units=open(argv[2],O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    int store=open(argv[3],O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    int input=open(argv[4],O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);unsigned char plan[PD_UPGRADE_APPROVAL_SIZE];
    int ok=input>=0 && pread(input,plan,sizeof(plan),0)==sizeof(plan);
    if(input>=0){close(input);}
    if(ok){
        if(!strcmp(argv[1],"arm")){ok=pd_upgrade_units_arm(units);}
        else if(!strcmp(argv[1],"verify")){ok=pd_upgrade_units_verify(units);}
        else if(!strcmp(argv[1],"disarm")){ok=pd_upgrade_units_disarm(units,store,plan);}
        else if(!strcmp(argv[1],"finish") || !strcmp(argv[1],"reconcile")){
            struct pd_upgrade approved;int parent=openat(store,"..",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
            ok=parent>=0 && pd_upgrade_parse(plan,sizeof(plan),&approved) &&
                (!strcmp(argv[1],"finish")?pd_upgrade_finish_cleanup(parent,units,store,approved.candidate_key):
                 pd_upgrade_reconcile_terminal(parent,units,store,approved.candidate_key));
            if(parent>=0){close(parent);}
        }
        else{ok=0;}
    }
    if(units>=0){close(units);}if(store>=0){close(store);}return ok?0:1;
}
