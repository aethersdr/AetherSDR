#define PD_SESSION_MAIN session_launch_program_main
#include "pd_session_launch.c"
#undef PD_SESSION_MAIN
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>

static unsigned open_descriptors(void)
{
    DIR* directory=opendir("/proc/self/fd");assert(directory);unsigned count=0;struct dirent* entry;
    while((entry=readdir(directory))){if(entry->d_name[0]!='.'){++count;}}
    assert(!closedir(directory));return count;
}
static void send_bad(int channel,const int* fds,unsigned count,unsigned bytes,const char* magic,uint32_t client,uint32_t gui)
{
    unsigned char payload[32]={0};struct session_api_reply reply={.client=client,.gui=gui};memcpy(reply.magic,magic,8);memcpy(payload,&reply,sizeof(reply));
    struct iovec vector={payload,bytes};
    union { struct cmsghdr alignment;unsigned char bytes[CMSG_SPACE(9*sizeof(int))]; } control={0};
    struct msghdr message={.msg_iov=&vector,.msg_iovlen=1};
    if(count){
        message.msg_control=control.bytes;message.msg_controllen=CMSG_SPACE(count*sizeof(int));
        struct cmsghdr* header=CMSG_FIRSTHDR(&message);header->cmsg_level=SOL_SOCKET;header->cmsg_type=SCM_RIGHTS;header->cmsg_len=CMSG_LEN(count*sizeof(int));
        memcpy(CMSG_DATA(header),fds,count*sizeof(int));
    }
    assert(sendmsg(channel,&message,MSG_NOSIGNAL)==(ssize_t)bytes);
}
int main(void)
{
    int protocol[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,protocol));
    const char reply[]="V1.4.0.0\nH11111111\nR970101|0|0x22222222\nS";
    assert(send(protocol[0],reply,sizeof(reply)-1,MSG_NOSIGNAL)==sizeof(reply)-1);
    struct stream_state state={.gui=0x22222222,.issued=1};
    assert(session_observe_bind(protocol[1],&state) && state.client==0x11111111);
    unsigned char pending=0;assert(recv(protocol[1],&pending,1,MSG_DONTWAIT)==1 && pending=='S');
    close(protocol[0]);close(protocol[1]);
    assert(!socketpair(AF_UNIX,SOCK_STREAM,0,protocol));
    const char mismatch[]="H11111111\nR970101|0|0x33333333\n";
    assert(send(protocol[0],mismatch,sizeof(mismatch)-1,MSG_NOSIGNAL)==sizeof(mismatch)-1);
    state=(struct stream_state){.gui=0x22222222,.issued=1};
    assert(!session_observe_bind(protocol[1],&state));close(protocol[0]);close(protocol[1]);
    assert(!socketpair(AF_UNIX,SOCK_STREAM,0,protocol));
    const char foreign_ack[]="H11111111\nR910000|0|\n";
    assert(send(protocol[0],foreign_ack,sizeof(foreign_ack)-1,MSG_NOSIGNAL)==sizeof(foreign_ack)-1);
    state=(struct stream_state){.gui=0x22222222,.issued=1};
    assert(!session_observe_bind(protocol[1],&state) && state.failed);
    close(protocol[0]);close(protocol[1]);
    int pair[2],pipe_fds[2];assert(!socketpair(AF_UNIX,SOCK_DGRAM|SOCK_CLOEXEC,0,pair));assert(!pipe(pipe_fds));
    int sources[9];for(unsigned i=0;i<9;++i){sources[i]=pipe_fds[i%2];}
    unsigned baseline=open_descriptors();uint32_t client=0;int received[4]={-1,-1,-1,-1};
    assert(session_send_apis(pair[0],sources,0x11111111,0x22222222));
    assert(session_read_apis(pair[1],received,&client,0x22222222) && client==0x11111111);
    assert(open_descriptors()==baseline+4);
    for(unsigned i=0;i<4;++i){assert(fcntl(received[i],F_GETFD)&FD_CLOEXEC);close(received[i]);}
    assert(open_descriptors()==baseline);
    const unsigned counts[]={0,1,3,5,9};
    for(unsigned i=0;i<sizeof(counts)/sizeof(counts[0]);++i){
        send_bad(pair[0],sources,counts[i],16,"PDAPI001",0x11111111,0x22222222);
        assert(!session_read_apis(pair[1],received,&client,0x22222222));assert(open_descriptors()==baseline);
    }
    const unsigned sizes[]={0,8,15,17,32};
    for(unsigned i=0;i<sizeof(sizes)/sizeof(sizes[0]);++i){
        send_bad(pair[0],sources,4,sizes[i],"PDAPI001",0x11111111,0x22222222);
        assert(!session_read_apis(pair[1],received,&client,0x22222222));assert(open_descriptors()==baseline);
    }
    const char* magic[]={"PDAPI000","XDAPI001","PDAPI002"};
    for(unsigned i=0;i<3;++i){
        send_bad(pair[0],sources,4,16,magic[i],0x11111111,0x22222222);
        assert(!session_read_apis(pair[1],received,&client,0x22222222));assert(open_descriptors()==baseline);
    }
    const uint32_t clients[]={0,0x22222222};
    for(unsigned i=0;i<2;++i){
        send_bad(pair[0],sources,4,16,"PDAPI001",clients[i],0x22222222);
        assert(!session_read_apis(pair[1],received,&client,0x22222222));assert(open_descriptors()==baseline);
    }
    send_bad(pair[0],sources,4,16,"PDAPI001",0x11111111,0x33333333);
    assert(!session_read_apis(pair[1],received,&client,0x22222222));assert(open_descriptors()==baseline);
    /* A well-formed envelope is still subject to kernel descriptor-role
     * validation: four pipe ends cannot stand in for the API sockets. */
    assert(session_send_apis(pair[0],sources,0x11111111,0x22222222));
    assert(session_read_apis(pair[1],received,&client,0x22222222));
    const int wrong[]={pair[0],received[0],received[2],pipe_fds[1],received[1],received[3]};assert(!handoff_roles(wrong));
    for(unsigned i=0;i<4;++i){close(received[i]);}
    close(pair[0]);close(pair[1]);close(pipe_fds[0]);close(pipe_fds[1]);
    puts("PASS: API handoff exact size/count/identity, CLOEXEC, truncated ancillary cleanup and descriptor-role rejection");
    return 0;
}
