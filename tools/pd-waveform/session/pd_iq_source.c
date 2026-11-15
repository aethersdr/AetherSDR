#define main prepared_iq_stream_main
#include "pd_iq_stream.c"
#undef main
#include <stddef.h>
#include <sys/un.h>

/* Ordinary-user socket construction for the measured8600 profile. The caller
 * supplies only private handoff/lifetime channels and sealed GUI metadata. */
static int source_channel(int fd)
{
    int type=0;socklen_t length=sizeof(type);struct sockaddr_un address={0};
    if(getsockopt(fd,SOL_SOCKET,SO_TYPE,&type,&length) || type!=SOCK_DGRAM){return 0;}
    length=sizeof(address);
    if(getsockname(fd,(struct sockaddr*)&address,&length) || address.sun_family!=AF_UNIX || length!=offsetof(struct sockaddr_un,sun_path)){return 0;}
    memset(&address,0,sizeof(address));length=sizeof(address);
    return !getpeername(fd,(struct sockaddr*)&address,&length) && address.sun_family==AF_UNIX && length==offsetof(struct sockaddr_un,sun_path);
}
static int source_lifetime(int fd)
{
    struct stat st;int flags=fcntl(fd,F_GETFL);
    return flags>=0 && (flags&O_ACCMODE)==O_RDONLY && !fstat(fd,&st) && S_ISFIFO(st.st_mode);
}
static int source_canceled(int life)
{
    struct pollfd p={life,0,0};int result=poll(&p,1,0);
    return result<0 || (p.revents&(POLLHUP|POLLERR|POLLNVAL));
}
static int source_connect(int life)
{
    int api=socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0);if(api<0){return -1;}
    struct sockaddr_in address={.sin_family=AF_INET,.sin_port=htons(4992),.sin_addr={htonl(0xac1e0201)}};
    int result=connect(api,(struct sockaddr*)&address,sizeof(address));
    if(result && errno!=EINPROGRESS){close(api);return -1;}
    int64_t start=now_ms();
    while(result){
        int64_t now=now_ms();if(start<0 || now<start || now-start>=2000){close(api);return -1;}
        struct pollfd p[2]={{api,POLLOUT,0},{life,0,0}};int ready=poll(p,2,20);
        if(ready<0 && errno==EINTR){continue;}
        if(ready<0 || p[1].revents&(POLLHUP|POLLERR|POLLNVAL)){close(api);return -1;}
        if(p[0].revents&(POLLOUT|POLLHUP|POLLERR)){
            int error=0;socklen_t length=sizeof(error);
            if(getsockopt(api,SOL_SOCKET,SO_ERROR,&error,&length) || error){close(api);return -1;}
            result=0;
        }
    }
    return api;
}
struct source_reply { char magic[8];uint32_t port,reserved; };
_Static_assert(sizeof(struct source_reply)==16,"fixed local UDP handoff");
static int source_send(int channel,int udp,unsigned port)
{
    struct source_reply reply={.magic={'P','D','U','D','P','0','0','1'},.port=port};
    struct iovec vector={&reply,sizeof(reply)};
    union { struct cmsghdr alignment;unsigned char bytes[CMSG_SPACE(sizeof(int))]; } control={0};
    struct msghdr message={.msg_iov=&vector,.msg_iovlen=1,.msg_control=control.bytes,.msg_controllen=sizeof(control.bytes)};
    struct cmsghdr* header=CMSG_FIRSTHDR(&message);header->cmsg_level=SOL_SOCKET;header->cmsg_type=SCM_RIGHTS;header->cmsg_len=CMSG_LEN(sizeof(int));memcpy(CMSG_DATA(header),&udp,sizeof(udp));
    return sendmsg(channel,&message,MSG_NOSIGNAL|MSG_DONTWAIT)==(ssize_t)sizeof(reply);
}
#ifndef PD_IQ_SOURCE_MAIN
#define PD_IQ_SOURCE_MAIN main
#endif
int PD_IQ_SOURCE_MAIN(int argc,char** argv)
{
    if(argc!=3 || !ordinary() || getgid()!=20 || getegid()!=20 || getgroups(0,NULL)!=0 || !standard_streams()){return 2;}
    int channel=descriptor(argv[1]),life=descriptor(argv[2]);uint32_t gui=0;
    if(channel<0 || life<0 || channel==life || !source_channel(channel) || !source_lifetime(life) ||
       !handle(getenv("SHIM_PD_GUI_HANDLE"),&gui) || !client_id(getenv("SHIM_PD_GUI_CLIENT_ID"))){return 3;}
    if(prctl(PR_SET_DUMPABLE,0,0,0,0) || !close_others(channel,life,-1) || source_canceled(life)){return 4;}
    int udp=socket(AF_INET,SOCK_DGRAM|SOCK_CLOEXEC,0);if(udp<0){return 4;}
    struct sockaddr_in address={.sin_family=AF_INET,.sin_addr={htonl(0xac1e0201)}};
    if(bind(udp,(struct sockaddr*)&address,sizeof(address)) || source_canceled(life)){close(udp);return 4;}
    int api=source_connect(life);unsigned port=0;
    if(api<0 || !prepared(api,udp,life,&port) || source_canceled(life) || !source_send(channel,udp,port)){
        if(api>=0){close(api);}close(udp);return 4;
    }
    close(channel);
    char numbers[3][16];snprintf(numbers[0],sizeof(numbers[0]),"%d",api);snprintf(numbers[1],sizeof(numbers[1]),"%d",udp);snprintf(numbers[2],sizeof(numbers[2]),"%d",life);
    char* arguments[]={"pd-iq-stream",numbers[0],numbers[1],numbers[2],NULL};
    return prepared_iq_stream_main(4,arguments);
}
