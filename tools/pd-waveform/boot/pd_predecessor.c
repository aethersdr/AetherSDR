#define _GNU_SOURCE
#include "pd_predecessor.h"
#include "pd_recovery.h"
#include "pd_state_transfer.h"
#include "pd_store.h"
#include "pd_transaction.h"
#include "monocypher.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/xattr.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum { kInventoryMax=8192,kFields=29,kExecutableMax=67108864 };
#ifdef PD_PREDECESSOR_TEST
extern int pd_predecessor_test_command(char* const*,unsigned char*,size_t*,int);
extern int pd_predecessor_test_root(void);
extern int pd_predecessor_test_process_root(pid_t);
extern int pd_predecessor_test_quiet(void);
static uid_t predecessor_owner(void){return getuid();}
static int predecessor_privileged(void){return 1;}
static int predecessor_host_root(void){return pd_predecessor_test_root();}
#else
static uid_t predecessor_owner(void){return 0;}
static int predecessor_privileged(void){return getuid()==0 && geteuid()==0;}
static int predecessor_host_root(void){return open("/",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);}
#endif
static const char kFormat[]=
    "{{.Id}}\n{{.Image}}\n{{printf \"%x\" .Name}}\n"
    "{{printf \"%x\" .Config.User}}\n{{printf \"%x\" .Config.Entrypoint}}\n"
    "{{json .Config.Cmd}}\n{{printf \"%x\" .Config.WorkingDir}}\n"
    "{{printf \"%x\" (index .Config.Labels \"com.flexradio.waveform.name\")}}\n"
    "{{printf \"%x\" (index .Config.Labels \"com.flexradio.waveform.version\")}}\n"
    "{{len .Config.Labels}}\n{{printf \"%x\" .HostConfig.NetworkMode}}\n"
    "{{printf \"%x\" .HostConfig.PidMode}}\n{{printf \"%x\" .HostConfig.IpcMode}}\n"
    "{{.HostConfig.Privileged}}\n{{.HostConfig.ReadonlyRootfs}}\n"
    "{{json .HostConfig.Binds}}\n{{json .HostConfig.CapAdd}}\n"
    "{{json .HostConfig.CapDrop}}\n{{json .HostConfig.Devices}}\n"
    "{{json .HostConfig.PortBindings}}\n{{json .HostConfig.SecurityOpt}}\n"
    "{{if $restored}}RESTORED{{else}}{{json .Mounts}}{{end}}\n{{printf \"%x\" .HostConfig.RestartPolicy.Name}}\n"
    "{{.HostConfig.RestartPolicy.MaximumRetryCount}}\n{{.State.Running}}\n"
    "{{.State.Pid}}\n{{len .Config.Env}}\nENV\n"
    "{{range .Config.Env}}{{printf \"%x\" .}}\n{{end}}END";
static const char kRestoredFields[]=
    "\n{{printf \"%x\" .Config.Hostname}}\n{{len .Mounts}}"
    "{{range .Mounts}}\n{{printf \"%x\" .Type}}\n{{printf \"%x\" .Source}}"
    "\n{{printf \"%x\" .Destination}}\n{{printf \"%x\" .Driver}}\n{{printf \"%x\" .Mode}}"
    "\n{{json .Options}}\n{{.RW}}\n{{printf \"%x\" .Propagation}}{{end}}";
int pd_predecessor_format(int restored,char* bytes,size_t capacity)
{
    int n=snprintf(bytes,capacity,"%s%s%s",restored?"{{$restored := true}}PDREST01\n":
        "{{$restored := false}}PDLEG001\n",kFormat,restored?kRestoredFields:"");
    return n>=0 && (size_t)n<capacity;
}

