#define _GNU_SOURCE
#include "pd_installation.h"
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
static unsigned kill_at=0;
void pd_installation_checkpoint(unsigned point){if(point==kill_at){raise(SIGKILL);}}
int pd_transaction_checkpoint(const char* point,unsigned phase){(void)point;(void)phase;return 1;}
int pd_upgrade_checkpoint(const char* point,unsigned index){(void)point;(void)index;return 1;}
int main(int argc,char** argv)
{
    if(argc!=3 && argc!=4){return 2;}
    if(argc==4){kill_at=(unsigned)atoi(argv[3]);}
    int store=open(argv[1],O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    int runtime=open(argv[2],O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    int ok=store>=0 && runtime>=0 && pd_installation_publish(store,runtime);
    if(store>=0){close(store);}if(runtime>=0){close(runtime);}return ok?0:1;
}
