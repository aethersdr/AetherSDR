#define _GNU_SOURCE
#include "pd_upgrade_state.h"
#include "pd_transaction.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char* faults;
static int root_fd,store_fd;
static int fault(const char* token)
{
    size_t size=strlen(token);const char* at=faults;
    while(at && *at){
        const char* end=strchr(at,',');size_t length=end?(size_t)(end-at):strlen(at);
        if(length==size && !memcmp(at,token,size)){return 1;}
        at=end?end+1:NULL;
    }
    return 0;
}
int pd_transaction_checkpoint(const char* point,unsigned phase)
{
    char token[128];snprintf(token,sizeof(token),"crash:%s:%u",point,phase);
    if(fault(token)){_exit(99);}
    snprintf(token,sizeof(token),"close:%s",point);
    if(fault(token)){close(store_fd);}
    return 1;
}
static void put(const char* name,const char* value)
{
    int fd=openat(root_fd,name,O_WRONLY|O_TRUNC|O_CREAT|O_CLOEXEC,0600);
    size_t size=strlen(value);
    if(fd<0 || write(fd,value,size)!=(ssize_t)size || close(fd)){_exit(80);}
}
static int action(const char* name,unsigned phase)
{
    struct pd_upgrade_state state;
    if(pd_upgrade_state_read(store_fd,&state)!=1 || state.phase!=phase){_exit(81);}
    int lease=-1,ok=pd_transaction_runtime_gate(store_fd,&lease);
    if(ok!=(phase==PD_UPGRADE_ACTIVATING || phase==PD_UPGRADE_RESTORING) || lease!=-1 ||
        pd_transaction_session_lock(store_fd)>=0){_exit(82);}
    int fd=openat(root_fd,"events",O_WRONLY|O_APPEND|O_CREAT|O_CLOEXEC,0600);
    if(fd<0 || dprintf(fd,"%s\n",name)<0 || close(fd)){_exit(83);}
    if(!strcmp(name,"quiesce")){put("running","0");}
    if(!strcmp(name,"select")){put("selected","1");}
    if(!strcmp(name,"activate")){put("running","1");put("identity","latest");}
    if(!strcmp(name,"rollback")){put("running","0");put("selected","0");}
    if(!strcmp(name,"restore")){put("running","1");}
    char token[128];snprintf(token,sizeof(token),"crash:%s",name);
    if(fault(token)){_exit(99);}
    snprintf(token,sizeof(token),"fail:%s",name);return !fault(token);
}
static int quiesce(void* ctx){(void)ctx;return action("quiesce",PD_UPGRADE_QUIESCING);}
static int select_release(void* ctx){(void)ctx;return action("select",PD_UPGRADE_SWITCHING);}
static int activate(void* ctx){(void)ctx;return action("activate",PD_UPGRADE_ACTIVATING);}
static int rollback(void* ctx){(void)ctx;return action("rollback",PD_UPGRADE_ROLLBACK);}
static int restore(void* ctx){(void)ctx;return action("restore",PD_UPGRADE_RESTORING);}
static int cleanup(void* ctx)
{
    (void)ctx;struct pd_upgrade_state state;
    if(pd_upgrade_state_read(store_fd,&state)!=1 ||
        (state.phase!=PD_UPGRADE_COMMITTED && state.phase!=PD_UPGRADE_ROLLED_BACK)){_exit(84);}
    int fd=openat(root_fd,"events",O_WRONLY|O_APPEND|O_CREAT|O_CLOEXEC,0600);
    if(fd<0 || dprintf(fd,"cleanup\n")<0 || close(fd)){_exit(83);}
    if(fault("crash:cleanup")){_exit(99);}
    if(fault("fail:cleanup")){return 0;}
    if(fault("retain:cleanup")){return 1;}
    /* Test effects are inert files. The production worker supplies fixed adapters. */
    return !unlinkat(root_fd,"aetherpd-upgrade",AT_REMOVEDIR) || errno==ENOENT;
}
int main(int argc,char** argv)
{
    if(argc!=5 || (strcmp(argv[3],"run") && strcmp(argv[3],"recover"))){return 2;}
    faults=argv[4];root_fd=open(argv[1],O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    store_fd=openat(root_fd,"store",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    int fd=open(argv[2],O_RDONLY|O_NOFOLLOW|O_CLOEXEC);unsigned char raw[PD_UPGRADE_APPROVAL_SIZE+1];
    if(root_fd<0 || store_fd<0 || fd<0 || read(fd,raw,sizeof(raw))!=PD_UPGRADE_APPROVAL_SIZE){return 2;}
    close(fd);struct pd_upgrade_ops ops={NULL,quiesce,select_release,activate,rollback,restore,cleanup};
    int result=pd_upgrade_transaction(store_fd,raw,&ops,!strcmp(argv[3],"recover"));
    close(store_fd);close(root_fd);printf("%d\n",result);
    return result==1?0:result==0?10:result==-1?20:30;
}