static int predecessor_hex(unsigned char c)
{
    if(c>='0' && c<='9'){return c-'0';}
    if(c>='a' && c<='f'){return c-'a'+10;}return -1;
}
static int predecessor_id(const char* value)
{
    if(strlen(value)!=64){return 0;}
    for(unsigned i=0;i<64;++i){if(predecessor_hex((unsigned char)value[i])<0){return 0;}}
    return 1;
}
static int predecessor_text(const char* hex,const char* value)
{
    size_t size=strlen(value);if(strlen(hex)!=2*size){return 0;}
    for(size_t i=0;i<size;++i){
        int a=predecessor_hex((unsigned char)hex[2*i]),b=predecessor_hex((unsigned char)hex[2*i+1]);
        if(a<0 || b<0 || (unsigned char)value[i]!=(unsigned char)(16*a+b)){return 0;}
    }
    return 1;
}
static int predecessor_env(const char* hex,unsigned* seen,struct pd_predecessor* result)
{
    char value[256];size_t size=strlen(hex);
    if(size%2 || size/2>=sizeof(value)){return 0;}
    for(size_t i=0;i<size/2;++i){
        int a=predecessor_hex((unsigned char)hex[2*i]),b=predecessor_hex((unsigned char)hex[2*i+1]);
        if(a<0 || b<0 || 16*a+b<32 || 16*a+b>126){return 0;}value[i]=(char)(16*a+b);
    }
    value[size/2]=0;unsigned bit=0;
    if(!strcmp(value,"PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin")){bit=1;}
    else if(!strcmp(value,"TERM=xterm")){bit=2;}
    else if(!strcmp(value,"container=podman")){bit=4;}
    else if(!strncmp(value,"HOSTNAME=",9)){
        size_t length=strlen(value+9);if(!length || length>64){return 0;}
        for(size_t i=9;value[i];++i){
            unsigned char c=(unsigned char)value[i];
            if(!((c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='-' || c=='.')){return 0;}
        }
        memcpy(result->hostname,value+9,length+1);
        bit=8;
    }else if(!strncmp(value,"SSDR_RADIO_ADDRESS=",19)){
        const char* address=value+19;struct in_addr parsed;char canonical[INET_ADDRSTRLEN];
        if(inet_pton(AF_INET,address,&parsed)!=1 || !inet_ntop(AF_INET,&parsed,canonical,sizeof(canonical))){return 0;}
        if(strcmp(canonical,address)){return 0;}memcpy(result->radio_address,canonical,strlen(canonical)+1);bit=16;
    }
    if(!bit || (*seen&bit)){return 0;}*seen|=bit;return 1;
}
static int predecessor_parse(const unsigned char* bytes,size_t size,const struct pd_recovery* recipe,struct pd_predecessor* result)
{
    memset(result,0,sizeof(*result));
    if(!size || size>=kInventoryMax || bytes[size-1]!='\n' || memchr(bytes,0,size)){return 0;}
    char text[kInventoryMax];memcpy(text,bytes,size);text[size]=0;
    char* lines[kFields+16];size_t count=0;char* start=text;
    for(size_t i=0;i<size;++i){
        if(text[i]!='\n'){continue;}if(count>=sizeof(lines)/sizeof(lines[0])){return 0;}
        text[i]=0;lines[count++]=start;start=text+i+1;
    }
    if(count!=(size_t)(kFields+(recipe?16:6)) || strcmp(lines[0],recipe?"PDREST01":"PDLEG001") ||
        !predecessor_id(lines[1]) || !predecessor_id(lines[2])){return 0;}
    if(recipe){
        if(strcmp(lines[2],recipe->migration.predecessor.image) || !strcmp(lines[1],recipe->migration.predecessor.container) ||
            strcmp(lines[16],"[\"/var/lib/smartsdr/aetherpd/legacy/state:/var/lib/flex-tailnet-shim:rw,rprivate,nosuid,rbind\"]") ||
            strcmp(lines[22],"RESTORED") || !predecessor_text(lines[35],recipe->hostname) || strcmp(lines[36],"1") ||
            !predecessor_text(lines[37],"bind") || !predecessor_text(lines[38],"/var/lib/smartsdr/aetherpd/legacy/state") ||
            !predecessor_text(lines[39],"/var/lib/flex-tailnet-shim") || !predecessor_text(lines[40],"") ||
            !predecessor_text(lines[41],"") || strcmp(lines[42],"[\"nosuid\",\"rbind\"]") ||
            strcmp(lines[43],"true") || !predecessor_text(lines[44],"rprivate")){return 0;}
    }else if(strcmp(lines[16],"[]") || strcmp(lines[22],"[]")){return 0;}
    if(!predecessor_text(lines[3],"flex-tailnet-shim") || !predecessor_text(lines[4],"") ||
        !predecessor_text(lines[5],"/flex-tailnet-shim") || strcmp(lines[6],"null") ||
        !predecessor_text(lines[7],"/") || !predecessor_text(lines[8],"flex-tailnet-shim") ||
        !predecessor_text(lines[9],"0.5.2") || strcmp(lines[10],"2") ||
        !predecessor_text(lines[11],"host") || !predecessor_text(lines[12],"private") ||
        !predecessor_text(lines[13],"shareable") || strcmp(lines[14],"false") || strcmp(lines[15],"false") ||
        strcmp(lines[17],"[]") ||
        strcmp(lines[18],"[\"CAP_AUDIT_WRITE\",\"CAP_MKNOD\",\"CAP_NET_RAW\"]") || strcmp(lines[19],"[]") ||
        strcmp(lines[20],"{}") || strcmp(lines[21],"[]") ||
        !predecessor_text(lines[23],"always") || strcmp(lines[24],"0") ||
        (strcmp(lines[25],"true") && strcmp(lines[25],"false")) ||
        strcmp(lines[27],"5") || strcmp(lines[28],"ENV") || strcmp(lines[34],"END")){return 0;}
    int stopped=!strcmp(lines[25],"false");
    size_t digits=strlen(lines[26]);if(!digits || digits>7 || (lines[26][0]=='0' && strcmp(lines[26],"0"))){return 0;}
    unsigned pid=0;for(size_t i=0;i<digits;++i){
        if(lines[26][i]<'0' || lines[26][i]>'9'){return 0;}pid=10*pid+(unsigned)(lines[26][i]-'0');
    }
    if(stopped?pid!=0:(pid<2 || pid>4194304)){return 0;}
    unsigned seen=0;for(unsigned i=29;i<34;++i){if(!predecessor_env(lines[i],&seen,result)){return 0;}}
    if(seen!=31){return 0;}
    if(recipe && (strcmp(result->hostname,recipe->hostname) || strcmp(result->radio_address,recipe->radio_address))){return 0;}
    memcpy(result->container,lines[1],65);memcpy(result->image,lines[2],65);result->pid=(pid_t)pid;return 1;
}
int pd_predecessor_parse_status(const unsigned char* bytes,size_t size,struct pd_predecessor* result)
{
    return predecessor_parse(bytes,size,NULL,result);
}
int pd_predecessor_parse_restored(const unsigned char* bytes,size_t size,const struct pd_recovery* recipe,struct pd_predecessor* result)
{
    return recipe && predecessor_parse(bytes,size,recipe,result);
}
int pd_predecessor_parse(const unsigned char* bytes,size_t size,struct pd_predecessor* result)
{
    return pd_predecessor_parse_status(bytes,size,result) && result->pid>0;
}
#ifndef PD_PREDECESSOR_TEST
static int64_t predecessor_now(void)
{
    struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t)){return -1;}
    return (int64_t)t.tv_sec*1000+t.tv_nsec/1000000;
}
static int predecessor_command_status(char* const* args,unsigned char bytes[kInventoryMax],size_t* size,int timeout)
{
    int pipes[2];if(pipe2(pipes,O_CLOEXEC)){return -1;}
    pid_t parent=getpid(),child=fork();
    if(child==0){
        if(prctl(PR_SET_PDEATHSIG,SIGKILL) || getppid()!=parent || setpgid(0,0) || dup2(pipes[1],1)<0){_exit(126);}
        int null=open("/dev/null",O_RDWR|O_CLOEXEC);
        if(null<0 || dup2(null,0)<0 || dup2(null,2)<0){_exit(126);}
        long limit=sysconf(_SC_OPEN_MAX);if(limit<0 || limit>1048576){_exit(126);}
        for(int fd=3;fd<limit;++fd){close(fd);}
        char* const env[]={"PATH=/usr/sbin:/usr/bin:/sbin:/bin","LC_ALL=C",NULL};
        execve("/usr/bin/podman",args,env);_exit(127);
    }
    close(pipes[1]);if(child<0){close(pipes[0]);return -1;}
    (void)setpgid(child,child);
    int flags=fcntl(pipes[0],F_GETFL);int ok=flags>=0 && !fcntl(pipes[0],F_SETFL,flags|O_NONBLOCK);
    int64_t now=predecessor_now(),deadline=now+timeout;ok=ok && now>=0;
    size_t used=0;int status=0,done=0,eof=0;
    while(ok && !(done && eof)){
        ssize_t n=read(pipes[0],bytes+used,kInventoryMax-used);
        if(n>0){used+=(size_t)n;if(used==kInventoryMax){ok=0;break;}}
        else if(n==0){eof=1;}
        else if(errno!=EINTR && errno!=EAGAIN){ok=0;break;}
        if(eof && !done){pid_t waited=waitpid(child,&status,WNOHANG);done=waited==child;if(waited<0 && errno!=EINTR){ok=0;break;}}
        if(done && eof){break;}
        now=predecessor_now();if(now<0 || now>=deadline){ok=0;break;}
        if(!(done && eof)){struct pollfd p={pipes[0],POLLIN,0};(void)poll(&p,1,20);}
    }
    close(pipes[0]);
    if(!ok){(void)kill(-child,SIGKILL);if(!done){(void)kill(child,SIGKILL);}}
    if(!done){while(waitpid(child,&status,0)<0 && errno==EINTR){}}
    *size=used;return ok && WIFEXITED(status)?WEXITSTATUS(status):-1;
}
#else
static int predecessor_command_status(char* const* args,unsigned char bytes[kInventoryMax],size_t* size,int timeout)
{
    return pd_predecessor_test_command(args,bytes,size,timeout);
}
#endif
static int predecessor_command(char* const* args,unsigned char bytes[kInventoryMax],size_t* size,int timeout)
{
    return predecessor_command_status(args,bytes,size,timeout)==0;
}
static int predecessor_inventory_kind(unsigned char bytes[kInventoryMax],size_t* size,int restored)
{
    char format[4096];if(!pd_predecessor_format(restored,format,sizeof(format))){return 0;}
    char* const args[]={"/usr/bin/podman","inspect","--format",format,"flex-tailnet-shim",NULL};
    return predecessor_command(args,bytes,size,8000);
}
static int predecessor_inventory(unsigned char bytes[kInventoryMax],size_t* size)
{
    return predecessor_inventory_kind(bytes,size,0);
}
int pd_predecessor_restored_status(const struct pd_recovery* recipe,struct pd_predecessor* result)
{
    unsigned char bytes[kInventoryMax];size_t size=0;
    return predecessor_privileged() && predecessor_inventory_kind(bytes,&size,1) &&
        pd_predecessor_parse_restored(bytes,size,recipe,result);
}
static int predecessor_hash(int fd,unsigned char digest[32])
{
    struct stat before,after;int ok=fd>=0 && !fstat(fd,&before) && S_ISREG(before.st_mode) &&
        before.st_uid==predecessor_owner() && !(before.st_mode&022) && before.st_nlink==1 && before.st_size>0 && before.st_size<=kExecutableMax;
    crypto_blake2b_ctx hash;crypto_blake2b_init(&hash,32);unsigned char bytes[16384];off_t used=0;
    while(ok && used<before.st_size){
        ssize_t n=pread(fd,bytes,sizeof(bytes),used);if(n<0 && errno==EINTR){continue;}
        if(n<=0 || n>before.st_size-used){ok=0;break;}
        crypto_blake2b_update(&hash,bytes,(size_t)n);used+=n;
    }
    ok=ok && !fstat(fd,&after) && before.st_size==after.st_size && before.st_mtime==after.st_mtime && before.st_ctime==after.st_ctime;
    crypto_blake2b_final(&hash,digest);return ok;
}
static int predecessor_root(pid_t pid)
{
#ifdef PD_PREDECESSOR_TEST
    return pd_predecessor_test_process_root(pid);
#else
    char name[80];snprintf(name,sizeof(name),"/proc/%ld",(long)pid);
    int process=open(name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);struct stat owner;
    if(process<0 || fstat(process,&owner) || owner.st_uid!=0){if(process>=0){close(process);}return -1;}
    int root=openat(process,"root",O_RDONLY|O_DIRECTORY|O_CLOEXEC);close(process);return root;
#endif
}
static int predecessor_executable(pid_t pid,unsigned char digest[32])
{
    int root=predecessor_root(pid);if(root<0){return 0;}
    int fd=openat(root,"flex-tailnet-shim",O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);close(root);
#ifdef PD_PREDECESSOR_TEST
    int running=fd>=0?dup(fd):-1;
#else
    char name[80];snprintf(name,sizeof(name),"/proc/%ld/exe",(long)pid);
    int running=open(name,O_RDONLY|O_CLOEXEC);
#endif
    struct stat disk,executing;
    int ok=fd>=0 && running>=0 && !fstat(fd,&disk) && !fstat(running,&executing) &&
        disk.st_dev==executing.st_dev && disk.st_ino==executing.st_ino && predecessor_hash(fd,digest);
    if(running>=0){close(running);}if(fd>=0){close(fd);}return ok;
}
int pd_predecessor_probe(struct pd_predecessor* result)
{
    if(!predecessor_privileged()){return 0;}
    unsigned char first[kInventoryMax],second[kInventoryMax];size_t a=0,b=0;
    if(!predecessor_inventory(first,&a) || !pd_predecessor_parse(first,a,result) ||
        !predecessor_executable(result->pid,result->executable) || !predecessor_inventory(second,&b) ||
        a!=b || memcmp(first,second,a)){memset(result,0,sizeof(*result));return 0;}
    return 1;
}
static int predecessor_same(const struct pd_predecessor* a,const struct pd_predecessor* b)
{
    return !strcmp(a->container,b->container) && !strcmp(a->image,b->image);
}
static int predecessor_status(const struct pd_predecessor* expected,struct pd_predecessor* observed)
{
    unsigned char bytes[kInventoryMax];size_t size=0;
    return predecessor_privileged() && predecessor_id(expected->container) && predecessor_id(expected->image) &&
        predecessor_inventory(bytes,&size) && pd_predecessor_parse_status(bytes,size,observed) && predecessor_same(expected,observed) &&
        (!expected->radio_address[0] || !strcmp(expected->radio_address,observed->radio_address));
}
static int predecessor_root_matches(int root,const struct pd_predecessor* expected)
{
    int fd=openat(root,"flex-tailnet-shim",O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);unsigned char hash[32];
    int ok=fd>=0 && predecessor_hash(fd,hash) && !crypto_verify32(hash,expected->executable);
    if(fd>=0){close(fd);}return ok;
}
static int predecessor_directory(int parent,const char* name)
{
    int fd=openat(parent,name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);struct stat st;
    if(fd>=0 && (fstat(fd,&st) || st.st_uid!=predecessor_owner() || (st.st_mode&022))){close(fd);return -1;}return fd;
}
int pd_predecessor_pin(const struct pd_predecessor* expected,int* root,int* state,int* tsnet)
{
    *root=*state=*tsnet=-1;struct pd_predecessor observed;
    if(!predecessor_status(expected,&observed) || observed.pid!=expected->pid || observed.pid<2){return 0;}
    *root=predecessor_root(expected->pid);if(*root<0){return 0;}
    int var=predecessor_directory(*root,"var"),lib=var>=0?predecessor_directory(var,"lib"):-1;
    *state=lib>=0?predecessor_directory(lib,"flex-tailnet-shim"):-1;
    *tsnet=*state>=0?predecessor_directory(*state,"tsnet"):-1;
    if(var>=0){close(var);}if(lib>=0){close(lib);}
    int ok=*state>=0 && *tsnet>=0 && predecessor_root_matches(*root,expected) &&
        predecessor_status(expected,&observed) && observed.pid==expected->pid;
    if(!ok){if(*tsnet>=0){close(*tsnet);}if(*state>=0){close(*state);}close(*root);*root=*state=*tsnet=-1;}return ok;
}
int pd_predecessor_stopped(const struct pd_predecessor* expected)
{
    struct pd_predecessor observed;return predecessor_status(expected,&observed) && observed.pid==0;
}
int pd_predecessor_stop(const struct pd_predecessor* expected,int root)
{
    struct pd_predecessor observed;
    if(!predecessor_status(expected,&observed) || observed.pid!=expected->pid || observed.pid<2 ||
        !predecessor_root_matches(root,expected)){return 0;}
    char* const args[]={"/usr/bin/podman","stop","--time","10",(char*)expected->container,NULL};
    unsigned char bytes[kInventoryMax];size_t size=0;
    return predecessor_command(args,bytes,&size,15000) && pd_predecessor_stopped(expected);
}
int pd_predecessor_recover(const struct pd_predecessor* expected,int root)
{
    struct pd_predecessor observed;if(!predecessor_status(expected,&observed)){return 0;}
    if(observed.pid==0){
        if(!predecessor_root_matches(root,expected)){return 0;}
        char* const args[]={"/usr/bin/podman","start",(char*)expected->container,NULL};
        unsigned char bytes[kInventoryMax];size_t size=0;
        if(!predecessor_command(args,bytes,&size,15000)){return 0;}
    }
    return pd_predecessor_probe(&observed) && predecessor_same(expected,&observed) &&
        !crypto_verify32(observed.executable,expected->executable);
}
int pd_predecessor_mount_path(const unsigned char* bytes,size_t size,char layer[65])
{
    static const char prefix[]="/var/lib/smartsdr/containers/storage/overlay/";
    static const char suffix[]="/merged\n";
    memset(layer,0,65);
    if(size!=sizeof(prefix)-1+64+sizeof(suffix)-1 || memcmp(bytes,prefix,sizeof(prefix)-1) ||
        memcmp(bytes+sizeof(prefix)-1+64,suffix,sizeof(suffix)-1)){return 0;}
    memcpy(layer,bytes+sizeof(prefix)-1,64);
    if(!predecessor_id(layer)){memset(layer,0,65);return 0;}return 1;
}
static int predecessor_path(const char* const* parts,size_t count)
{
    int fd=predecessor_host_root();struct stat st;
    if(fd<0){return -1;}
    if(fstat(fd,&st) || st.st_uid!=predecessor_owner() || (st.st_mode&022)){close(fd);return -1;}
    for(size_t i=0;i<count;++i){
        int next=predecessor_directory(fd,parts[i]);close(fd);fd=next;if(fd<0){return -1;}
    }
    return fd;
}
static int predecessor_quiet(void)
{
#ifdef PD_PREDECESSOR_TEST
    return pd_predecessor_test_quiet();
#else
    struct stat st;return lstat("/run/aetherpd",&st)<0 && errno==ENOENT;
#endif
}
int pd_predecessor_recover_fresh(const struct pd_predecessor* expected)
{
    struct pd_predecessor observed;
    if(!predecessor_quiet() || !predecessor_status(expected,&observed)){return 0;}
    if(observed.pid>0){
        return pd_predecessor_probe(&observed) && predecessor_same(expected,&observed) &&
            !crypto_verify32(observed.executable,expected->executable);
    }
    if(!pd_predecessor_overlay_verify(expected) || !predecessor_quiet() || !pd_predecessor_stopped(expected)){return 0;}
    unsigned char bytes[kInventoryMax];size_t size=0;
    char* const start_args[]={"/usr/bin/podman","start",(char*)expected->container,NULL};
    return predecessor_command(start_args,bytes,&size,15000) && pd_predecessor_probe(&observed) &&
        predecessor_same(expected,&observed) && !crypto_verify32(observed.executable,expected->executable);
}

