#define _GNU_SOURCE
#include <sys/socket.h>
static unsigned source_socket_calls;
static int tracked_source_socket(int domain,int type,int protocol)
{
    ++source_socket_calls;return socket(domain,type,protocol);
}
#define socket tracked_source_socket
#define PD_IQ_SOURCE_MAIN iq_source_program_main
#include "pd_iq_source.c"
#undef PD_IQ_SOURCE_MAIN
#undef socket
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
int main(void)
{
    int channel[2],life[2];assert(!socketpair(AF_UNIX,SOCK_DGRAM|SOCK_CLOEXEC,0,channel));assert(!pipe(life));
    assert(source_channel(channel[0]) && source_channel(channel[1]));
    assert(!source_channel(life[0]) && !source_channel(-1));
    assert(source_lifetime(life[0]) && !source_lifetime(life[1]) && !source_lifetime(channel[0]));
    assert(!source_canceled(life[0]));close(life[1]);assert(source_canceled(life[0]));
    int streams[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,streams));assert(!source_channel(streams[0]));close(streams[0]);close(streams[1]);
    int bound=socket(AF_UNIX,SOCK_DGRAM,0);assert(bound>=0);
    struct sockaddr_un named={.sun_family=AF_UNIX};snprintf(named.sun_path+1,sizeof(named.sun_path)-1,"aetherpd-iq-source-test-%ld",(long)getpid());
    assert(!bind(bound,(struct sockaddr*)&named,offsetof(struct sockaddr_un,sun_path)+1+strlen(named.sun_path+1)));assert(!source_channel(bound));close(bound);
    int udp=socket(AF_INET,SOCK_DGRAM|SOCK_CLOEXEC,0);assert(udp>=0);
    struct sockaddr_in address={.sin_family=AF_INET,.sin_addr={htonl(INADDR_LOOPBACK)}};assert(!bind(udp,(struct sockaddr*)&address,sizeof(address)));socklen_t length=sizeof(address);assert(!getsockname(udp,(struct sockaddr*)&address,&length));
    unsigned port=ntohs(address.sin_port);assert(source_send(channel[0],udp,port));
    struct source_reply reply={0};struct iovec vector={&reply,sizeof(reply)};
    union { struct cmsghdr alignment;unsigned char bytes[CMSG_SPACE(sizeof(int))]; } control={0};
    struct msghdr message={.msg_iov=&vector,.msg_iovlen=1,.msg_control=control.bytes,.msg_controllen=sizeof(control.bytes)};
    assert(recvmsg(channel[1],&message,MSG_CMSG_CLOEXEC)==sizeof(reply));assert(!(message.msg_flags&(MSG_TRUNC|MSG_CTRUNC)));
    struct cmsghdr* header=CMSG_FIRSTHDR(&message);assert(header && header->cmsg_level==SOL_SOCKET && header->cmsg_type==SCM_RIGHTS && header->cmsg_len==CMSG_LEN(sizeof(int)));
    int received=-1;memcpy(&received,CMSG_DATA(header),sizeof(received));assert(received>=3 && (fcntl(received,F_GETFD)&FD_CLOEXEC));
    struct stat original,copy;assert(!fstat(udp,&original) && !fstat(received,&copy));assert(original.st_ino==copy.st_ino && original.st_dev==copy.st_dev);
    assert(!memcmp(reply.magic,"PDUDP001",8) && reply.port==port && !reply.reserved);
    close(received);close(udp);close(channel[0]);close(channel[1]);close(life[0]);
    if(ordinary() && getgid()==20 && getgroups(0,NULL)==0 && standard_streams()){
        int canceled[2],private[2];assert(!pipe(canceled));close(canceled[1]);
        assert(!socketpair(AF_UNIX,SOCK_DGRAM,0,private));
        assert(!setenv("SHIM_PD_GUI_CLIENT_ID","0D3EA0E8-B052-4AC6-9D32-D96918634469",1));
        assert(!setenv("SHIM_PD_GUI_HANDLE","0x11111111",1));
        char channel_text[16],life_text[16];snprintf(channel_text,sizeof(channel_text),"%d",private[0]);snprintf(life_text,sizeof(life_text),"%d",canceled[0]);
        char* arguments[]={"pd-iq-source",channel_text,life_text,NULL};
        unsigned before=source_socket_calls;
        assert(iq_source_program_main(3,arguments)==4);
        assert(source_socket_calls==before);
        close(private[0]);close(canceled[0]);
        puts("PASS: canceled ordinary-user source creates zero UDP/API sockets");
    }
    puts("PASS: IQ source private-channel/lifetime admission, cancellation and exact UDP descriptor handoff; no radio API");
    return 0;
}
