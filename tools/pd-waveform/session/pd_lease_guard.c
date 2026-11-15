#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/capability.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

enum { kSlices=8,kLine=4096,kRestoreSequence=900002 };
struct guard_state {
    int target,idle,snapshot,inventory_known,refresh,query_rejected,restore_sent,restore_ack,restore_mode_seen,restore_rejected,protocol_errors;
    char mode[kSlices][8];unsigned known,inventory;
    uint32_t expected_owner,expected_pan,target_owner,target_pan;
    unsigned target_identity_known;
};
struct input { char line[kLine];size_t used;int discard; };
static volatile sig_atomic_t stop_requested;
static void stop(int signal_number){(void)signal_number;stop_requested=1;}
static int known_mode(const char* mode)
{
    const char* const modes[]={"USB","LSB","DIGU","DIGL","CW","AM","SAM","FM","NFM","DFM","RTTY"};
    for(unsigned i=0;i<sizeof(modes)/sizeof(modes[0]);++i){if(!strcmp(mode,modes[i])){return 1;}}return 0;
}
static int hex_identity(const char* text,uint32_t* value)
{
    if(!text || strlen(text)<3 || strlen(text)>10 || text[0]!='0' || text[1]!='x'){return 0;}
    for(const char* p=text+2;*p;++p){if(!((*p>='0' && *p<='9') || (*p>='a' && *p<='f') || (*p>='A' && *p<='F'))){return 0;}}
    char* end;errno=0;unsigned long parsed=strtoul(text+2,&end,16);
    if(errno || *end || parsed>UINT32_MAX){return 0;}*value=(uint32_t)parsed;return 1;
}
static int target_owned(const struct guard_state* state)
{
    return state->expected_owner && state->expected_pan && state->target_identity_known==3 &&
        state->target_owner==state->expected_owner && state->target_pan==state->expected_pan;
}
static int lifetime_ended(short events){return (events&(POLLHUP|POLLERR|POLLNVAL))!=0;}
/* A duplicate/oversized value is invalid, never a truncated usable token. */
static int field(const char* text,const char* key,char* out,size_t capacity)
{
    int found=0;size_t key_length=strlen(key);
    while(*text){
        while(*text==' '){++text;}if(!*text){break;}
        const char* end=strchr(text,' ');if(!end){end=text+strlen(text);}
        size_t length=(size_t)(end-text);
        if(length>key_length && !memcmp(text,key,key_length) && text[key_length]=='='){
            size_t value_length=length-key_length-1;if(found || value_length>=capacity){return -1;}
            memcpy(out,text+key_length+1,value_length);out[value_length]=0;found=1;
        }
        text=end;
    }
    return found;
}
static void invalid_status(struct guard_state* state)
{
    state->idle=0;state->snapshot=0;state->refresh=1;++state->protocol_errors;
}
static void parse_status(struct guard_state* state,const char* line,int lease,const char* restore_mode)
{
    if(line[0]=='R'){
        if(line[1]<'0' || line[1]>'9'){return;}
        char* end;errno=0;unsigned long sequence=strtoul(line+1,&end,10);
        if(errno || end==line+1 || *end!='|' || end-(line+1)>10 || sequence>UINT32_MAX){return;}
        const char* code=end+1;errno=0;unsigned long error=strtoul(code,&end,16);
        if(errno || end==code || *end!='|' || end-code>8 || error>UINT32_MAX){return;}
        for(const char* p=code;p<end;++p){if(!((*p>='0' && *p<='9') || (*p>='a' && *p<='f') || (*p>='A' && *p<='F'))){return;}}
        if(!lease && sequence==1){state->snapshot=error==0;if(error){state->query_rejected=1;}}
        if(!lease && sequence==3){
            unsigned mask=0;const char* ids=end+1;
            if(error){state->query_rejected=1;return;}
            while(*ids){
                while(*ids==' '){++ids;}if(!*ids){break;}
                if(*ids<'0' || *ids>'9'){invalid_status(state);return;}
                const char* first=ids;char* next;errno=0;unsigned long id=strtoul(ids,&next,10);
                if(errno || next==first || id>=kSlices || (*next && *next!=' ') || mask&(1u<<id)){invalid_status(state);return;}
                mask|=1u<<id;ids=next;
            }
            state->inventory=mask;state->inventory_known=1;
        }
        if(lease && state->restore_sent && sequence==kRestoreSequence){state->restore_ack=error==0;state->restore_rejected=error!=0;}
        return;
    }
    if(line[0]!='S' || lease){return;}
    const char* body=strchr(line,'|');if(!body || body==line+1 || body-line>9){invalid_status(state);return;}
    for(const char* h=line+1;h<body;++h){if(!((*h>='0' && *h<='9') || (*h>='a' && *h<='f') || (*h>='A' && *h<='F'))){invalid_status(state);return;}}++body;
    if(!strncmp(body,"interlock ",10) && strncmp(body+10,"band ",5)){
        char mode[32],source[32],handle[32],allowed[8],reason[64];
        int a=field(body+10,"state",mode,sizeof(mode)),b=field(body+10,"source",source,sizeof(source));
        int c=field(body+10,"tx_client_handle",handle,sizeof(handle)),d=field(body+10,"tx_allowed",allowed,sizeof(allowed)),e=field(body+10,"reason",reason,sizeof(reason));
        if(!a && !b && !c && !d && !e){return;}
        state->idle=a==1 && b==1 && c==1 && d==1 && e==1 && !strcmp(mode,"READY") && !source[0] &&
            !strcmp(handle,"0x00000000") && !strcmp(allowed,"1") && !reason[0];
        return;
    }
    if(strncmp(body,"slice ",6)){return;}
    if(body[6]<'0' || body[6]>'9'){invalid_status(state);return;}
    char* end;errno=0;unsigned long id=strtoul(body+6,&end,10);
    if(errno || end==body+6 || *end!=' ' || id>=kSlices){invalid_status(state);return;}
    char value[8];int present=field(end+1,"in_use",value,sizeof(value));
    if(present<0){invalid_status(state);return;}
    if(present==1 && strcmp(value,"0") && strcmp(value,"1")){invalid_status(state);return;}
    if(present==1 && !strcmp(value,"0")){
        state->known&=~(1u<<id);state->inventory&=~(1u<<id);state->mode[id][0]=0;
        if(id==(unsigned)state->target){state->target_identity_known=0;state->target_owner=state->target_pan=0;}
        return;
    }
    if(id==(unsigned)state->target){
        char identity[16];uint32_t parsed=0;
        present=field(end+1,"client_handle",identity,sizeof(identity));
        if(present<0 || (present && !hex_identity(identity,&parsed))){invalid_status(state);return;}
        if(present){state->target_owner=parsed;state->target_identity_known|=1;}
        present=field(end+1,"pan",identity,sizeof(identity));
        if(present<0 || (present && !hex_identity(identity,&parsed))){invalid_status(state);return;}
        if(present){state->target_pan=parsed;state->target_identity_known|=2;}
    }
    present=field(end+1,"mode",value,sizeof(value));
    if(present<0){state->mode[id][0]=0;invalid_status(state);return;}
    if(present){
        state->known|=1u<<id;state->inventory|=1u<<id;memcpy(state->mode[id],value,strlen(value)+1);
        if(state->restore_sent && id==(unsigned)state->target && !strcmp(value,restore_mode)){state->restore_mode_seen=1;}
    }
}
/* Firmware 4.3.16 uses DEL separators in waveform inventory metadata.
 * Ignore only that status shape; DEL in safety-relevant input is invalid. */
