#define _GNU_SOURCE
#include "pd_readiness.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

struct json_cursor { const unsigned char* p;const unsigned char* end;unsigned tokens; };
static void json_space(struct json_cursor* c)
{
    while(c->p<c->end && (*c->p==' ' || *c->p=='\t' || *c->p=='\r' || *c->p=='\n')){++c->p;}
}
static int json_take(struct json_cursor* c,unsigned char value)
{
    json_space(c);if(c->p==c->end || *c->p!=value){return 0;}++c->p;return 1;
}
static int json_hex(unsigned char c)
{
    return (c>='0' && c<='9') || (c>='a' && c<='f') || (c>='A' && c<='F');
}
static int json_string(struct json_cursor* c,char* output,size_t capacity)
{
    if(!json_take(c,'"')){return 0;}size_t used=0;
    while(c->p<c->end){
        unsigned char value=*c->p++;
        if(value=='"'){if(output){output[used]=0;}return 1;}
        if(value<32){return 0;}
        if(value=='\\'){
            /* Known identity/key fields have an ASCII wire representation.
             * Unknown extension values may use JSON escapes. */
            if(output || c->p==c->end){return 0;}value=*c->p++;
            if(value=='u'){
                if((size_t)(c->end-c->p)<4){return 0;}
                for(unsigned i=0;i<4;++i){if(!json_hex(*c->p++)){return 0;}}
            }else if(!value || !strchr("\"\\/bfnrt",value)){return 0;}
        }else if(output){
            if(value>126 || used+1>=capacity){return 0;}output[used++]=(char)value;
        }
    }
    return 0;
}
static int json_literal(struct json_cursor* c,const char* value)
{
    json_space(c);size_t size=strlen(value);
    if((size_t)(c->end-c->p)<size || memcmp(c->p,value,size)){return 0;}c->p+=size;return 1;
}
static int json_skip(struct json_cursor* c,unsigned depth)
{
    json_space(c);if(depth>16 || ++c->tokens>8192 || c->p==c->end){return 0;}
    unsigned char first=*c->p;
    if(first=='"'){return json_string(c,NULL,0);}
    if(first=='{' || first=='['){
        ++c->p;unsigned char end=first=='{'?'}':']';if(json_take(c,end)){return 1;}
        do{
            if(first=='{' && (!json_string(c,NULL,0) || !json_take(c,':'))){return 0;}
            if(!json_skip(c,depth+1)){return 0;}
            if(json_take(c,end)){return 1;}
        }while(json_take(c,','));return 0;
    }
    if(first=='t'){return json_literal(c,"true");}if(first=='f'){return json_literal(c,"false");}
    if(first=='n'){return json_literal(c,"null");}
    if(*c->p=='-'){++c->p;}if(c->p==c->end){return 0;}
    if(*c->p=='0'){++c->p;}
    else{
        if(*c->p<'1' || *c->p>'9'){return 0;}
        do{++c->p;}while(c->p<c->end && *c->p>='0' && *c->p<='9');
    }
    if(c->p<c->end && *c->p=='.'){
        ++c->p;const unsigned char* begin=c->p;
        while(c->p<c->end && *c->p>='0' && *c->p<='9'){++c->p;}if(c->p==begin){return 0;}
    }
    if(c->p<c->end && (*c->p=='e' || *c->p=='E')){
        ++c->p;if(c->p<c->end && (*c->p=='+' || *c->p=='-')){++c->p;}
        const unsigned char* begin=c->p;
        while(c->p<c->end && *c->p>='0' && *c->p<='9'){++c->p;}if(c->p==begin){return 0;}
    }
    return 1;
}
static int ready_text(const char* value,size_t capacity,int dns)
{
    const char* end=memchr(value,0,capacity);if(!end || end==value){return 0;}
    for(const char* p=end;p<value+capacity;++p){if(*p){return 0;}}
    size_t length=(size_t)(end-value),label=0;
    if(dns && length>253){return 0;}
    for(size_t i=0;i<length;++i){
        unsigned char c=(unsigned char)value[i];
        if(dns && c=='.'){
            if(!label || value[i-1]=='-'){return 0;}label=0;continue;
        }
        if(!((c>='a' && c<='z') || (c>='0' && c<='9') || c=='-') ||
            (!label && c=='-') || ++label>63){return 0;}
    }
    return value[length-1]!='-' && (label || (dns && value[length-1]=='.'));
}
int pd_readiness_valid(const struct pd_readiness* value)
{
    struct in_addr address;char text[16];const char* end=memchr(value->address,0,sizeof(value->address));
    if(!ready_text(value->hostname,sizeof(value->hostname),0) || !ready_text(value->dns,sizeof(value->dns),1) || !end){return 0;}
    for(const char* p=end;p<value->address+sizeof(value->address);++p){if(*p){return 0;}}
    return inet_pton(AF_INET,value->address,&address)==1 &&
        (ntohl(address.s_addr)&0xffc00000u)==0x64400000u &&
        inet_ntop(AF_INET,&address,text,sizeof(text)) && !strcmp(text,value->address);
}
int pd_readiness_parse(const unsigned char* bytes,size_t size,struct pd_readiness* result)
{
    memset(result,0,sizeof(*result));if(!size || size>65536){return 0;}
    struct json_cursor c={bytes,bytes+size,0};struct pd_readiness parsed={0};unsigned seen=0;
    if(!json_take(&c,'{')){return 0;}
    do{
        char key[64];if(!json_string(&c,key,sizeof(key)) || !json_take(&c,':')){return 0;}
        unsigned bit=0;int ok=0;
        if(!strcmp(key,"hostname")){bit=1;ok=json_string(&c,parsed.hostname,sizeof(parsed.hostname));}
        else if(!strcmp(key,"tailnet_ip")){bit=2;ok=json_string(&c,parsed.address,sizeof(parsed.address));}
        else if(!strcmp(key,"dns_name")){bit=4;ok=json_string(&c,parsed.dns,sizeof(parsed.dns));}
        else if(!strcmp(key,"provisioned")){bit=8;ok=json_literal(&c,"true");}
        else if(!strcmp(key,"state")){char state[16];bit=16;ok=json_string(&c,state,sizeof(state)) && !strcmp(state,"running");}
        else if(!strcmp(key,"sessions")){bit=32;ok=json_literal(&c,"0");}
        else if(!strcmp(key,"claimable")){bit=64;ok=json_literal(&c,"false");}
        else{ok=json_skip(&c,0);}
        if(!ok || (seen&bit)){return 0;}seen|=bit;
        if(json_take(&c,'}')){json_space(&c);if(c.p!=c.end || seen!=127 || !pd_readiness_valid(&parsed)){return 0;}*result=parsed;return 1;}
    }while(json_take(&c,','));return 0;
}
int pd_readiness_equal(const struct pd_readiness* a,const struct pd_readiness* b)
{
    return pd_readiness_valid(a) && pd_readiness_valid(b) && !strcmp(a->hostname,b->hostname) &&
        !strcmp(a->address,b->address) && !strcmp(a->dns,b->dns);
}
int pd_readiness_http(const unsigned char* bytes,size_t size,struct pd_readiness* result)
{
    memset(result,0,sizeof(*result));
    if(size<19 || size>PD_READINESS_HTTP_MAX ||
        (memcmp(bytes,"HTTP/1.0 200 OK\r\n",17) && memcmp(bytes,"HTTP/1.1 200 OK\r\n",17))){return 0;}
    size_t offset=17,length=0;int have_length=0,have_type=0;
    while(offset<size && offset<4096){
        size_t begin=offset;
        while(offset+1<size && !(bytes[offset]=='\r' && bytes[offset+1]=='\n')){
            if(bytes[offset]<32 || bytes[offset]>126){return 0;}++offset;
        }
        if(offset+1>=size || offset>=4096){return 0;}size_t count=offset-begin;offset+=2;
        if(!count){return have_type && (!have_length || size-offset==length) && pd_readiness_parse(bytes+offset,size-offset,result);}
        const unsigned char* colon=memchr(bytes+begin,':',count);if(!colon || colon==bytes+begin){return 0;}
        size_t key=(size_t)(colon-(bytes+begin));
        for(size_t i=0;i<key;++i){unsigned char c=bytes[begin+i];if(!((c>='a' && c<='z') || (c>='A' && c<='Z') || c=='-')){return 0;}}
        const unsigned char* value=colon+1;const unsigned char* end=bytes+begin+count;
        while(value<end && *value==' '){++value;}while(end>value && end[-1]==' '){--end;}
        if(key==17 && !strncasecmp((const char*)bytes+begin,"transfer-encoding",key)){return 0;}
        if(key==14 && !strncasecmp((const char*)bytes+begin,"content-length",key)){
            if(have_length || value==end){return 0;}have_length=1;
            for(;value<end;++value){if(*value<'0' || *value>'9' || length>6553){return 0;}length=10*length+(*value-'0');}
            if(length>65536){return 0;}
        }else if(key==12 && !strncasecmp((const char*)bytes+begin,"content-type",key)){
            if(have_type || (size_t)(end-value)!=16 || memcmp(value,"application/json",16)){return 0;}have_type=1;
        }
    }
    return 0;
}
static int64_t ready_now(void)
{
    struct timespec value;if(clock_gettime(CLOCK_MONOTONIC,&value)){return -1;}
    return (int64_t)value.tv_sec*1000+value.tv_nsec/1000000;
}
static int ready_poll(int fd,short events,int64_t deadline)
{
    for(;;){
        int64_t now=ready_now();if(now<0 || now>=deadline){return 0;}
        struct pollfd p={fd,events,0};int rc=poll(&p,1,(int)(deadline-now));
        if(rc<0 && errno==EINTR){continue;}return rc>0;
    }
}
static int readiness_probe_until(const char* local_address,struct pd_readiness* result,int64_t deadline)
{
    memset(result,0,sizeof(*result));struct in_addr address;char canonical[16];
    if(inet_pton(AF_INET,local_address,&address)!=1 || !inet_ntop(AF_INET,&address,canonical,sizeof(canonical)) || strcmp(canonical,local_address)){return 0;}
    uint32_t ip=ntohl(address.s_addr);
    if(!((ip&0xff000000u)==0x0a000000u || (ip&0xfff00000u)==0xac100000u || (ip&0xffff0000u)==0xc0a80000u) || (ip&0xffff0000u)==0xac1e0000u){return 0;}
    struct ifaddrs* interfaces=NULL;if(getifaddrs(&interfaces)){return 0;}int local=0;
    for(struct ifaddrs* p=interfaces;p;p=p->ifa_next){
        if(p->ifa_addr && p->ifa_addr->sa_family==AF_INET && ((struct sockaddr_in*)p->ifa_addr)->sin_addr.s_addr==address.s_addr){local=1;}
    }
    freeifaddrs(interfaces);if(!local){return 0;}
    int fd=socket(AF_INET,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);if(fd<0){return 0;}
    int64_t now=ready_now();int ok=now>=0 && now<deadline;
    struct sockaddr_in peer={.sin_family=AF_INET,.sin_port=htons(48992),.sin_addr=address};
    if(ok && connect(fd,(struct sockaddr*)&peer,sizeof(peer))){ok=errno==EINPROGRESS;}
    int error=0;socklen_t length=sizeof(error);
    ok=ok && ready_poll(fd,POLLOUT,deadline) && !getsockopt(fd,SOL_SOCKET,SO_ERROR,&error,&length) && !error;
    char request[160];int count=snprintf(request,sizeof(request),"GET /v1/status HTTP/1.0\r\nHost: %s:48992\r\nConnection: close\r\n\r\n",canonical);
    size_t sent=0;ok=ok && count>0 && (size_t)count<sizeof(request);
    while(ok && sent<(size_t)count){
        if(!ready_poll(fd,POLLOUT,deadline)){ok=0;break;}
        ssize_t n=send(fd,request+sent,(size_t)count-sent,MSG_NOSIGNAL);
        if(n<0 && (errno==EINTR || errno==EAGAIN)){continue;}if(n<=0){ok=0;break;}sent+=(size_t)n;
    }
    unsigned char response[PD_READINESS_HTTP_MAX];size_t used=0;int eof=0;
    while(ok && !eof){
        if(used==sizeof(response) || !ready_poll(fd,POLLIN,deadline)){ok=0;break;}
        ssize_t n=recv(fd,response+used,sizeof(response)-used,0);
        if(n<0 && (errno==EINTR || errno==EAGAIN)){continue;}if(n<0){ok=0;break;}
        if(!n){eof=1;}else{used+=(size_t)n;}
    }
    close(fd);return ok && eof && pd_readiness_http(response,used,result);
}
int pd_readiness_probe(const char* local_address,struct pd_readiness* result)
{
    int64_t now=ready_now();if(now<0){memset(result,0,sizeof(*result));return 0;}
    return readiness_probe_until(local_address,result,now+5000);
}
int pd_readiness_wait(const char* local_address,const struct pd_readiness* expected)
{
    if(!pd_readiness_valid(expected)){return 0;}int64_t start=ready_now();if(start<0){return 0;}
    int64_t deadline=start+15000;
    for(;;){
        int64_t now=ready_now();if(now<start || now>=deadline){return 0;}
        int64_t attempt=now+3000;if(attempt>deadline){attempt=deadline;}
        struct pd_readiness observed;
        if(readiness_probe_until(local_address,&observed,attempt) && pd_readiness_equal(expected,&observed)){return 1;}
        now=ready_now();if(now<start || now>=deadline){return 0;}
        int delay=deadline-now<100?(int)(deadline-now):100;(void)poll(NULL,0,delay);
    }
}
#if defined(PD_MIGRATION_TEST) || defined(PD_READINESS_TEST)
static uid_t ready_owner(void){return getuid();}
#else
static uid_t ready_owner(void){return 0;}
#endif
#ifdef PD_READINESS_FAULTS
extern int pd_readiness_checkpoint(const char* point);
#else
static int pd_readiness_checkpoint(const char* point){(void)point;return 1;}
#endif
static int ready_store(int store)
{
    struct stat st;return store>=0 && !fstat(store,&st) && S_ISDIR(st.st_mode) && st.st_uid==ready_owner() && !(st.st_mode&022);
}
static int ready_file(int fd,unsigned char bytes[PD_READINESS_SIZE],size_t* size)
{
    struct stat st;
    if(fd<0 || fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_uid!=ready_owner() || st.st_nlink!=1 ||
        (st.st_mode&07777)!=0600 || st.st_size<0 || st.st_size>PD_READINESS_SIZE){return 0;}
    *size=(size_t)st.st_size;size_t used=0;
    while(used<*size){ssize_t n=pread(fd,bytes+used,*size-used,(off_t)used);if(n<0 && errno==EINTR){continue;}if(n<=0){return 0;}used+=(size_t)n;}return 1;
}
static void ready_encode(unsigned char bytes[PD_READINESS_SIZE],const unsigned char* plan,const struct pd_readiness* identity)
{
    memcpy(bytes,"PDREADY1",8);memcpy(bytes+8,plan,PD_MIGRATION_SIZE);
    memcpy(bytes+316,identity->hostname,64);memcpy(bytes+380,identity->address,16);memcpy(bytes+396,identity->dns,256);
}
int pd_readiness_read(int store,const unsigned char plan[PD_MIGRATION_SIZE],struct pd_readiness* result)
{
    memset(result,0,sizeof(*result));struct pd_migration parsed;
    if(!ready_store(store) || !pd_migration_parse(plan,PD_MIGRATION_SIZE,&parsed)){return 0;}
    int fd=openat(store,"expected-identity",O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);size_t size=0;unsigned char bytes[PD_READINESS_SIZE];
    int ok=ready_file(fd,bytes,&size) && size==sizeof(bytes) && !memcmp(bytes,"PDREADY1",8) && !memcmp(bytes+8,plan,PD_MIGRATION_SIZE);
    struct stat st;ok=ok && fstatat(store,"expected-identity.new",&st,AT_SYMLINK_NOFOLLOW)<0 && errno==ENOENT;
    if(fd>=0){close(fd);}if(!ok){return 0;}
    struct pd_readiness identity={0};memcpy(identity.hostname,bytes+316,64);memcpy(identity.address,bytes+380,16);memcpy(identity.dns,bytes+396,256);
    if(!pd_readiness_valid(&identity)){return 0;}*result=identity;return 1;
}
int pd_readiness_prepare(int store,const unsigned char plan[PD_MIGRATION_SIZE],const struct pd_readiness* expected)
{
    struct pd_migration parsed;if(!ready_store(store) || !pd_readiness_valid(expected) || !pd_migration_parse(plan,PD_MIGRATION_SIZE,&parsed)){return 0;}
    unsigned char bytes[PD_READINESS_SIZE],prior[PD_READINESS_SIZE];ready_encode(bytes,plan,expected);size_t size=0;
    int fd=openat(store,"expected-identity",O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    if(fd>=0){
        struct stat st;int ok=ready_file(fd,prior,&size) && size==sizeof(bytes) && !memcmp(bytes,prior,size) &&
            fstatat(store,"expected-identity.new",&st,AT_SYMLINK_NOFOLLOW)<0 && errno==ENOENT;
        close(fd);return ok && !fsync(store);
    }
    if(errno!=ENOENT){return 0;}
    fd=openat(store,"expected-identity.new",O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    if(fd>=0){
        int ok=ready_file(fd,prior,&size) && !memcmp(bytes,prior,size);close(fd);
        if(!ok || unlinkat(store,"expected-identity.new",0)){return 0;}
    }else if(errno!=ENOENT){return 0;}
    fd=openat(store,"expected-identity.new",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if(fd<0){return 0;}size_t used=0;int ok=1;
    while(used<sizeof(bytes)){
        ssize_t n=write(fd,bytes+used,sizeof(bytes)-used);if(n<0 && errno==EINTR){continue;}if(n<=0){ok=0;break;}used+=(size_t)n;
    }
    ok=ok && !fsync(fd);if(close(fd)){ok=0;}
    return ok && pd_readiness_checkpoint("identity-file") && !renameat(store,"expected-identity.new",store,"expected-identity") &&
        pd_readiness_checkpoint("identity-rename") && !fsync(store);
}
