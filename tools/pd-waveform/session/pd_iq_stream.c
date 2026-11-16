#define _GNU_SOURCE
#include "pd_iq_selection.h"
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
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
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

/* Ordinary-user stream owner. Readback binds the created stream to its GUI
 * and pan. This component never changes a receiver, network state or RF. */
enum { kLine=4096,kBind=970101,kSubscribe=970102,kPort=970103,kCreate=970104,kRate=970105,kConfirm=970106,kRemove=970107,kPan=970108,kPanCount=8 };
struct stream_pan { uint32_t id,owner,channel;unsigned fields;int removed; };
struct stream_state {
    uint32_t client,gui,stream,pan,channel;
    unsigned issued,replies,fields;
    int failed,removed,binding_ready;
    struct stream_pan pans[kPanCount];
};
struct stream_input { char line[kLine+1];size_t used; };
static volatile sig_atomic_t stream_stop;
static void stop_stream(int number){(void)number;stream_stop=1;}
static int number(const char* text,unsigned base,unsigned digits,uint32_t* value)
{
    if(!text || !*text || strlen(text)>digits){return 0;}
    uint64_t n=0;
    for(const char* p=text;*p;++p){
        unsigned d;
        if(*p>='0' && *p<='9'){d=(unsigned)(*p-'0');}
        else if(base==16 && *p>='a' && *p<='f'){d=(unsigned)(*p-'a'+10);}
        else if(base==16 && *p>='A' && *p<='F'){d=(unsigned)(*p-'A'+10);}
        else{return 0;}
        if(d>=base){return 0;}n=n*base+d;if(n>UINT32_MAX){return 0;}
    }
    *value=(uint32_t)n;return 1;
}
static int handle(const char* text,uint32_t* value)
{
    return text && text[0]=='0' && text[1]=='x' && number(text+2,16,8,value) && *value;
}
static int descriptor(const char* text)
{
    uint32_t n=0;return number(text,10,4,&n) && n>=3 && n<=1023 ? (int)n : -1;
}
static int client_id(const char* text)
{
    if(!text || strlen(text)!=36){return 0;}
    for(unsigned i=0;i<36;++i){
        if(i==8 || i==13 || i==18 || i==23){if(text[i]!='-'){return 0;}}
        else if(!((text[i]>='0' && text[i]<='9') || (text[i]>='a' && text[i]<='f') || (text[i]>='A' && text[i]<='F'))){return 0;}
    }
    return 1;
}
static int attribute(const char* words,const char* key,char* value,size_t capacity)
{
    size_t length=strlen(key);int found=0;
    while(*words){
        while(*words==' '){++words;}if(!*words){break;}
        const char* end=strchr(words,' ');if(!end){end=words+strlen(words);}
        size_t size=(size_t)(end-words);
        if(size>length && !memcmp(words,key,length) && words[length]=='='){
            size_t n=size-length-1;if(found || n>=capacity){return -1;}
            memcpy(value,words+length+1,n);value[n]=0;found=1;
        }
        words=end;
    }
    return found;
}
/* Pan identities come from FlexLib Panadapter.client_handle and the stream's
 * explicit pan field, never from the VITA stream ID or status-prefix handle. */