static int waveform_inventory(const char* line)
{
    if(line[0]!='S'){return 0;}
    const char* body=strchr(line,'|');if(!body || body==line+1 || body-line>9){return 0;}
    for(const char* p=line+1;p<body;++p){if(!((*p>='0' && *p<='9') || (*p>='a' && *p<='f') || (*p>='A' && *p<='F'))){return 0;}}
    return !strncmp(body+1,"waveform installed_list=",sizeof("waveform installed_list=")-1);
}
static int feed(struct guard_state* state,struct input* input,const unsigned char* bytes,size_t count,int lease,const char* restore_mode)
{
    for(size_t i=0;i<count;++i){
        unsigned c=bytes[i];
        if(c=='\n'){
            if(input->discard){invalid_status(state);}
            else{
                if(input->used && input->line[input->used-1]=='\r'){--input->used;}
                input->line[input->used]=0;
                if(memchr(input->line,'\r',input->used) || (memchr(input->line,127,input->used) && !waveform_inventory(input->line))){invalid_status(state);}else{parse_status(state,input->line,lease,restore_mode);}
            }
            input->used=0;input->discard=0;continue;
        }
        if((c<32 && c!='\r') || c>127 || input->used>=sizeof(input->line)-1){input->discard=1;continue;}
        if(!input->discard){input->line[input->used++]=(char)c;}
    }
    return 1;
}
static int other_pd(const struct guard_state* state)
{
    for(unsigned i=0;i<kSlices;++i){if(i!=(unsigned)state->target && state->known&(1u<<i) && !strcmp(state->mode[i],"PD")){return 1;}}return 0;
}
static int safe_mode(const struct guard_state* state)
{
    if(!(state->inventory&(1u<<state->target))){return 1;}
    if(!(state->known&(1u<<state->target))){return 0;}
    return target_owned(state) && known_mode(state->mode[state->target]);
}
static int snapshot_complete(const struct guard_state* state)
{
    return state->snapshot && state->inventory_known && !state->query_rejected && (state->known&state->inventory)==state->inventory;
}
static int restore_admitted(const struct guard_state* state)
{
    return state->idle && snapshot_complete(state) && !other_pd(state) && target_owned(state) &&
        !strcmp(state->mode[state->target],"PD") && !state->restore_sent;
}
static void event(const char* name,int slice,int errors)
{
    char output[256];int n=snprintf(output,sizeof(output),"{\"event\":\"%s\",\"slice\":%d,\"protocol_errors\":%d,\"stop_requested\":%s,\"rf_commands\":[]}\n",name,slice,errors,stop_requested?"true":"false");
    if(n>0 && n<(int)sizeof(output)){(void)write(1,output,(size_t)n);}
}
struct write_io {
    ssize_t (*send_bytes)(int,const void*,size_t,int);
    int (*wait)(struct pollfd*,nfds_t,int);
    int64_t (*time_ms)(void);
};
static int64_t monotonic_ms(void)
{
    struct timespec now;if(clock_gettime(CLOCK_MONOTONIC,&now)){return -1;}
    return (int64_t)now.tv_sec*1000+now.tv_nsec/1000000;
}
static int write_command_io(int fd,const char* text,const struct write_io* io)
{
    size_t left=strlen(text);const char* p=text;
    int64_t start=io->time_ms();if(start<0){return 0;}
    while(left){
        int64_t now=io->time_ms();if(now<start || now-start>=2000){return 0;}
        ssize_t n=io->send_bytes(fd,p,left,MSG_NOSIGNAL|MSG_DONTWAIT);
        if(n>0){p+=n;left-=(size_t)n;continue;}
        if(n<0 && errno==EINTR){continue;}
        if(n<0 && (errno==EAGAIN || errno==EWOULDBLOCK)){
            int remaining=(int)(2000-(now-start));if(remaining>500){remaining=500;}
            struct pollfd out={fd,POLLOUT,0};int ready=io->wait(&out,1,remaining);
            if(!ready || (ready<0 && errno==EINTR) || (ready>0 && out.revents&POLLOUT)){continue;}
        }
        return 0;
    }
    return 1;
}
static int write_command(int fd,const char* text)
{
    const struct write_io io={.send_bytes=send,.wait=poll,.time_ms=monotonic_ms};
    return write_command_io(fd,text,&io);
}
static int nonblock(int fd){int flags=fcntl(fd,F_GETFL);return flags>=0 && !fcntl(fd,F_SETFL,flags|O_NONBLOCK);}
static int valid_lease(int fd)
{
    struct sockaddr_in peer={0};socklen_t length=sizeof(peer);int type=0;socklen_t size=sizeof(type);
    return fd>=3 && !getpeername(fd,(struct sockaddr*)&peer,&length) && peer.sin_family==AF_INET && peer.sin_port==htons(4992) && peer.sin_addr.s_addr==htonl(INADDR_LOOPBACK) &&
        !getsockopt(fd,SOL_SOCKET,SO_TYPE,&type,&size) && type==SOCK_STREAM;
}
static int observer_open(void)
{
    int fd=socket(AF_INET,SOCK_STREAM,0);if(fd<0 || !nonblock(fd)){if(fd>=0){close(fd);}return -1;}
    struct sockaddr_in address={.sin_family=AF_INET,.sin_port=htons(4992),.sin_addr={htonl(INADDR_LOOPBACK)}};
    if(connect(fd,(struct sockaddr*)&address,sizeof(address)) && errno!=EINPROGRESS){close(fd);return -1;}
    struct pollfd out={fd,POLLOUT,0};int error=0;socklen_t size=sizeof(error);
    if(poll(&out,1,1000)<=0 || !(out.revents&POLLOUT) || getsockopt(fd,SOL_SOCKET,SO_ERROR,&error,&size) || error){close(fd);return -1;}
    if(!write_command(fd,"C1|sub slice all\nC2|sub tx all\nC3|slice list\n")){close(fd);return -1;}return fd;
}
/* An internal control failure is not proof that the radio connection is
 * gone. Retain the lease; only a genuine peer disconnect ends this hold. */
