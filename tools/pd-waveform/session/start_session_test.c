#define PD_START_TEST
#define PD_START_MAIN start_program_main
#include "pd_start_session.c"
#undef PD_START_MAIN
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
static int installation=-1,launches;
int pd_start_test_installation(void){return installation;}
int pd_start_test_launch(const char* id,uint32_t gui,int control,int lease)
{
    assert(service_uuid(id) && gui==1 && control==0 && lease==installation && lease>=0);
    assert(fcntl(lease,F_GETFD)>=0);++launches;return 42;
}
int main(void)
{
    assert(start_session_launch("0D3EA0E8-B052-4AC6-9D32-D96918634469",1)==6 && !launches);
    installation=open("/dev/null",O_RDONLY|O_CLOEXEC);assert(installation>=0);
    assert(start_session_launch("0D3EA0E8-B052-4AC6-9D32-D96918634469",1)==42 && launches==1);
    assert(fcntl(installation,F_GETFD)<0 && errno==EBADF);installation=-1;
    unsigned char frame[48]={0};memcpy(frame,"PDSTART1",8);memcpy(frame+8,"0D3EA0E8-B052-4AC6-9D32-D96918634469",36);
    frame[44]=3;frame[45]=0x21;frame[46]=0xb4;frame[47]=0xde;
    char id[37];uint32_t gui=0;assert(start_frame(frame,id,&gui) && gui==0x0321b4de);
    frame[0]='x';assert(!start_frame(frame,id,&gui));frame[0]='P';
    frame[16]='/';assert(!start_frame(frame,id,&gui));frame[16]='-';
    memset(frame+44,0,4);assert(!start_frame(frame,id,&gui));frame[47]=1;
    int pair[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,pair));
    int executable=open("/proc/self/exe",O_RDONLY);assert(executable>=0);
    assert(!start_peer(pair[0],executable));close(executable);
    pid_t child=fork();assert(child>=0);
    if(!child){close(pair[0]);for(unsigned i=0;i<48;++i){assert(write(pair[1],frame+i,1)==1);}_exit(0);}
    close(pair[1]);unsigned char received[48];assert(start_read(pair[0],received));assert(!memcmp(frame,received,48));close(pair[0]);assert(waitpid(child,NULL,0)==child);
    assert(!socketpair(AF_UNIX,SOCK_STREAM,0,pair));assert(write(pair[1],frame,47)==47);close(pair[1]);assert(!start_read(pair[0],received));close(pair[0]);
    assert(!socketpair(AF_UNIX,SOCK_STREAM,0,pair));alarm(3);assert(!start_read(pair[0],received));alarm(0);close(pair[0]);close(pair[1]);
    return 0;
}
