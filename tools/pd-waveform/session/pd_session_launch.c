#define main api_stream_program_main
#define standard_streams iq_standard_streams
#include "pd_iq_stream.c"
#undef main
#undef standard_streams
#define PD_PREPARED_MAIN native_prepared_main
#include "pd_prepared_launch.c"
#undef PD_PREPARED_MAIN

/* Development provider step: only IQ and setup lifetime remain external.
 * All radio protocol parsing below runs in our capability-free child. */
struct session_api_reply { char magic[8];uint32_t client,gui; };
_Static_assert(sizeof(struct session_api_reply)==16,"fixed local handoff size");

static int session_connect(void)
{
    int fd=socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0);
    if(fd<0){return -1;}
    struct sockaddr_in address={.sin_family=AF_INET,.sin_port=htons(4992),.sin_addr={htonl(INADDR_LOOPBACK)}};
    int result=connect(fd,(struct sockaddr*)&address,sizeof(address));
    if(result && errno!=EINPROGRESS){close(fd);return -1;}
    if(result){
        struct pollfd p={fd,POLLOUT,0};int ready;
        do{ready=poll(&p,1,1000);}while(ready<0 && errno==EINTR);
        int error=0;socklen_t length=sizeof(error);
        if(ready<=0 || getsockopt(fd,SOL_SOCKET,SO_ERROR,&error,&length) || error){close(fd);return -1;}
    }
    int flags=fcntl(fd,F_GETFL);
    if(flags<0 || fcntl(fd,F_SETFL,flags&~O_NONBLOCK)){close(fd);return -1;}
    return fd;
}
static int session_send_apis(int channel,const int fds[4],uint32_t client,uint32_t gui)
{
    struct session_api_reply reply={.magic={'P','D','A','P','I','0','0','1'},.client=client,.gui=gui};
    struct iovec vector={&reply,sizeof(reply)};
    union { struct cmsghdr alignment;unsigned char bytes[CMSG_SPACE(4*sizeof(int))]; } control={0};
    struct msghdr message={.msg_iov=&vector,.msg_iovlen=1,.msg_control=control.bytes,.msg_controllen=sizeof(control.bytes)};
    struct cmsghdr* header=CMSG_FIRSTHDR(&message);
    header->cmsg_level=SOL_SOCKET;header->cmsg_type=SCM_RIGHTS;header->cmsg_len=CMSG_LEN(4*sizeof(int));
    memcpy(CMSG_DATA(header),fds,4*sizeof(int));
    return sendmsg(channel,&message,MSG_NOSIGNAL)==(ssize_t)sizeof(reply);
}
// Zig's musl CMSG_NXTHDR macro compares size_t with ptrdiff_t on ARM.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
static struct cmsghdr* session_next_header(struct msghdr* message,struct cmsghdr* header)
{
    return CMSG_NXTHDR(message,header);
}
#pragma GCC diagnostic pop
static int session_read_apis(int channel,int fds[4],uint32_t* client,uint32_t gui)
{
    struct session_api_reply reply={0};struct iovec vector={&reply,sizeof(reply)};
    union { struct cmsghdr alignment;unsigned char bytes[CMSG_SPACE(8*sizeof(int))]; } control={0};
    struct msghdr message={.msg_iov=&vector,.msg_iovlen=1,.msg_control=control.bytes,.msg_controllen=sizeof(control.bytes)};
    ssize_t size=recvmsg(channel,&message,MSG_DONTWAIT|MSG_CMSG_CLOEXEC);
    if(size<0){return 0;}
    int received[8],valid=1;unsigned count=0,headers=0;
    for(struct cmsghdr* header=CMSG_FIRSTHDR(&message);header;header=session_next_header(&message,header)){
        ++headers;
        if(header->cmsg_level!=SOL_SOCKET || header->cmsg_type!=SCM_RIGHTS || header->cmsg_len<CMSG_LEN(0)){valid=0;continue;}
        size_t bytes=header->cmsg_len-CMSG_LEN(0);
        if(bytes%sizeof(int)){valid=0;}
        unsigned n=(unsigned)(bytes/sizeof(int));
        for(unsigned i=0;i<n;++i){
            int fd;memcpy(&fd,(unsigned char*)CMSG_DATA(header)+i*sizeof(int),sizeof(fd));
            if(count<8){received[count++]=fd;}else{close(fd);valid=0;}
        }
    }
    valid=valid && size==(ssize_t)sizeof(reply) && !(message.msg_flags&(MSG_TRUNC|MSG_CTRUNC)) &&
        headers==1 && count==4 && !memcmp(reply.magic,"PDAPI001",8) && reply.gui==gui && reply.client && reply.client!=gui;
    if(valid){
        for(unsigned i=0;i<4;++i){if(received[i]<3 || received[i]>1023){valid=0;}}
    }
    if(!valid){for(unsigned i=0;i<count;++i){close(received[i]);}return 0;}
    memcpy(fds,received,4*sizeof(int));*client=reply.client;return 1;
}
static int session_observe_bind(int api,struct stream_state* state)
{
    struct stream_input input={0};int64_t start=prepared_ms();unsigned total=0;
    if(start<0){return 0;}
    for(;;){
        if(state->failed){return 0;}
        if(state->client && (state->replies&1)){return input.used==0;}
        int64_t now=prepared_ms();if(now<start || now-start>=3000 || total>=65536){return 0;}
        struct pollfd p={api,POLLIN,0};int ready=poll(&p,1,50);
        if(ready<0 && errno==EINTR){continue;}if(ready<0 || p.revents&(POLLERR|POLLNVAL)){return 0;}
        if(p.revents&(POLLIN|POLLHUP)){
            // Stop exactly after the complete bind ACK line. A bulk read can
            // consume a prefix of the next status line needed by the guardian.
            unsigned char byte;ssize_t n=recv(api,&byte,1,MSG_DONTWAIT);
            if(n<0 && (errno==EINTR || errno==EAGAIN)){continue;}
            if(n!=1){return 0;}
            unsigned char rejected[96];size_t saved=input.used<sizeof(rejected)?input.used:sizeof(rejected);
            if(byte=='\n'){memcpy(rejected,input.line,saved);}
            if(!feed(state,&input,&byte,1)){
                if(byte=='\n'){
                    const char hex[]="0123456789abcdef";char diagnostic[256]="AetherPD rejected bind frame hex=";size_t used=strlen(diagnostic);
                    for(size_t i=0;i<saved;++i){diagnostic[used++]=hex[rejected[i]>>4];diagnostic[used++]=hex[rejected[i]&15];}
                    diagnostic[used++]='\n';(void)write(2,diagnostic,used);
                }
                return 0;
            }++total;
        }
    }
}
static void session_api_child(int channel,uint32_t gui,const char* id)
{
    const int kept[]={channel};
    if(!prepared_close_except(kept,1,-1) || !drop_root()){_exit(30);}
    int lease=session_connect(),unkey=session_connect(),life[2];
    if(lease<0 || unkey<0 || pipe2(life,O_CLOEXEC)){_exit(31);}
    struct stream_state state={.gui=gui};
    char bind[128];int n=snprintf(bind,sizeof(bind),"C%d|client bind client_id=%s\n",kBind,id);
    if(n<=0 || n>=(int)sizeof(bind) || !issue(unkey,&state,1,bind) || !session_observe_bind(unkey,&state) || state.client==gui){
        char diagnostic[192];int length=snprintf(diagnostic,sizeof(diagnostic),"AetherPD GUI bind failed: client=%08x gui=%08x issued=%u replies=%u parser_failed=%d\n",state.client,state.gui,state.issued,state.replies,state.failed);
        if(length>0 && length<(int)sizeof(diagnostic)){(void)write(2,diagnostic,(size_t)length);}_exit(32);
    }
    // The existing guardian requires its own pending bind acknowledgment.
    // Our earlier transaction verifies the GUI identity; this identical bind
    // leaves sequence910000 unread for the guardian's independent admission.
    n=snprintf(bind,sizeof(bind),"C910000|client bind client_id=%s\n",id);
    if(n<=0 || n>=(int)sizeof(bind) || !command(unkey,bind)){_exit(32);}
    const int fds[]={lease,unkey,life[1],life[0]};
    _exit(session_send_apis(channel,fds,state.client,gui)?0:33);
}
static int session_prepare_apis(int fds[4],uint32_t* client,uint32_t gui,const char* id)
{
    int pair[2];if(socketpair(AF_UNIX,SOCK_DGRAM|SOCK_CLOEXEC,0,pair)){return 0;}
    pid_t child=fork();
    if(child<0){close(pair[0]);close(pair[1]);return 0;}
    if(!child){close(pair[0]);session_api_child(pair[1],gui,id);}
    close(pair[1]);int64_t start=prepared_ms();int got=0,status=0,finished=0,waitable=1;
    while(start>=0){
        int64_t now=prepared_ms();if(now<start || now-start>=8000){break;}
        pid_t done=waitpid(child,&status,WNOHANG);
        if(done==child){finished=1;break;}
        if(done<0 && errno!=EINTR){if(errno==ECHILD){waitable=0;}break;}
        struct pollfd p={pair[0],POLLIN,0};int ready=poll(&p,1,20);
        if(ready<0 && errno!=EINTR){break;}
        if(!got && ready>0 && (p.revents&POLLIN)){
            if(!session_read_apis(pair[0],fds,client,gui)){break;}got=1;
        }
    }
    if(!finished){
        if(waitable){kill(child,SIGKILL);while(waitpid(child,&status,0)<0 && errno==EINTR){}}
    }else if(!got && WIFEXITED(status) && !WEXITSTATUS(status)){
        got=session_read_apis(pair[0],fds,client,gui);
    }
    close(pair[0]);
    if(!finished || !got || !WIFEXITED(status) || WEXITSTATUS(status)){
        char diagnostic[160];int length=snprintf(diagnostic,sizeof(diagnostic),"AetherPD API preparation failed: child_finished=%d waitable=%d status=%d handoff=%d\n",finished,waitable,status,got);
        if(length>0 && length<(int)sizeof(diagnostic)){(void)write(2,diagnostic,(size_t)length);}
        if(got){for(unsigned i=0;i<4;++i){close(fds[i]);}}return 0;
    }
    return 1;
}
#ifndef PD_SESSION_MAIN
#define PD_SESSION_MAIN main
#endif
int PD_SESSION_MAIN(int argc,char** argv)
{
    if(argc!=3 || geteuid()!=0 || !standard_streams() || syscall(SYS_sched_getscheduler,0)!=SCHED_OTHER){return 2;}
    int iq=launch_fd(argv[1]),setup=launch_fd(argv[2]);struct stat setup_stat;
    uint32_t gui=0;const char* id=getenv("SHIM_PD_GUI_CLIENT_ID");
    if(iq<0 || setup<0 || iq==setup || !private_datagram(iq) || !pipe_role(setup,O_WRONLY,&setup_stat) ||
       !launch_handle(getenv("SHIM_PD_GUI_HANDLE"),&gui) || !client_id(id)){return 3;}
    // Reject replaceable installation trees before any API preparation.
    char directory_path[PATH_MAX];int directory=executable_directory(directory_path);
    if(directory<0){return 4;}
    const char* programs[]={kShim,"aether-pd","pd-unkey-guard","pd-lease-guard"};
    for(unsigned i=0;i<4;++i){
        int program=open_program(directory,programs[i]);
        if(program<0){close(directory);return 4;}close(program);
    }
    if(!check_data(directory,"state") || !check_data(directory,"run")){close(directory);return 4;}
    close(directory);
    int api[4];uint32_t client=0;
    if(!session_prepare_apis(api,&client,gui,id)){return 8;}
    const int fds[]={iq,api[0],api[2],setup,api[1],api[3]};
    if(!handoff_roles(fds)){for(unsigned i=0;i<4;++i){close(api[i]);}return 3;}
    char entries[2][64];snprintf(entries[0],sizeof(entries[0]),"0x%08x",client);snprintf(entries[1],sizeof(entries[1]),"0x%08x",gui);
    if(setenv("SHIM_PD_UNKEY_CLIENT_HANDLE",entries[0],1) || setenv("SHIM_PD_GUI_HANDLE",entries[1],1)){return 8;}
    char numbers[6][16];char* args[]={"pd-prepared-launch",numbers[0],numbers[1],numbers[2],numbers[3],numbers[4],numbers[5],NULL};
    for(unsigned i=0;i<6;++i){snprintf(numbers[i],sizeof(numbers[i]),"%d",fds[i]);}
    return native_prepared_main(7,args);
}