static int hold_unknown(int api,const char* reason,int target)
{
    event(reason,target,0);
    for(;;){
        struct pollfd p={api,POLLRDHUP,0};int ready=poll(&p,1,500);
        if(ready>0 && p.revents&(POLLHUP|POLLERR|POLLRDHUP)){event("api_lost",target,0);return 7;}
        if(ready<0 && errno!=EINTR){struct timespec delay={0,500000000};nanosleep(&delay,NULL);}
    }
}
/* Only the lifetime pipe is read while DSP owns the duplicated API socket. */
static int guard_run(int api,int life,int target,const char* restore_mode,uint32_t owner,uint32_t pan)
{
    event("armed",target,0);
    for(;;){
        struct pollfd wait={life,0,0};int ready=poll(&wait,1,500);
        if(ready<0 && errno==EINTR){continue;}if(ready<0){return hold_unknown(api,"lifetime_error",target);}
        if(!ready){continue;}
        if(lifetime_ended(wait.revents)){break;}
    }
    if(!nonblock(api)){return hold_unknown(api,"lease_error",target);}
    unsigned char data[8192];size_t discarded=0;
    while(discarded<1024*1024){
        ssize_t n=recv(api,data,sizeof(data),MSG_DONTWAIT);
        if(n>0){discarded+=(size_t)n;continue;}if(n<0 && errno==EINTR){continue;}
        if(n<0 && (errno==EAGAIN || errno==EWOULDBLOCK)){break;}
        event("api_lost",target,0);return 7;
    }
    event("awaiting_idle",target,0);
    struct guard_state state={.target=target,.expected_owner=owner,.expected_pan=pan};struct input monitor_input={0},lease_input={0};int monitor=-1;int announced_rejection=0;
    for(;;){
        if(monitor<0){
            monitor=observer_open();memset(&monitor_input,0,sizeof(monitor_input));state.idle=0;state.snapshot=0;state.inventory_known=0;state.refresh=0;state.known=0;state.inventory=0;state.query_rejected=0;memset(state.mode,0,sizeof(state.mode));
            state.target_owner=state.target_pan=state.target_identity_known=0;
            if(monitor<0){struct timespec delay={1,0};nanosleep(&delay,NULL);continue;}
        }
        struct pollfd fds[2]={{monitor,POLLIN,0},{api,POLLIN,0}};int ready=poll(fds,2,500);
        if(ready<0 && errno==EINTR){continue;}if(ready<0){continue;}
        int monitor_failed=0,lease_failed=0,all_drained=1;
        for(unsigned which=0;which<2;++which){
            if(!(fds[which].revents&(POLLIN|POLLERR|POLLHUP))){continue;}
            for(unsigned batch=0;batch<64;++batch){
                ssize_t n=recv(fds[which].fd,data,sizeof(data),MSG_DONTWAIT);
                if(n>0){
                    feed(&state,which?&lease_input:&monitor_input,data,(size_t)n,which,restore_mode);if(batch==63){all_drained=0;}continue;}
                if(n<0 && errno==EINTR){continue;}
                if(n<0 && (errno==EAGAIN || errno==EWOULDBLOCK)){break;}
                if(which){lease_failed=1;}else{monitor_failed=1;}break;
            }
        }
        if(lease_failed){if(monitor>=0){close(monitor);}event("api_lost",target,state.protocol_errors);return 7;}
        if(monitor_failed){close(monitor);monitor=-1;state.idle=0;state.snapshot=0;continue;}
        if(state.refresh){close(monitor);monitor=-1;state.refresh=0;continue;}
        if(!all_drained){continue;}
        if(state.idle && snapshot_complete(&state) && !other_pd(&state) && safe_mode(&state) && (!state.restore_sent || state.restore_ack)){
            close(monitor);event(state.restore_sent && state.restore_mode_seen && !strcmp(state.mode[target],restore_mode)?"restored":"already_safe",target,state.protocol_errors);return 0;
        }
        if(restore_admitted(&state)){
            char command[96];snprintf(command,sizeof(command),"C%d|slice set %d mode=%s\n",kRestoreSequence,target,restore_mode);
            state.restore_sent=1;state.restore_mode_seen=0;state.restore_ack=0;state.restore_rejected=0;
            if(!write_command(api,command)){close(monitor);return hold_unknown(api,"restore_write_error",target);}
        }
        if((state.restore_rejected || state.query_rejected) && !announced_rejection){event("restore_held",target,state.protocol_errors);announced_rejection=1;}
    }
}
int main(int argc,char** argv)
{
    uint32_t owner=0,pan=0;
    if(argc!=7 || geteuid()==0 || (strcmp(argv[4],"USB") && strcmp(argv[4],"LSB")) ||
       !hex_identity(argv[5],&owner) || !owner || !hex_identity(argv[6],&pan) || !pan || prctl(PR_SET_DUMPABLE,0,0,0,0)){return 2;}
    long values[3];
    for(unsigned i=0;i<3;++i){char* end;errno=0;values[i]=strtol(argv[i+1],&end,10);if(errno || end==argv[i+1] || *end || values[i]<0 || values[i]>1023){return 2;}}
    int api=(int)values[0],life=(int)values[1],target=(int)values[2];if(!valid_lease(api) || life<3 || life==api || target>=kSlices){return 3;}
    struct stat st;if(fstat(life,&st) || !S_ISFIFO(st.st_mode) || (fcntl(life,F_GETFL)&O_ACCMODE)!=O_RDONLY || fstat(1,&st) || !S_ISFIFO(st.st_mode)){return 3;}
    struct __user_cap_header_struct h={_LINUX_CAPABILITY_VERSION_3,0};struct __user_cap_data_struct caps[2]={{0}};
    if(syscall(SYS_capget,&h,caps) || caps[0].effective || caps[0].permitted || caps[0].inheritable ||
       caps[1].effective || caps[1].permitted || caps[1].inheritable){return 3;}
    if(prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0) && errno!=EINVAL){return 3;}
    struct sigaction action={.sa_handler=stop};sigemptyset(&action.sa_mask);sigaction(SIGTERM,&action,NULL);sigaction(SIGINT,&action,NULL);
    signal(SIGPIPE,SIG_IGN);return guard_run(api,life,target,argv[4],owner,pan);
}
