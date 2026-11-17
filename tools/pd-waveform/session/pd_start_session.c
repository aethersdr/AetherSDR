#define PD_SERVICE_MAIN inherited_service_main
#include "pd_service_launch.c"
#undef PD_SERVICE_MAIN
#include "../boot/pd_transaction.h"
#ifdef PD_FIXED_RUNTIME_DIRECTORY
#include "../boot/pd_service.h"
#endif

/* Socket activation admits the fixed request before any network or scheduler
 * mutation. V2 adds only a bounded channel and explicit pan identity. */
enum { kStartLegacySize=48,kStartSelectedSize=56 };
static uint32_t start_u32(const unsigned char* bytes)
{
    return ((uint32_t)bytes[0]<<24)|((uint32_t)bytes[1]<<16)|((uint32_t)bytes[2]<<8)|bytes[3];
}
static int start_frame(const unsigned char* frame,size_t size,char id[37],uint32_t* gui,struct pd_iq_selection* selection)
{
    if(!frame || (size!=kStartLegacySize && size!=kStartSelectedSize)){return 0;}
    if(memcmp(frame,size==kStartLegacySize?"PDSTART1":"PDSTART2",8)){return 0;}
    memcpy(id,frame+8,36);id[36]=0;
    if(!service_uuid(id)){return 0;}
    *gui=start_u32(frame+44);
    *selection=(struct pd_iq_selection){1,0};
    if(size==kStartSelectedSize){
        *selection=(struct pd_iq_selection){start_u32(frame+48),start_u32(frame+52)};
        if(!selection->pan || !pd_iq_selection_valid(*selection)){return 0;}
    }
    return *gui!=0;
}
static int start_peer(int channel,int program)
{
    int type=0;socklen_t size=sizeof(type);struct sockaddr_un address;
    if(getsockopt(channel,SOL_SOCKET,SO_TYPE,&type,&size) || type!=SOCK_STREAM){return 0;}
    size=sizeof(address);
    if(getpeername(channel,(struct sockaddr*)&address,&size) || address.sun_family!=AF_UNIX){return 0;}
    struct ucred peer={0};size=sizeof(peer);
    if(getsockopt(channel,SOL_SOCKET,SO_PEERCRED,&peer,&size) || size!=sizeof(peer) || peer.uid!=78666 || peer.gid!=20 || peer.pid<=1){return 0;}
    char path[64];snprintf(path,sizeof(path),"/proc/%d/exe",peer.pid);
    int executable=open(path,O_RDONLY|O_CLOEXEC);
    struct stat expected,actual;int valid=executable>=0 && !fstat(program,&expected) && !fstat(executable,&actual) &&
        protected_program(&actual) && actual.st_dev==expected.st_dev && actual.st_ino==expected.st_ino;
    if(executable>=0){close(executable);}return valid;
}
static int start_read(int fd,unsigned char frame[kStartSelectedSize])
{
    int64_t begin=service_ms();size_t used=0,wanted=8;
    while(begin>=0 && used<wanted){
        int64_t now=service_ms();if(now<begin || now-begin>=2000){return 0;}
        struct pollfd p={fd,POLLIN,0};int ready=poll(&p,1,(int)(2000-(now-begin)));
        if(ready<0 && errno==EINTR){continue;}
        if(ready<=0 || p.revents&(POLLERR|POLLNVAL)){return 0;}
        ssize_t count=recv(fd,frame+used,wanted-used,MSG_DONTWAIT);
        if(count<0 && (errno==EINTR || errno==EAGAIN)){continue;}
        if(count<=0){return 0;}used+=(size_t)count;
        if(used==8){
            if(!memcmp(frame,"PDSTART1",8)){wanted=kStartLegacySize;}
            else if(!memcmp(frame,"PDSTART2",8)){wanted=kStartSelectedSize;}
            else{return 0;}
        }
    }
    return used==wanted?(int)used:0;
}
#ifdef PD_START_TEST
extern int pd_start_test_installation(void);
extern int pd_start_test_launch(const char* id,uint32_t gui,int control,int installation,struct pd_iq_selection selection);
#endif
static int start_session_launch(const char* id,uint32_t gui,struct pd_iq_selection selection)
{
    if(!pd_iq_selection_valid(selection)){return 5;}
#ifdef PD_FIXED_RUNTIME_DIRECTORY
    if(!pd_service_session_policy()){return 6;}
#endif
#ifdef PD_START_TEST
    int installation=pd_start_test_installation();
#else
    int installation=pd_transaction_session_open();
#endif
    if(installation<0){return 6;}
#ifdef PD_START_TEST
    int result=pd_start_test_launch(id,gui,0,installation,selection);
#else
    int result=service_launch(id,gui,0,installation,selection);
#endif
    close(installation);return result;
}
static int start_session_request(int channel)
{
#ifdef PD_FIXED_RUNTIME_DIRECTORY
    if(!pd_service_session_prepare()){return 6;}
#endif
    unsigned char frame[kStartSelectedSize];char id[37];uint32_t gui=0;struct pd_iq_selection selection;
    int size=start_read(channel,frame);
    if(!size || !start_frame(frame,(size_t)size,id,&gui,&selection)){return 5;}
    return start_session_launch(id,gui,selection);
}
#ifndef PD_START_MAIN
#define PD_START_MAIN main
#endif
int PD_START_MAIN(int argc,char** argv)
{
    (void)argv;
    if(argc!=1 || getuid()!=0 || geteuid()!=0){return 2;}
    char path[PATH_MAX];int directory=executable_directory(path);
    if(directory<0){return 3;}
    int shim=open_program(directory,kShim);close(directory);
    if(shim<0){return 3;}
    int admitted=start_peer(0,shim);close(shim);
    if(!admitted){return 4;}
    return start_session_request(0);
}
