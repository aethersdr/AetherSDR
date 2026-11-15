#define _GNU_SOURCE
#include "pd_primary.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>

enum { kMetadataMax=16384,kTcpMax=65536,kProcMagic=0x9fa0 };
static int primary_text(const unsigned char* bytes,size_t size,char* text,size_t capacity)
{
    if(!size || size>=capacity || bytes[size-1]!='\n' || memchr(bytes,0,size)){return 0;}
    memcpy(text,bytes,size);text[size]=0;return 1;
}
static char* primary_trim(char* text)
{
    while(*text==' ' || *text=='\t'){++text;}size_t size=strlen(text);
    while(size && (text[size-1]==' ' || text[size-1]=='\t')){text[--size]=0;}return text;
}
static int primary_number(const char* text,unsigned base,uint64_t limit,uint64_t* result)
{
    if(!*text){return 0;}uint64_t value=0;
    for(;*text;++text){
        unsigned digit;unsigned char c=(unsigned char)*text;
        if(c>='0' && c<='9'){digit=c-'0';}else if(c>='a' && c<='f'){digit=c-'a'+10;}
        else if(c>='A' && c<='F'){digit=c-'A'+10;}else{return 0;}
        if(digit>=base || digit>limit || value>(limit-digit)/base){return 0;}value=value*base+digit;
    }
    *result=value;return 1;
}
static int primary_ids(char* value,uint64_t expected)
{
    char* state=NULL;unsigned count=0;
    for(char* word=strtok_r(value," \t",&state);word;word=strtok_r(NULL," \t",&state)){
        uint64_t id;if(!primary_number(word,10,UINT32_MAX,&id) || id!=expected || ++count>4){return 0;}
    }
    return count==4;
}
int pd_primary_status(const unsigned char* bytes,size_t size,pid_t pid,unsigned features)
{
    char text[kMetadataMax];if(pid<2 || features>3 || !primary_text(bytes,size,text,sizeof(text))){return 0;}
    unsigned seen=0;char* state=NULL;
    for(char* line=strtok_r(text,"\n",&state);line;line=strtok_r(NULL,"\n",&state)){
        char* colon=strchr(line,':');if(!colon){return 0;}*colon=0;char* value=primary_trim(colon+1);
        unsigned bit=0;uint64_t number=0;int ok=1;
        if(!strcmp(line,"Uid")){bit=1;ok=primary_ids(value,78666);}
        else if(!strcmp(line,"Gid")){bit=2;ok=primary_ids(value,20);}
        else if(!strcmp(line,"Groups")){bit=4;ok=!*value;}
        else if(!strcmp(line,"Tgid")){bit=8;ok=primary_number(value,10,4194304,&number) && number==(uint64_t)pid;}
        else if(!strcmp(line,"Pid")){bit=16;ok=primary_number(value,10,4194304,&number) && number==(uint64_t)pid;}
        else if(!strcmp(line,"TracerPid")){bit=32;ok=!strcmp(value,"0");}
        else if(!strcmp(line,"NoNewPrivs")){bit=64;ok=!strcmp(value,"1");}
        else if(!strcmp(line,"CapInh")){bit=128;}
        else if(!strcmp(line,"CapPrm")){bit=256;}
        else if(!strcmp(line,"CapEff")){bit=512;}
        else if(!strcmp(line,"CapBnd")){bit=1024;}
        else if(!strcmp(line,"CapAmb")){bit=2048;}
        if(bit>=128){size_t n=strlen(value);ok=(n==8 || n==16) && primary_number(value,16,UINT64_MAX,&number) && number==0;}
        if(!ok || (seen&bit)){return 0;}seen|=bit;
    }
    unsigned required=1|2|4|8|16|32|128|256|512|1024;
    if(features&1){required|=64;}if(features&2){required|=2048;}
    return (seen&required)==required;
}
int pd_primary_stat(const unsigned char* bytes,size_t size,pid_t pid,uint64_t* started)
{
    *started=0;char text[4096];if(!primary_text(bytes,size,text,sizeof(text))){return 0;}
    char* open=strchr(text,'(');char* close=strrchr(text,')');uint64_t number=0;
    if(!open || !close || open<=text || open[-1]!=' ' || close<open || close[1]!=' '){return 0;}
    open[-1]=0;if(!primary_number(text,10,4194304,&number) || number!=(uint64_t)pid){return 0;}
    char* state=NULL;unsigned field=3;uint64_t tick=0;
    for(char* word=strtok_r(close+2," \n",&state);word;word=strtok_r(NULL," \n",&state),++field){
        if(field==3 && (strlen(word)!=1 || !strchr("RSD",*word))){return 0;}
        if(field==19 && strcmp(word,"10")){return 0;}
        if(field==22 && (!primary_number(word,10,UINT64_MAX,&tick) || !tick)){return 0;}
        if(field==41 && strcmp(word,"0")){return 0;}
    }
    if(field<=41 || !tick){return 0;}*started=tick;return 1;
}
int pd_primary_cgroup(const unsigned char* bytes,size_t size)
{
    char text[kMetadataMax];if(!primary_text(bytes,size,text,sizeof(text))){return 0;}
    unsigned found=0;char* state=NULL;
    for(char* line=strtok_r(text,"\n",&state);line;line=strtok_r(NULL,"\n",&state)){
        char* first=strchr(line,':');char* second=first?strchr(first+1,':'):NULL;uint64_t hierarchy=0;
        if(!first || !second){return 0;}*first=*second=0;
        if(!primary_number(line,10,UINT32_MAX,&hierarchy)){return 0;}
        if(!strcmp(first+1,"name=systemd") || (!hierarchy && !first[1])){
            if(++found>1 || (strcmp(second+1,"/system.slice/aetherpd-shim.service") &&
                strcmp(second+1,"/system/aetherpd-shim.service"))){return 0;}
        }
    }
    return found==1;
}
static int primary_zero(const char* bytes,size_t size)
{
    for(size_t i=0;i<size;++i){if(bytes[i]!='0'){return 0;}}return 1;
}
int pd_primary_tcp(const unsigned char* bytes,size_t size,int ipv6,uint64_t* inode)
{
    char text[kTcpMax];if((ipv6!=0 && ipv6!=1) || !primary_text(bytes,size,text,sizeof(text))){return 0;}
    char* state=NULL;char* line=strtok_r(text,"\n",&state);
    if(!line || !strstr(line,"local_address") || !strstr(line,"inode")){return 0;}
    while((line=strtok_r(NULL,"\n",&state))){
        char* fields[10];char* field_state=NULL;unsigned count=0;
        for(char* p=strtok_r(line," \t",&field_state);p && count<10;p=strtok_r(NULL," \t",&field_state)){fields[count++]=p;}
        if(count<4){return 0;}size_t address=ipv6?32:8;uint64_t port=0;
        if(strlen(fields[1])!=address+5 || fields[1][address]!=':' ||
            !primary_number(fields[1]+address+1,16,65535,&port)){return 0;}
        if(port!=48992 || strcmp(fields[3],"0A")){continue;}
        uint64_t owner=0,observed=0;
        if(*inode || count<10 || !primary_zero(fields[1],address) ||
            strlen(fields[2])!=address+5 || fields[2][address]!=':' ||
            !primary_zero(fields[2],address) || strcmp(fields[2]+address+1,"0000") ||
            !primary_number(fields[7],10,UINT32_MAX,&owner) || owner!=78666 ||
            !primary_number(fields[9],10,UINT64_MAX,&observed) || !observed){return 0;}
        *inode=observed;
    }
    return 1;
}
static int primary_read(int directory,const char* name,unsigned char* bytes,size_t capacity,size_t* size)
{
    *size=0;int fd=openat(directory,name,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);if(fd<0){return 0;}int ok=1;
    while(*size<capacity){
        ssize_t n=read(fd,bytes+*size,capacity-*size);if(n<0 && errno==EINTR){continue;}if(n<0){ok=0;break;}if(!n){break;}*size+=(size_t)n;
    }
    close(fd);return ok && *size<capacity;
}
static int primary_program(int fd,struct stat* st)
{
    return fd>=0 && !fstat(fd,st) && S_ISREG(st->st_mode) && st->st_uid==0 && st->st_nlink==1 &&
        !(st->st_mode&06022) && (st->st_mode&0005)==0005 && st->st_size>0 && st->st_size<=48*1024*1024;
}
static int primary_namespaces(int process)
{
    const char* const names[]={"ns/net","ns/pid"};
    int init=open("/proc/1",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(init<0){return 0;}int ok=1;
    for(unsigned i=0;ok && i<2;++i){
        struct stat a,b;ok=!fstatat(init,names[i],&a,0) && !fstatat(process,names[i],&b,0) && a.st_dev==b.st_dev && a.st_ino==b.st_ino;
    }
    close(init);return ok;
}
static int primary_owns_socket(int process,uint64_t inode)
{
    int fd=openat(process,"fd",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(fd<0){return 0;}
    DIR* directory=fdopendir(fd);if(!directory){close(fd);return 0;}
    char expected[64];int length=snprintf(expected,sizeof(expected),"socket:[%llu]",(unsigned long long)inode);
    int found=0,ok=length>0 && (size_t)length<sizeof(expected);unsigned count=0;struct dirent* entry;
    while(ok){
        errno=0;entry=readdir(directory);if(!entry){if(errno){ok=0;}break;}
        if(!strcmp(entry->d_name,".") || !strcmp(entry->d_name,"..")){continue;}
        uint64_t number=0;if(++count>8192 || !primary_number(entry->d_name,10,INT_MAX,&number)){ok=0;break;}
        char link[128];ssize_t n=readlinkat(fd,entry->d_name,link,sizeof(link));
        if(n<0){if(errno==ENOENT){continue;}ok=0;break;}
        if(n==length && !memcmp(link,expected,(size_t)n)){found=1;}
    }
    closedir(directory);return ok && found;
}
static int primary_snapshot(const struct pd_primary* primary,uint64_t* started,uint64_t* listener)
{
    struct stat expected,running;struct statfs fs;
    if(primary->process<0 || fstatfs(primary->process,&fs) || fs.f_type!=kProcMagic ||
        !primary_program(primary->executable,&expected) || !primary_namespaces(primary->process)){return 0;}
    int exe=openat(primary->process,"exe",O_RDONLY|O_CLOEXEC);
    int ok=primary_program(exe,&running) && expected.st_dev==running.st_dev && expected.st_ino==running.st_ino;
    if(exe>=0){close(exe);}if(!ok){return 0;}
    unsigned char bytes[kTcpMax];size_t size=0;uint64_t before=0,after=0;
    if(!primary_read(primary->process,"stat",bytes,4096,&size) || !pd_primary_stat(bytes,size,primary->pid,&before) ||
        !primary_read(primary->process,"status",bytes,kMetadataMax,&size) || !pd_primary_status(bytes,size,primary->pid,(unsigned)primary->features) ||
        !primary_read(primary->process,"cgroup",bytes,kMetadataMax,&size) || !pd_primary_cgroup(bytes,size)){return 0;}
    int net=openat(primary->process,"net",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(net<0){return 0;}
    uint64_t inode=0;
    ok=primary_read(net,"tcp",bytes,sizeof(bytes),&size) && pd_primary_tcp(bytes,size,0,&inode) &&
        primary_read(net,"tcp6",bytes,sizeof(bytes),&size) && pd_primary_tcp(bytes,size,1,&inode) && inode &&
        primary_owns_socket(primary->process,inode);
    close(net);
    ok=ok && primary_read(primary->process,"stat",bytes,4096,&size) && pd_primary_stat(bytes,size,primary->pid,&after) && before==after;
    if(ok){*started=before;*listener=inode;}return ok;
}
void pd_primary_close(struct pd_primary* primary)
{
    if(primary->process>=0){close(primary->process);}if(primary->executable>=0){close(primary->executable);}
    primary->process=primary->executable=-1;
}
int pd_primary_open(pid_t pid,int executable,struct pd_primary* primary)
{
    memset(primary,0,sizeof(*primary));primary->process=primary->executable=-1;primary->pid=pid;
    if(getuid()!=0 || geteuid()!=0 || pid<2 || pid>4194304){return 0;}
    int value=prctl(PR_GET_NO_NEW_PRIVS,0,0,0,0);
    if(value>=0){primary->features|=1;}else if(errno!=EINVAL){return 0;}
    value=prctl(PR_CAP_AMBIENT,PR_CAP_AMBIENT_IS_SET,0,0,0);
    if(value>=0){primary->features|=2;}else if(errno!=EINVAL){return 0;}
    char name[32];snprintf(name,sizeof(name),"/proc/%ld",(long)pid);
    primary->process=open(name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    primary->executable=fcntl(executable,F_DUPFD_CLOEXEC,3);
    if(!primary_snapshot(primary,&primary->started,&primary->listener)){pd_primary_close(primary);return 0;}
    return 1;
}
int pd_primary_check(const struct pd_primary* primary)
{
    uint64_t started=0,listener=0;
    return primary_snapshot(primary,&started,&listener) && started==primary->started && listener==primary->listener;
}
int pd_primary_wait(const struct pd_primary* primary,const char* address,const struct pd_readiness* expected)
{
    return pd_primary_check(primary) && pd_readiness_wait(address,expected) && pd_primary_check(primary);
}