static struct stream_pan* stream_find_pan(struct stream_state* s,uint32_t id,int create)
{
    struct stream_pan* vacant=NULL;
    for(unsigned i=0;i<kPanCount;++i){
        if(s->pans[i].id==id){return &s->pans[i];}
        if(!s->pans[i].id && !vacant){vacant=&s->pans[i];}
    }
    if(create && vacant){vacant->id=id;return vacant;}
    return NULL;
}
static int stream_binding_valid(struct stream_state* s)
{
    struct stream_pan* pan=stream_find_pan(s,s->pan,0);
    return s->pan && pan && pan->fields==3 && !pan->removed && pan->owner==s->gui && pan->channel==s->channel;
}
static void stream_parse_pan(struct stream_state* s,char* body)
{
    char* words=strchr(body+12,' ');uint32_t id=0;
    if(!words){s->failed=1;return;}*words++=0;
    if(!handle(body+12,&id)){s->failed=1;return;}
    struct stream_pan* pan=stream_find_pan(s,id,1);
    if(!pan){s->failed=1;return;}
    if(!strcmp(words,"removed")){
        if(id==s->pan){pan->removed=1;s->failed=1;}
        else{memset(pan,0,sizeof(*pan));}
        return;
    }
    const char* keys[]={"client_handle","daxiq_channel"};
    for(unsigned i=0;i<2;++i){
        char value[32];uint32_t parsed=0;int found=attribute(words,keys[i],value,sizeof(value));
        if(found<0){s->failed=1;return;}if(!found){continue;}
        if(i==0?(!handle(value,&parsed) && strcmp(value,"0x00000000")):(!number(value,10,1,&parsed) || parsed>4)){s->failed=1;return;}
        if(i==0){pan->owner=parsed;}else{pan->channel=parsed;}
        pan->fields|=1u<<i;
    }
    if(s->binding_ready && !stream_binding_valid(s)){s->failed=1;}
}
static void parse_line(struct stream_state* s,char* line)
{
    if(s->failed){return;}
    if(line[0]=='H'){
        uint32_t h=0;if(s->client || !number(line+1,16,8,&h) || !h){s->failed=1;}else{s->client=h;}return;
    }
    if(line[0]=='R'){
        char* bar=strchr(line+1,'|');if(!bar){s->failed=1;return;}*bar=0;uint32_t seq=0;
        if(!number(line+1,10,6,&seq)){s->failed=1;return;}
        char* body=strchr(bar+1,'|');if(!body){s->failed=1;return;}*body++=0;uint32_t code=0;
        if(!number(bar+1,16,8,&code) || seq<kBind || seq>kPan || code || !(s->issued&(1u<<(seq-kBind))) || (s->replies&(1u<<(seq-kBind)))){s->failed=1;return;}
        if(seq==kBind){
            uint32_t bound=0;if(!handle(body,&bound) || bound!=s->gui){s->failed=1;return;}
        }else if(seq==kCreate){
            uint32_t id=0;const char* value=!strncmp(body,"0x",2)?body+2:body;
            if(!number(value,16,8,&id) || !id || s->stream){s->failed=1;return;}s->stream=id;
        }else if(seq==kRate){
            /* Firmware4.3.16 returns this informational text; readback below
             * must still establish the requested stream rate. */
            if(*body && strcmp(body,"DAXIQ rate set succesfully")){s->failed=1;return;}
        }else if(*body){s->failed=1;return;}
        s->replies|=1u<<(seq-kBind);return;
    }
    if(line[0]!='S'){if(line[0]!='V' && line[0]!='M'){s->failed=1;}return;}
    char* body=strchr(line,'|');uint32_t recipient=0;
    if(!body){s->failed=1;return;}*body++=0;
    if(!number(line+1,16,8,&recipient)){s->failed=1;return;}
    /* FlexLib routes by status topic, not the S-prefix handle. Resource
     * ownership is sealed by our create ACK and explicit stream fields. */
    if(!strncmp(body,"display pan ",12)){stream_parse_pan(s,body);return;}
    if(strncmp(body,"stream ",7)){return;}
    char* words=strchr(body+7,' ');if(!words){s->failed=1;return;}*words++=0;uint32_t id=0;
    if(!handle(body+7,&id)){s->failed=1;return;}
    /* The create ACK seals ownership. A pre-ACK status is not authority. */
    if(id!=s->stream || !s->stream){return;}
    if(!strcmp(words,"removed")){s->removed=1;return;}
    const char* keys[]={"type","daxiq_channel","daxiq_rate","client_handle","client_gui_handle","ip","pan"};
    for(unsigned i=0;i<7;++i){
        char value[64];int found=attribute(words,keys[i],value,sizeof(value));uint32_t h=0;
        if(found<0){s->failed=1;return;}if(!found){continue;}
        if(i==2 && !(s->issued&(1u<<(kRate-kBind)))){continue;}
        char selected_channel[2]={(char)('0'+s->channel),0};
        int valid=i==0?!strcmp(value,"dax_iq"):i==1?!strcmp(value,selected_channel):i==2?!strcmp(value,"48000"):
            i==3?(handle(value,&h) && h==s->client):i==4?(handle(value,&h) && h==s->gui):
            i==5?!strcmp(value,"172.30.2.1"):(handle(value,&h) && (!s->pan || s->pan==h));
        if(i==6 && valid){s->pan=h;}
        if(!valid){s->failed=1;return;}s->fields|=1u<<i;
    }
    if(s->binding_ready && !stream_binding_valid(s)){s->failed=1;}
}
static int feed(struct stream_state* s,struct stream_input* in,const unsigned char* data,size_t count)
{
    for(size_t i=0;i<count;++i){unsigned c=data[i];
        if(c=='\n'){
            if(in->used && in->line[in->used-1]=='\r'){--in->used;}
            in->line[in->used]=0;if(!in->used || memchr(in->line,'\r',in->used)){return 0;}
            parse_line(s,in->line);in->used=0;if(s->failed){return 0;}
        }else{
            if((c<32 && c!='\r') || c>127 || in->used>=kLine){return 0;}
            in->line[in->used++]=(char)c;
        }
    }
    return 1;
}
static int64_t now_ms(void)
{
    struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t)){return -1;}return (int64_t)t.tv_sec*1000+t.tv_nsec/1000000;
}
static int nonblock(int fd){int f=fcntl(fd,F_GETFL);return f>=0 && !fcntl(fd,F_SETFL,f|O_NONBLOCK);}
static int command(int api,const char* text)
{
    size_t size=strlen(text),sent=0;int64_t start=now_ms();if(start<0){return 0;}
    while(sent<size){
        int64_t now=now_ms();if(now<start || now-start>=2000){return 0;}
        ssize_t n=send(api,text+sent,size-sent,MSG_NOSIGNAL|MSG_DONTWAIT);
        if(n>0){sent+=(size_t)n;continue;}if(n<0 && errno==EINTR){continue;}
        if(n<0 && (errno==EAGAIN || errno==EWOULDBLOCK)){struct pollfd p={api,POLLOUT,0};poll(&p,1,20);continue;}return 0;
    }
    return 1;
}
static int issue(int api,struct stream_state* s,unsigned mask,const char* text)
{
    if(!mask || s->issued&mask){return 0;}s->issued|=mask;return command(api,text);
}
static int observe(int api,int life,struct stream_state* s,struct stream_input* in,unsigned mask,int require_fields,int cancel)
{
    int64_t start=now_ms();size_t total=0;if(start<0){return 0;}
    for(;;){
        if(s->failed || (s->removed && !(mask&(1u<<(kRemove-kBind))))){return 0;}
        if((s->replies&mask)==mask && (!require_fields || (s->fields==127 && stream_binding_valid(s))) && s->client){return 1;}
        if(cancel && stream_stop){return 0;}
        int64_t now=now_ms();if(now<start || now-start>=3000){return 0;}
        struct pollfd p[2]={{api,POLLIN,0},{life,0,0}};int r=poll(p,cancel?2:1,50);
        if(r<0 && errno==EINTR){continue;}if(r<0 || (cancel && p[1].revents&(POLLHUP|POLLERR|POLLNVAL))){return 0;}
        if(p[0].revents&(POLLIN|POLLHUP|POLLERR)){
            unsigned char bytes[4096];ssize_t n=recv(api,bytes,sizeof(bytes),MSG_DONTWAIT);
            if(n<=0){if(n<0 && (errno==EAGAIN || errno==EINTR)){continue;}return 0;}
            total+=(size_t)n;if(total>1024*1024 || !feed(s,in,bytes,(size_t)n)){return 0;}
        }
    }
}
static int prepared(int api,int udp,int life,unsigned* port)
{
    if(api==udp || api==life || udp==life){return 0;}
    struct sockaddr_in a={0};socklen_t n=sizeof(a);int type=0;socklen_t t=sizeof(type);
    if(getsockopt(api,SOL_SOCKET,SO_TYPE,&type,&t) || type!=SOCK_STREAM || getpeername(api,(struct sockaddr*)&a,&n) || n!=sizeof(a) || a.sin_family!=AF_INET || a.sin_addr.s_addr!=htonl(0xac1e0201) || a.sin_port!=htons(4992)){return 0;}
    t=sizeof(type);
    if(getsockopt(api,SOL_SOCKET,SO_PROTOCOL,&type,&t) || type!=IPPROTO_TCP){return 0;}
    n=sizeof(a);t=sizeof(type);
    if(getsockopt(udp,SOL_SOCKET,SO_TYPE,&type,&t) || type!=SOCK_DGRAM || getsockname(udp,(struct sockaddr*)&a,&n) || n!=sizeof(a) || a.sin_family!=AF_INET || a.sin_addr.s_addr!=htonl(0xac1e0201) || ntohs(a.sin_port)<1024){return 0;}
    t=sizeof(type);if(getsockopt(udp,SOL_SOCKET,SO_PROTOCOL,&type,&t) || type!=IPPROTO_UDP){return 0;}
    int reused=0;t=sizeof(reused);if(getsockopt(udp,SOL_SOCKET,SO_REUSEADDR,&reused,&t) || reused){return 0;}
    t=sizeof(reused);if(getsockopt(udp,SOL_SOCKET,SO_REUSEPORT,&reused,&t)){if(errno!=ENOPROTOOPT){return 0;}}else if(reused){return 0;}
    struct stat st;int flags=fcntl(life,F_GETFL);
    if(flags<0 || (flags&O_ACCMODE)!=O_RDONLY || fstat(life,&st) || !S_ISFIFO(st.st_mode)){return 0;}
    *port=ntohs(a.sin_port);return 1;
}
static int stream_standard_policy(const struct stat fds[3],const int flags[3])
{
    return flags[0]>=0 && (flags[0]&O_ACCMODE)!=O_WRONLY && S_ISCHR(fds[0].st_mode) &&
        fds[0].st_rdev==makedev(1,3) && flags[1]>=0 && flags[2]>=0 &&
        (flags[1]&O_ACCMODE)==O_WRONLY && (flags[2]&O_ACCMODE)==O_WRONLY &&
        S_ISFIFO(fds[1].st_mode) && S_ISFIFO(fds[2].st_mode);
}
static int standard_streams(void)
{
    struct stat fds[3];int flags[3];
    for(int i=0;i<3;++i){if(fstat(i,&fds[i])){return 0;}flags[i]=fcntl(i,F_GETFL);}
    return stream_standard_policy(fds,flags);
}
static int ordinary(void)
{
    struct __user_cap_header_struct h={_LINUX_CAPABILITY_VERSION_3,0};struct __user_cap_data_struct c[2]={{0}};
    return getuid()==78666 && geteuid()==78666 && !syscall(SYS_capget,&h,c) && !c[0].effective && !c[0].permitted && !c[0].inheritable && !c[1].effective && !c[1].permitted && !c[1].inheritable;
}
static int close_others(int api,int udp,int life)
{
    DIR* d=opendir("/proc/self/fd");if(!d){return 0;}int own=dirfd(d);struct dirent* e;
    while((e=readdir(d))){if(e->d_name[0]=='.'){continue;}uint32_t fd=0;if(!number(e->d_name,10,10,&fd) || fd>INT_MAX){closedir(d);return 0;}
        if(fd>2 && fd!=(unsigned)api && fd!=(unsigned)udp && fd!=(unsigned)life && fd!=(unsigned)own){close((int)fd);}}
    return !closedir(d);
}
static int event(const char* name,const struct stream_state* s,int success)
{
    char line[320];int n=snprintf(line,sizeof(line),"{\"event\":\"%s\",\"stream\":\"0x%08x\",\"client\":\"0x%08x\",\"pan\":\"0x%08x\",\"channel\":%u,\"uid\":%u,\"success\":%s,\"issued\":%u,\"replies\":%u,\"fields\":%u,\"parser_failed\":%s,\"rf_commands\":[]}\n",name,s->stream,s->client,s->pan,s->channel,(unsigned)geteuid(),success?"true":"false",s->issued,s->replies,s->fields,s->failed?"true":"false");
    return n>0 && n<(int)sizeof(line) && write(1,line,(size_t)n)==n;
}
static int run_stream(int api,int life,unsigned port,const char* gui_id,struct stream_state* s,struct stream_input* in)
{
    char text[160];
    if(s->channel<1 || s->channel>4){return 0;}
    if(!observe(api,life,s,in,0,0,1)){return 0;}
    snprintf(text,sizeof(text),"C%d|client bind client_id=%s\n",kBind,gui_id);
    if(!issue(api,s,1u,text) || !observe(api,life,s,in,1u,0,1)){return 0;}
    snprintf(text,sizeof(text),"C%d|sub daxiq all\nC%d|client udpport %u\n",kSubscribe,kPort,port);
    if(!issue(api,s,6u,text) || !observe(api,life,s,in,7u,0,1)){return 0;}
    if(!issue(api,s,128u,"C970108|sub pan all\n") || !observe(api,life,s,in,135u,0,1)){return 0;}
    snprintf(text,sizeof(text),"C970104|stream create type=dax_iq daxiq_channel=%u\n",s->channel);
    if(!issue(api,s,8u,text) || !observe(api,life,s,in,15u,0,1)){return 0;}
    snprintf(text,sizeof(text),"C%d|stream set 0x%08X daxiq_rate=48000\n",kRate,s->stream);
    if(!issue(api,s,16u,text) || !observe(api,life,s,in,31u,0,1)){return 0;}
    if(!issue(api,s,32u,"C970106|sub daxiq all\n") || !observe(api,life,s,in,191u,1,1)){return 0;}
    s->binding_ready=1;
    if(!event("ready",s,1)){return 0;}
    int success=1;
    while(!stream_stop){struct pollfd p[2]={{life,0,0},{api,POLLIN,0}};int r=poll(p,2,100);
        if(r<0 && errno==EINTR){continue;}if(r<0){success=0;break;}
        if(p[0].revents&(POLLHUP|POLLERR|POLLNVAL)){break;}
        if(p[1].revents&(POLLIN|POLLERR|POLLHUP)){
            unsigned char data[4096];ssize_t n=recv(api,data,sizeof(data),MSG_DONTWAIT);
            if(n<=0 || !feed(s,in,data,(size_t)n) || s->removed){success=0;break;}
        }
    }
    return success;
}
int main(int argc,char** argv)
{
    if(argc!=4 || !ordinary() || !standard_streams()){return 2;}
    int api=descriptor(argv[1]),udp=descriptor(argv[2]),life=descriptor(argv[3]);unsigned port=0;struct stream_state s={0};struct stream_input in={0};
    struct pd_iq_selection selection;
    if(!pd_iq_selection_environment(&selection)){return 3;}
    s.channel=selection.channel;s.pan=selection.pan;
    const char* gui_id=getenv("SHIM_PD_GUI_CLIENT_ID");
    if(api<0 || udp<0 || life<0 || !handle(getenv("SHIM_PD_GUI_HANDLE"),&s.gui) || !client_id(gui_id) || !prepared(api,udp,life,&port)){return 3;}
    if(prctl(PR_SET_DUMPABLE,0,0,0,0) || !close_others(api,udp,life) || !nonblock(api) || !nonblock(1)){return 4;}
    struct sigaction action={0};action.sa_handler=stop_stream;sigemptyset(&action.sa_mask);sigaction(SIGTERM,&action,NULL);sigaction(SIGINT,&action,NULL);signal(SIGPIPE,SIG_IGN);
    int success=run_stream(api,life,port,gui_id,&s,&in);char text[160];
    if(success && s.stream && !s.failed && !s.removed){
        /* On an uncertain or removed identity, close only our API lease;
         * never send an ID-based removal after ownership has become unclear. */
        struct stream_state removal={.client=s.client,.gui=s.gui,.stream=s.stream};struct stream_input tail={0};
        snprintf(text,sizeof(text),"C%d|stream remove 0x%08X\n",kRemove,s.stream);
        if(!issue(api,&removal,64u,text) || !observe(api,life,&removal,&tail,1u<<(kRemove-kBind),0,0)){success=0;}
    }
    close(api);close(udp);close(life);event("complete",&s,success);return success?0:5;
}