static const char kImageFormat[]=
    "PDIMG001\n{{.Id}}\n{{.Architecture}}\n{{.Os}}\n{{.RootFS.Type}}\n{{len .RootFS.Layers}}\n"
    "{{range .RootFS.Layers}}{{.}}\n{{end}}{{.GraphDriver.Name}}\n{{len .GraphDriver.Data}}\n"
    "{{index .GraphDriver.Data \"UpperDir\"}}\n{{index .GraphDriver.Data \"WorkDir\"}}\n{{json .Config}}";
const char* pd_predecessor_image_format(void){return kImageFormat;}
int pd_predecessor_image_parse(const unsigned char* bytes,size_t size,const struct pd_predecessor* expected,char layer[65])
{
    memset(layer,0,65);
    if(!size || size>=kInventoryMax || bytes[size-1]!='\n' || memchr(bytes,0,size) || !predecessor_id(expected->image)){return 0;}
    char text[kInventoryMax];memcpy(text,bytes,size);text[size]=0;char* lines[12];size_t count=0;char* start=text;
    for(size_t i=0;i<size;++i){
        if(text[i]!='\n'){continue;}if(count>=12){return 0;}
        text[i]=0;lines[count++]=start;start=text+i+1;
    }
    if(count!=12 || strcmp(lines[0],"PDIMG001") || strcmp(lines[1],expected->image) ||
        strcmp(lines[2],"arm64") || strcmp(lines[3],"linux") || strcmp(lines[4],"layers") ||
        strcmp(lines[5],"1") || strncmp(lines[6],"sha256:",7) || !predecessor_id(lines[6]+7) ||
        strcmp(lines[7],"overlay") || strcmp(lines[8],"2") ||
        strcmp(lines[11],"{\"Entrypoint\":[\"/flex-tailnet-shim\"],\"Labels\":{\"com.flexradio.waveform.name\":\"flex-tailnet-shim\",\"com.flexradio.waveform.version\":\"0.5.2\"}}")){return 0;}
    char upper[256],work[256];
    snprintf(upper,sizeof(upper),"/var/lib/smartsdr/containers/storage/overlay/%s/diff",lines[6]+7);
    snprintf(work,sizeof(work),"/var/lib/smartsdr/containers/storage/overlay/%s/work",lines[6]+7);
    if(strcmp(lines[9],upper) || strcmp(lines[10],work)){return 0;}
    memcpy(layer,lines[6]+7,65);return 1;
}
static int predecessor_image_layer(const struct pd_predecessor* expected,char layer[65])
{
    if(!predecessor_privileged() || !predecessor_id(expected->image)){return 0;}
    char* const args[]={"/usr/bin/podman","image","inspect","--format",(char*)kImageFormat,(char*)expected->image,NULL};
    unsigned char first[kInventoryMax],second[kInventoryMax];size_t a=0,b=0;
    if(!predecessor_command(args,first,&a,8000) || !pd_predecessor_image_parse(first,a,expected,layer)){return 0;}
    const char* const parts[]={"var","lib","smartsdr","containers","storage","overlay",layer,"diff"};
    int root=predecessor_path(parts,sizeof(parts)/sizeof(parts[0]));
    int ok=root>=0 && predecessor_root_matches(root,expected);if(root>=0){close(root);}
    return ok && predecessor_command(args,second,&b,8000) && a==b && !memcmp(first,second,a);
}
int pd_predecessor_image_verify(const struct pd_predecessor* expected)
{
    char layer[65];return predecessor_image_layer(expected,layer);
}
static int predecessor_exists(const char* id)
{
    char* const args[]={"/usr/bin/podman","container","exists",(char*)id,NULL};
    unsigned char bytes[kInventoryMax];size_t size=0;int code=predecessor_command_status(args,bytes,&size,8000);
    return size==0 && (code==0 || code==1)?code:-1;
}
static int predecessor_transaction(int store,const unsigned char* approval,unsigned phase,struct pd_recovery* recipe)
{
    if(!predecessor_privileged() || !predecessor_quiet()){return -1;}
    const char* const parts[]={"var","lib","smartsdr","aetherpd"};
    int canonical=predecessor_path(parts,sizeof(parts)/sizeof(parts[0]));struct stat a,b;
    int ok=canonical>=0 && !fstat(canonical,&a) && !fstat(store,&b) && a.st_dev==b.st_dev && a.st_ino==b.st_ino;
    if(canonical>=0){close(canonical);}
    if(!ok || !pd_transaction_at_phase(store,approval,phase) || !pd_recovery_read(store,approval,recipe)){return -1;}
    int lock=openat(store,"migration.lock",O_RDWR|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    ok=lock>=0 && !fstat(lock,&a) && S_ISREG(a.st_mode) && a.st_uid==predecessor_owner() &&
        a.st_nlink==1 && (a.st_mode&07777)==0600 && a.st_size==0 && !flock(lock,LOCK_EX|LOCK_NB) &&
        pd_transaction_at_phase(store,approval,phase) && predecessor_quiet();
    if(!ok){if(lock>=0){close(lock);}return -1;}return lock;
}
static int predecessor_stopped_as(const struct pd_predecessor* expected,const struct pd_recovery* recipe)
{
    struct pd_predecessor observed;
    int ok=recipe?pd_predecessor_restored_status(recipe,&observed):predecessor_status(expected,&observed);
    return ok && observed.pid==0 && predecessor_same(expected,&observed);
}
static int predecessor_verify_stopped(const struct pd_predecessor* expected,const struct pd_recovery* recipe)
{
    return predecessor_quiet() && predecessor_stopped_as(expected,recipe) &&
        pd_predecessor_overlay_verify(expected) && predecessor_quiet() && predecessor_stopped_as(expected,recipe);
}
int pd_predecessor_retire(int store,const unsigned char* approval)
{
    struct pd_recovery recipe;int lock=predecessor_transaction(store,approval,PD_TRANSACTION_RETIRING,&recipe);
    if(lock<0){return 0;}const struct pd_predecessor* original=&recipe.migration.predecessor;
    int ok=pd_predecessor_image_verify(original) && pd_state_transfer(store,-1,-1,recipe.migration.approval);
    int absent=ok?predecessor_exists(original->container):-1;
    if(absent==0){
        ok=predecessor_verify_stopped(original,NULL) && pd_transaction_at_phase(store,approval,PD_TRANSACTION_RETIRING);
        char* const args[]={"/usr/bin/podman","rm",(char*)original->container,NULL};
        unsigned char bytes[kInventoryMax];size_t size=0;
        ok=ok && predecessor_command(args,bytes,&size,15000);
    }else{ok=ok && absent==1;}
    ok=ok && predecessor_exists(original->container)==1 && predecessor_exists("flex-tailnet-shim")==1 &&
        pd_predecessor_image_verify(original);close(lock);return ok;
}
static int predecessor_restore_state(int store,const struct pd_recovery* recipe)
{
    int made=!mkdirat(store,"legacy",0700);if(!made && errno!=EEXIST){return 0;}
    int legacy=predecessor_directory(store,"legacy");struct stat st;
    int ok=legacy>=0 && !fstat(legacy,&st) && (st.st_mode&07777)==0700 && !fsync(store);
    if(ok && mkdirat(legacy,"state",0700) && errno!=EEXIST){ok=0;}
    int state=openat(store,"state",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    int ts=state>=0?openat(state,"tsnet",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC):-1;
    int provision=state>=0?openat(state,"provision.json",O_RDONLY|O_NONBLOCK|O_NOFOLLOW|O_CLOEXEC):-1;
    int identity=ts>=0?openat(ts,"tailscaled.state",O_RDONLY|O_NONBLOCK|O_NOFOLLOW|O_CLOEXEC):-1;
    ok=ok && pd_state_restore_legacy(legacy,provision,identity,recipe->migration.approval);
    if(provision>=0){close(provision);}if(identity>=0){close(identity);}if(ts>=0){close(ts);}
    if(state>=0){close(state);}if(legacy>=0){close(legacy);}return ok;
}
static int predecessor_restore_create(const struct pd_recovery* recipe)
{
    char address[40];snprintf(address,sizeof(address),"SSDR_RADIO_ADDRESS=%s",recipe->radio_address);
    char* const args[]={"/usr/bin/podman","create","--pull=never","--name","flex-tailnet-shim","--hostname",(char*)recipe->hostname,
        "--network=host","--pid=private","--ipc=shareable","--restart=always","--cap-drop=AUDIT_WRITE",
        "--cap-drop=MKNOD","--cap-drop=NET_RAW","--workdir=/","--volume",
        "/var/lib/smartsdr/aetherpd/legacy/state:/var/lib/flex-tailnet-shim:rw,rprivate,nosuid,rbind",
        "--env","PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin","--env","TERM=xterm",
        "--env","container=podman","--env",address,(char*)recipe->migration.predecessor.image,NULL};
    unsigned char bytes[kInventoryMax];size_t size=0;
    if(!predecessor_command(args,bytes,&size,15000) || size!=65 || bytes[64]!='\n'){return 0;}
    bytes[64]=0;struct pd_predecessor observed;
    return predecessor_id((const char*)bytes) && pd_predecessor_restored_status(recipe,&observed) &&
        !strcmp(observed.container,(const char*)bytes);
}
static int predecessor_restored_running(const struct pd_recovery* recipe)
{
    struct pd_predecessor first,second;
    return pd_predecessor_restored_status(recipe,&first) && first.pid>0 &&
        predecessor_executable(first.pid,first.executable) &&
        !crypto_verify32(first.executable,recipe->migration.predecessor.executable) &&
        pd_predecessor_restored_status(recipe,&second) && predecessor_same(&first,&second) && first.pid==second.pid;
}
int pd_predecessor_restore(int store,const unsigned char* approval)
{
    struct pd_recovery recipe;int lock=predecessor_transaction(store,approval,PD_TRANSACTION_ROLLBACK,&recipe);
    if(lock<0){return 0;}
    int ok=predecessor_exists(recipe.migration.predecessor.container)==1 &&
        pd_predecessor_image_verify(&recipe.migration.predecessor);
    int absent=ok?predecessor_exists("flex-tailnet-shim"):-1;struct pd_predecessor restored;
    if(absent==0){ok=pd_predecessor_restored_status(&recipe,&restored);}
    else{ok=ok && absent==1;}
    ok=ok && predecessor_restore_state(store,&recipe) && predecessor_quiet() &&
        pd_transaction_at_phase(store,approval,PD_TRANSACTION_ROLLBACK);
    if(ok && absent==1){ok=predecessor_restore_create(&recipe);}
    ok=ok && pd_predecessor_restored_status(&recipe,&restored);
    if(ok && restored.pid==0){
        memcpy(restored.executable,recipe.migration.predecessor.executable,32);
        ok=predecessor_verify_stopped(&restored,&recipe) && pd_transaction_at_phase(store,approval,PD_TRANSACTION_ROLLBACK);
        char* const args[]={"/usr/bin/podman","start",restored.container,NULL};
        unsigned char bytes[kInventoryMax];size_t size=0;
        ok=ok && predecessor_command(args,bytes,&size,15000);
    }
    ok=ok && predecessor_restored_running(&recipe) && predecessor_quiet();close(lock);return ok;
}

static const char kOverlayFormat[]=
    "PDOVL001\n{{.Id}}\n{{.Image}}\n{{.GraphDriver.Name}}\n{{len .GraphDriver.Data}}\n"
    "{{index .GraphDriver.Data \"MergedDir\"}}\n{{index .GraphDriver.Data \"UpperDir\"}}\n"
    "{{index .GraphDriver.Data \"WorkDir\"}}\n{{index .GraphDriver.Data \"LowerDir\"}}";
static int predecessor_overlay_parse(const unsigned char* bytes,size_t size,const struct pd_predecessor* expected,
                                     const char* image_layer,char upper_layer[65])
{
    if(!size || size>=kInventoryMax || bytes[size-1]!='\n' || memchr(bytes,0,size)){return 0;}
    char text[kInventoryMax];memcpy(text,bytes,size);text[size]=0;char* lines[9];size_t count=0;char* start=text;
    for(size_t i=0;i<size;++i){
        if(text[i]!='\n'){continue;}if(count>=9){return 0;}
        text[i]=0;lines[count++]=start;start=text+i+1;
    }
    static const char prefix[]="/var/lib/smartsdr/containers/storage/overlay/";
    if(count!=9 || strcmp(lines[0],"PDOVL001") || strcmp(lines[1],expected->container) ||
        strcmp(lines[2],expected->image) || strcmp(lines[3],"overlay") ||
        (strcmp(lines[4],"3") && strcmp(lines[4],"4")) ||
        strlen(lines[6])!=sizeof(prefix)-1+64+5 || strncmp(lines[6],prefix,sizeof(prefix)-1) ||
        strcmp(lines[6]+sizeof(prefix)-1+64,"/diff")){return 0;}
    memcpy(upper_layer,lines[6]+sizeof(prefix)-1,64);upper_layer[64]=0;
    if(!predecessor_id(upper_layer)){return 0;}
    char path[256];snprintf(path,sizeof(path),"%s%s/merged",prefix,upper_layer);
    if(strcmp(lines[5],!strcmp(lines[4],"3")?"":path)){return 0;}
    snprintf(path,sizeof(path),"%s%s/work",prefix,upper_layer);if(strcmp(lines[7],path)){return 0;}
    snprintf(path,sizeof(path),"%s%s/diff",prefix,image_layer);return !strcmp(lines[8],path);
}
static int predecessor_plain_upper(int upper)
{
    const char* const attrs[]={"trusted.overlay.opaque","trusted.overlay.redirect","user.overlay.opaque","user.overlay.redirect"};
    for(unsigned i=0;i<sizeof(attrs)/sizeof(attrs[0]);++i){
        if(fgetxattr(upper,attrs[i],NULL,0)>=0 || (errno!=ENODATA && errno!=ENOTSUP)){return 0;}
    }
    const char* const whiteouts[]={".wh.flex-tailnet-shim",".wh..wh..opq"};struct stat st;
    for(unsigned i=0;i<sizeof(whiteouts)/sizeof(whiteouts[0]);++i){
        if(!fstatat(upper,whiteouts[i],&st,AT_SYMLINK_NOFOLLOW) || errno!=ENOENT){return 0;}
    }
    return 1;
}
int pd_predecessor_overlay_verify(const struct pd_predecessor* expected)
{
    char image_layer[65],upper_layer[65];
    if(!predecessor_privileged() || !predecessor_id(expected->container) || !predecessor_image_layer(expected,image_layer)){return 0;}
    char* const args[]={"/usr/bin/podman","inspect","--format",(char*)kOverlayFormat,(char*)expected->container,NULL};
    unsigned char first[kInventoryMax],second[kInventoryMax];size_t a=0,b=0;
    if(!predecessor_command(args,first,&a,8000) || !predecessor_overlay_parse(first,a,expected,image_layer,upper_layer)){return 0;}
    const char* const parts[]={"var","lib","smartsdr","containers","storage","overlay",upper_layer,"diff"};
    int upper=predecessor_path(parts,sizeof(parts)/sizeof(parts[0]));struct stat st;
    int ok=upper>=0 && predecessor_plain_upper(upper);
    if(ok && !fstatat(upper,"flex-tailnet-shim",&st,AT_SYMLINK_NOFOLLOW)){ok=predecessor_root_matches(upper,expected);}
    else if(ok && errno!=ENOENT){ok=0;}
    if(upper>=0){close(upper);}
    return ok && predecessor_command(args,second,&b,8000) && a==b && !memcmp(first,second,a);
}
