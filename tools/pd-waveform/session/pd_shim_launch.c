#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <linux/capability.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/un.h>
#include <unistd.h>

/* Development 8600 handoff only. The trusted provider prepares resources;
 * this process validates fixed roles and retires root before running Go. */
static const char kShim[]="flex-tailnet-shim";
static int launch_fd(const char* text)
{
    if(!text || !*text || strlen(text)>4){return -1;}
    unsigned n=0;for(const char* p=text;*p;++p){if(*p<'0' || *p>'9'){return -1;}n=n*10+(unsigned)(*p-'0');}
    return n>=3 && n<=1023 ? (int)n : -1;
}
static int launch_handle(const char* text,uint32_t* value)
{
    if(!text || strlen(text)!=10 || text[0]!='0' || text[1]!='x'){return 0;}
    uint32_t n=0;for(unsigned i=2;i<10;++i){
        unsigned digit;
        if(text[i]>='0' && text[i]<='9'){digit=(unsigned)(text[i]-'0');}
        else if(text[i]>='a' && text[i]<='f'){digit=(unsigned)(text[i]-'a'+10);}
        else if(text[i]>='A' && text[i]<='F'){digit=(unsigned)(text[i]-'A'+10);}
        else{return 0;}
        n=(n<<4)|digit;
    }
    *value=n;return n!=0;
}
static int protected_directory(const struct stat* st)
{
    return S_ISDIR(st->st_mode) && st->st_uid==0 && !(st->st_mode&022);
}
static int protected_program(const struct stat* st)
{
    return S_ISREG(st->st_mode) && st->st_uid==0 && st->st_nlink==1 &&
        !(st->st_mode&(022|06000)) && (st->st_mode&0005)==0005;
}
static int data_directory(const struct stat* st)
{
    return S_ISDIR(st->st_mode) && st->st_uid==78666 && st->st_gid==20 && (st->st_mode&07777)==0700;
}
static int private_datagram(int fd)
{
    int type=0;socklen_t n=sizeof(type);struct sockaddr_un address;struct stat st;
    if(fstat(fd,&st) || !S_ISSOCK(st.st_mode) || getsockopt(fd,SOL_SOCKET,SO_TYPE,&type,&n) || type!=SOCK_DGRAM){return 0;}
    memset(&address,0,sizeof(address));n=sizeof(address);
    if(getsockname(fd,(struct sockaddr*)&address,&n) || address.sun_family!=AF_UNIX || n!=offsetof(struct sockaddr_un,sun_path)){return 0;}
    memset(&address,0,sizeof(address));n=sizeof(address);
    return !getpeername(fd,(struct sockaddr*)&address,&n) && address.sun_family==AF_UNIX && n==offsetof(struct sockaddr_un,sun_path);
}
static int local_api(int fd)
{
    int type=0;socklen_t n=sizeof(type);struct sockaddr_in address;struct stat st;
    if(fstat(fd,&st) || !S_ISSOCK(st.st_mode) || getsockopt(fd,SOL_SOCKET,SO_TYPE,&type,&n) || type!=SOCK_STREAM){return 0;}
    type=0;n=sizeof(type);
    if(getsockopt(fd,SOL_SOCKET,SO_PROTOCOL,&type,&n) || type!=IPPROTO_TCP){return 0;}
    memset(&address,0,sizeof(address));n=sizeof(address);
    return !getpeername(fd,(struct sockaddr*)&address,&n) && n==sizeof(address) &&
        address.sin_family==AF_INET && address.sin_addr.s_addr==htonl(INADDR_LOOPBACK) && ntohs(address.sin_port)==4992;
}
static int pipe_role(int fd,int access,struct stat* st)
{
    int flags=fcntl(fd,F_GETFL);
    return flags>=0 && (flags&O_ACCMODE)==access && !fstat(fd,st) && S_ISFIFO(st->st_mode);
}
static int handoff_roles(const int fds[6])
{
    for(unsigned i=0;i<6;++i){for(unsigned j=0;j<i;++j){if(fds[i]==fds[j]){return 0;}}}
    struct stat life,setup,read,api,unkey;
    return private_datagram(fds[0]) && local_api(fds[1]) &&
        pipe_role(fds[2],O_WRONLY,&life) && pipe_role(fds[3],O_WRONLY,&setup) &&
        local_api(fds[4]) && pipe_role(fds[5],O_RDONLY,&read) &&
        !fstat(fds[1],&api) && !fstat(fds[4],&unkey) &&
        (api.st_dev!=unkey.st_dev || api.st_ino!=unkey.st_ino) &&
        life.st_dev==read.st_dev && life.st_ino==read.st_ino &&
        (life.st_dev!=setup.st_dev || life.st_ino!=setup.st_ino);
}
static int null_input(int flags,const struct stat* input)
{
    return flags>=0 && (flags&O_ACCMODE)!=O_WRONLY && S_ISCHR(input->st_mode) &&
        input->st_rdev==makedev(1,3);
}
static int standard_streams(void)
{
    struct stat input,output,error;
    int flags=fcntl(0,F_GETFL);
    return !fstat(0,&input) && null_input(flags,&input) &&
        pipe_role(1,O_WRONLY,&output) && pipe_role(2,O_WRONLY,&error);
}
static int executable_directory(char path[PATH_MAX])
{
#ifdef PD_FIXED_RUNTIME_DIRECTORY
    strcpy(path,"/run/aetherpd/bin");
#else
    ssize_t n=readlink("/proc/self/exe",path,PATH_MAX-1);
    if(n<=0 || n>=PATH_MAX-1 || path[0]!='/'){return -1;}path[n]=0;
    char* last=strrchr(path,'/');if(!last || last==path){return -1;}*last=0;
#endif
    int current=open("/",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(current<0){return -1;}
    struct stat st;if(fstat(current,&st) || !protected_directory(&st)){close(current);return -1;}
    const char* part=path+1;
    while(*part){
        const char* end=strchr(part,'/');size_t length=end?(size_t)(end-part):strlen(part);
        if(!length || length>NAME_MAX || (length==1 && part[0]=='.') || (length==2 && !memcmp(part,"..",2))){close(current);return -1;}
        char name[NAME_MAX+1];memcpy(name,part,length);name[length]=0;
        int next=openat(current,name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);close(current);
        if(next<0){return -1;}current=next;
        if(fstat(current,&st) || !protected_directory(&st)){close(current);return -1;}
        part=end?end+1:part+length;
    }
    return current;
}
static int open_program(int directory,const char* name)
{
    int fd=openat(directory,name,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);struct stat st;unsigned char magic[4];
    if(fd<0){return -1;}
    ssize_t n=pread(fd,magic,sizeof(magic),0);
    if(fstat(fd,&st) || !protected_program(&st) || n!=4 || memcmp(magic,"\177ELF",4)){close(fd);return -1;}
    return fd;
}
static int check_data(int directory,const char* name)
{
    int fd=openat(directory,name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(fd<0){return 0;}
    struct stat st;int valid=!fstat(fd,&st) && data_directory(&st);close(fd);return valid;
}
static int close_unrelated(const int fds[6],int program)
{
    DIR* d=opendir("/proc/self/fd");if(!d){return 0;}int own=dirfd(d);struct dirent* e;
    while((e=readdir(d))){
        if(e->d_name[0]=='.'){continue;}char* end;errno=0;long fd=strtol(e->d_name,&end,10);
        if(errno || *end || fd<0 || fd>INT_MAX){closedir(d);return 0;}
        int keep=fd<=2 || fd==program || fd==own;
        for(unsigned i=0;i<6;++i){keep|=fd==fds[i];}
        if(!keep){close((int)fd);}
    }
    return !closedir(d);
}
static int drop_root(void)
{
    for(int cap=0;cap<64;++cap){if(prctl(PR_CAPBSET_DROP,cap,0,0,0) && errno!=EINVAL){return 0;}}
    if(setgroups(0,NULL) || setresgid(20,20,20) || setresuid(78666,78666,78666)){return 0;}
    struct __user_cap_header_struct header={_LINUX_CAPABILITY_VERSION_3,0};struct __user_cap_data_struct caps[2]={{0}};
    if(syscall(SYS_capset,&header,caps) || syscall(SYS_capget,&header,caps) || prctl(PR_SET_DUMPABLE,0,0,0,0)){return 0;}
    for(unsigned i=0;i<2;++i){if(caps[i].effective || caps[i].permitted || caps[i].inheritable){return 0;}}
    return getuid()==78666 && geteuid()==78666 && getgid()==20 && getegid()==20;
}
int main(int argc,char** argv)
{
    if(argc!=7 || geteuid()!=0 || !standard_streams()){return 2;}
    int fds[6];for(unsigned i=0;i<6;++i){fds[i]=launch_fd(argv[i+1]);if(fds[i]<0){return 2;}}
    uint32_t client,gui;
    if(!launch_handle(getenv("SHIM_PD_UNKEY_CLIENT_HANDLE"),&client) ||
       !launch_handle(getenv("SHIM_PD_GUI_HANDLE"),&gui) || client==gui || !handoff_roles(fds)){return 3;}
    char directory_path[PATH_MAX];int directory=executable_directory(directory_path);if(directory<0){return 4;}
    int program=open_program(directory,kShim);
    const char* helpers[]={"aether-pd","pd-unkey-guard","pd-lease-guard"};
    for(unsigned i=0;i<3;++i){int fd=open_program(directory,helpers[i]);if(fd<0){close(directory);if(program>=0){close(program);}return 4;}close(fd);}
    if(program<0 || !check_data(directory,"state") || !check_data(directory,"run")){close(directory);if(program>=0){close(program);}return 4;}
    close(directory);
    char state[PATH_MAX],run[PATH_MAX];int s=snprintf(state,sizeof(state),"%s/state",directory_path),r=snprintf(run,sizeof(run),"%s/run",directory_path);
    if(s<=0 || s>=(int)sizeof(state) || r<=0 || r>=(int)sizeof(run)){close(program);return 4;}
    const char* keys[]={"SHIM_PD_IQ_FD","SHIM_PD_API_FD","SHIM_PD_GUARD_FD","SHIM_PD_SETUP_FD","SHIM_PD_UNKEY_API_FD","SHIM_PD_GUARD_READ_FD"};
    char entries[8][96];char* env[13]={"PATH=/usr/bin:/bin","SHIM_PD_OWN_LEASE_GUARD=1","SHIM_PD_TRANSPORT_CACHE=1"};
    for(unsigned i=0;i<6;++i){snprintf(entries[i],sizeof(entries[i]),"%s=%d",keys[i],fds[i]);env[3+i]=entries[i];}
    snprintf(entries[6],sizeof(entries[6]),"SHIM_PD_UNKEY_CLIENT_HANDLE=0x%08x",client);env[9]=entries[6];
    snprintf(entries[7],sizeof(entries[7]),"SHIM_PD_GUI_HANDLE=0x%08x",gui);env[10]=entries[7];
    if(!close_unrelated(fds,program) || !drop_root()){close(program);return 5;}
    for(unsigned i=0;i<6;++i){int flags=fcntl(fds[i],F_GETFD);if(flags<0 || fcntl(fds[i],F_SETFD,flags&~FD_CLOEXEC)){close(program);return 5;}}
    char* args[]={"flex-tailnet-shim","-state",state,"-run-dir",run,"-radio","127.0.0.1","-api","192.168.50.100:48997",NULL};
    fexecve(program,args,env);close(program);return 6;
}
