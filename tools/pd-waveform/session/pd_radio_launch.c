#define PD_SESSION_MAIN native_session_main
#include "pd_session_launch.c"
#undef PD_SESSION_MAIN
#include <inttypes.h>

struct radio_child { pid_t pid;int log; };
static int radio_line(int fd,char* line,size_t capacity,int timeout)
{
    size_t used=0;int64_t start=prepared_ms();
    while(start>=0 && used+1<capacity){
        int64_t now=prepared_ms();if(now<start || now-start>=timeout){return 0;}
        struct pollfd p={fd,POLLIN,0};int ready=poll(&p,1,20);
        if(ready<0 && errno==EINTR){continue;}if(ready<0){return 0;}
        if(p.revents&(POLLIN|POLLHUP|POLLERR)){
            unsigned char byte;if(read(fd,&byte,1)!=1){return 0;}
            if(byte=='\n'){line[used++]=(char)byte;line[used]=0;return 1;}
            if(byte<32 || byte>126){return 0;}line[used++]=(char)byte;
        }
    }
    return 0;
}
static struct radio_child radio_spawn(int program,const int* fds,size_t count,char* const* args,int ordinary_child,const char* id,uint32_t gui,int diagnostics)
{
    struct radio_child child={.pid=-1,.log=-1};int output[2];
    if(diagnostics){
        output[0]=-1;output[1]=open("/proc/self/fd/2",O_WRONLY|O_CLOEXEC);
        if(output[1]<0){return child;}
    }else if(pipe2(output,O_CLOEXEC)){return child;}
    child.pid=fork();
    if(child.pid<0){if(output[0]>=0){close(output[0]);}close(output[1]);return child;}
    if(!child.pid){
        if(output[0]>=0){close(output[0]);}
        int executable=fcntl(program,F_DUPFD_CLOEXEC,64);
        if(executable<0 || dup2(output[1],1)!=1 || !prepared_close_except(fds,count,executable)){_exit(40);}
        for(size_t i=0;i<count;++i){int flags=fcntl(fds[i],F_GETFD);if(flags<0 || fcntl(fds[i],F_SETFD,flags&~FD_CLOEXEC)){_exit(40);}}
        if(ordinary_child && !drop_root()){_exit(41);}
        char gui_entry[64],id_entry[96];snprintf(gui_entry,sizeof(gui_entry),"SHIM_PD_GUI_HANDLE=0x%08x",gui);snprintf(id_entry,sizeof(id_entry),"SHIM_PD_GUI_CLIENT_ID=%s",id);
        char* env[]={"PATH=/usr/bin:/bin",gui_entry,id_entry,NULL};
        fexecve(executable,args,env);_exit(42);
    }
    close(output[1]);child.log=output[0];return child;
}
static int radio_arp_ready(const char* line)
{
    return !strcmp(line,"{\"event\":\"ready\",\"uid\":65534,\"owned\":true,\"success\":true,\"rf_commands\":[]}\n") ||
        !strcmp(line,"{\"event\":\"ready\",\"uid\":65534,\"owned\":false,\"success\":true,\"rf_commands\":[]}\n");
}
static int radio_stream_ready(const char* line,uint32_t* sid,uint32_t* pan)
{
    unsigned stream=0,client=0,feedback=0;int used=0;
    int fields=sscanf(line,"{\"event\":\"ready\",\"stream\":\"0x%8x\",\"client\":\"0x%8x\",\"pan\":\"0x%8x\",\"uid\":78666,\"success\":true,\"issued\":191,\"replies\":191,\"fields\":127,\"parser_failed\":false,\"rf_commands\":[]}\n%n",&stream,&client,&feedback,&used);
    if(fields!=3 || !stream || !client || !feedback || used!=(int)strlen(line)){return 0;}
    char canonical[320];
    int length=snprintf(canonical,sizeof(canonical),"{\"event\":\"ready\",\"stream\":\"0x%08x\",\"client\":\"0x%08x\",\"pan\":\"0x%08x\",\"uid\":78666,\"success\":true,\"issued\":191,\"replies\":191,\"fields\":127,\"parser_failed\":false,\"rf_commands\":[]}\n",stream,client,feedback);
    if(length<=0 || length>=(int)sizeof(canonical) || strcmp(line,canonical)){return 0;}
    *sid=stream;*pan=feedback;return 1;
}
static int radio_udp(int channel,unsigned* port)
{
    struct {char magic[8];uint32_t port,reserved;} reply={0};
    struct iovec vector={&reply,sizeof(reply)};
    union {struct cmsghdr alignment;unsigned char bytes[CMSG_SPACE(4*sizeof(int))];} control={0};
    struct msghdr message={.msg_iov=&vector,.msg_iovlen=1,.msg_control=control.bytes,.msg_controllen=sizeof(control.bytes)};
    ssize_t size=recvmsg(channel,&message,MSG_DONTWAIT|MSG_CMSG_CLOEXEC);
    if(size<0){return -1;}
    int received[4];unsigned count=0,headers=0;int valid=1;
    for(struct cmsghdr* h=CMSG_FIRSTHDR(&message);h;h=session_next_header(&message,h)){
        ++headers;
        if(h->cmsg_level!=SOL_SOCKET || h->cmsg_type!=SCM_RIGHTS || h->cmsg_len<CMSG_LEN(0)){valid=0;continue;}
        size_t bytes=h->cmsg_len-CMSG_LEN(0);if(bytes%sizeof(int)){valid=0;}
        for(size_t i=0;i<bytes/sizeof(int);++i){
            int fd;memcpy(&fd,(unsigned char*)CMSG_DATA(h)+i*sizeof(int),sizeof(fd));
            if(count<4){received[count++]=fd;}else{close(fd);valid=0;}
        }
    }
    valid=valid && size==16 && !(message.msg_flags&(MSG_TRUNC|MSG_CTRUNC)) && headers==1 && count==1 &&
        !memcmp(reply.magic,"PDUDP001",8) && !reply.reserved && reply.port>=1024 && reply.port<=65535;
    if(valid){
        int fd=received[0],type=0,reuse=0;struct stat st;struct sockaddr_in endpoint={0};socklen_t length=sizeof(type);
        valid=fd>=3 && fd<=1023 && !fstat(fd,&st) && st.st_uid==78666 && st.st_gid==20 &&
            !getsockopt(fd,SOL_SOCKET,SO_TYPE,&type,&length) && type==SOCK_DGRAM;
        length=sizeof(type);valid=valid && !getsockopt(fd,SOL_SOCKET,SO_PROTOCOL,&type,&length) && type==IPPROTO_UDP;
        length=sizeof(endpoint);valid=valid && !getsockname(fd,(struct sockaddr*)&endpoint,&length) && length==sizeof(endpoint) && endpoint.sin_family==AF_INET && endpoint.sin_addr.s_addr==htonl(0xac1e0201) && ntohs(endpoint.sin_port)==reply.port;
        length=sizeof(reuse);valid=valid && !getsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&reuse,&length) && !reuse;
        length=sizeof(reuse);
        if(valid && getsockopt(fd,SOL_SOCKET,SO_REUSEPORT,&reuse,&length)){valid=errno==ENOPROTOOPT;}else if(valid && reuse){valid=0;}
    }
    if(!valid){for(unsigned i=0;i<count;++i){close(received[i]);}return -1;}
    *port=reply.port;return received[0];
}
static int radio_reap(struct radio_child children[3])
{
    int64_t start=prepared_ms();unsigned pending=0;int success=1;
    for(;;){
        pending=0;
        for(unsigned i=0;i<3;++i){
            if(children[i].pid<=0){continue;}int status=0;pid_t done=waitpid(children[i].pid,&status,WNOHANG);
            if(done==children[i].pid){if(!WIFEXITED(status) || WEXITSTATUS(status)){success=0;}children[i].pid=0;}
            else if(done<0 && errno!=EINTR){success=0;children[i].pid=0;}else{++pending;}
        }
        if(!pending){return success;}
        int64_t now=prepared_ms();if(start<0 || now<start || now-start>=6000){return 0;}
        struct timespec pause={0,10000000};nanosleep(&pause,NULL);
    }
}
#ifndef PD_RADIO_MAIN
#define PD_RADIO_MAIN main
#endif
int PD_RADIO_MAIN(int argc,char** argv)
{
    (void)argv;
    if(argc!=1 || geteuid()!=0 || !standard_streams() || syscall(SYS_sched_getscheduler,0)!=SCHED_OTHER){return 2;}
    uint32_t gui=0;const char* id=getenv("SHIM_PD_GUI_CLIENT_ID");
    if(!client_id(id) || !launch_handle(getenv("SHIM_PD_GUI_HANDLE"),&gui)){return 3;}
    const char* session_text=getenv("SHIM_PD_SESSION_FD");
    if(session_text){
        int fd=launch_fd(session_text),type=0;struct sockaddr_un peer;socklen_t size=sizeof(peer),type_size=sizeof(type);
        if(fd<3 || getsockopt(fd,SOL_SOCKET,SO_TYPE,&type,&type_size) || type!=SOCK_STREAM ||
           getpeername(fd,(struct sockaddr*)&peer,&size) || peer.sun_family!=AF_UNIX){return 3;}
        prepared_session_fd=fd;
    }
    char directory_path[PATH_MAX];int directory=executable_directory(directory_path);if(directory<0){return 4;}
    const char* names[]={"pd-fixed-arp","pd-iq-source","pd-capture-bootstrap",kShim,"aether-pd","pd-unkey-guard","pd-lease-guard"};int programs[7];
    for(unsigned i=0;i<7;++i){programs[i]=open_program(directory,names[i]);if(programs[i]<0){close(directory);return 4;}}
    if(!check_data(directory,"state") || !check_data(directory,"run")){close(directory);return 4;}close(directory);
    int life[2],handoff[2],iq[2];
    if(pipe2(life,O_CLOEXEC) || socketpair(AF_UNIX,SOCK_DGRAM|SOCK_CLOEXEC,0,handoff) || socketpair(AF_UNIX,SOCK_DGRAM|SOCK_CLOEXEC,0,iq)){return 5;}
    int bytes=1<<20;
    if(setsockopt(iq[0],SOL_SOCKET,SO_SNDBUF,&bytes,sizeof(bytes)) || setsockopt(iq[1],SOL_SOCKET,SO_RCVBUF,&bytes,sizeof(bytes))){return 5;}
    struct radio_child children[3]={{.pid=-1,.log=-1},{.pid=-1,.log=-1},{.pid=-1,.log=-1}};
    char read_text[16],channel_text[16];snprintf(read_text,sizeof(read_text),"%d",life[0]);snprintf(channel_text,sizeof(channel_text),"%d",handoff[1]);
    char* arp_args[]={"pd-fixed-arp",read_text,NULL};int arp_fds[]={life[0]};char line[384];int ok=0,udp=-1;unsigned port=0;uint32_t sid=0,pan=0;
    children[2]=radio_spawn(programs[0],arp_fds,1,arp_args,0,id,gui,0);
    if(children[2].pid>0 && radio_line(children[2].log,line,sizeof(line),3000) && radio_arp_ready(line)){
        char* source_args[]={"pd-iq-source",channel_text,read_text,NULL};int source_fds[]={handoff[1],life[0]};
        children[0]=radio_spawn(programs[1],source_fds,2,source_args,1,id,gui,0);close(handoff[1]);handoff[1]=-1;
        struct pollfd ready={handoff[0],POLLIN,0};
        if(children[0].pid>0 && poll(&ready,1,4000)>0){udp=radio_udp(handoff[0],&port);}
        if(udp>=0 && radio_line(children[0].log,line,sizeof(line),6000) && radio_stream_ready(line,&sid,&pan)){
            uint64_t generation=0;int random=open("/dev/urandom",O_RDONLY|O_CLOEXEC);
            if(random>=0){if(read(random,&generation,sizeof(generation))!=(ssize_t)sizeof(generation)){generation=0;}close(random);}
            if(generation){
                char udp_text[16],output_text[16],sid_text[16],generation_text[24];
                snprintf(udp_text,sizeof(udp_text),"%d",udp);snprintf(output_text,sizeof(output_text),"%d",iq[0]);snprintf(sid_text,sizeof(sid_text),"%08x",sid);snprintf(generation_text,sizeof(generation_text),"%" PRIx64,generation);
                char* capture_args[]={"pd-capture-bootstrap",udp_text,output_text,read_text,sid_text,generation_text,"48000","0",NULL};int capture_fds[]={udp,iq[0],life[0]};
                children[1]=radio_spawn(programs[2],capture_fds,3,capture_args,0,id,gui,1);
                // Capture readiness is enforced by the private DSP IQ handshake,
                // then by Go helper admission before its registration gate.
                ok=children[1].pid>0;
            }
        }
    }
    for(unsigned i=0;i<7;++i){close(programs[i]);}
    for(unsigned i=0;i<3;++i){if(children[i].log>=0){close(children[i].log);children[i].log=-1;}}
    close(life[0]);close(handoff[0]);if(handoff[1]>=0){close(handoff[1]);}if(udp>=0){close(udp);}close(iq[0]);
    if(ok){
        prepared_feedback_pan=pan;
        for(unsigned i=0;i<3;++i){prepared_setup_pids[i]=children[i].pid;}
        char event[320];int length=snprintf(event,sizeof(event),"{\"event\":\"native_resources_prepared\",\"source_pid\":%ld,\"capture_pid\":%ld,\"arp_pid\":%ld,\"stream\":\"0x%08x\",\"port\":%u,\"rf_commands\":[]}\n",(long)children[0].pid,(long)children[1].pid,(long)children[2].pid,sid,port);
        if(length>0 && length<(int)sizeof(event) && write(1,event,(size_t)length)==length){
            char iq_text[16],life_text[16];snprintf(iq_text,sizeof(iq_text),"%d",iq[1]);snprintf(life_text,sizeof(life_text),"%d",life[1]);char* arguments[]={"pd-session-launch",iq_text,life_text,NULL};
            int result=native_session_main(3,arguments);close(iq[1]);close(life[1]);int cleaned=radio_reap(children);return result?result:cleaned?0:9;
        }
    }
    close(iq[1]);close(life[1]);int cleaned=radio_reap(children);
    (void)write(2,"AetherPD native resource startup failed\n",39);return cleaned?8:9;
}
