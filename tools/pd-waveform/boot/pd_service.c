#define _GNU_SOURCE
#include "pd_service.h"
#include "pd_transaction.h"
#include "pd_upgrade_state.h"
#include "pd_worker_units.h"
#include "pd_upgrade_units.h"
#include "pd_units.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/vfs.h>
#include <time.h>
#include <unistd.h>
enum { kServiceMax=PD_SERVICE_OUTPUT_MAX };
static const char* const kNames[]={"aetherpd-boot.service","aetherpd-start.socket","aetherpd-shim.service"};
static const char* const kUpgradeWorkers[]={"aetherpd-upgrade.service","aetherpd-upgrade-recover.service"};
static const char kProperties[]="--property=Id,Names,LoadState,ActiveState,SubState,MainPID,ControlPID,ControlGroup,FragmentPath,SourcePath,DropInPaths,NeedDaemonReload,Transient,Type,ExecStart,ExecStartPre,ExecStartPost,ExecStop,ExecStopPost,ExecReload,User,Group,Environment,EnvironmentFiles,WorkingDirectory,RootDirectory,KillMode,RemainAfterExit,Listen,Accept,StandardInput,SendSIGKILL";
#ifdef PD_SERVICE_TEST
extern int pd_service_test_command(char* const*,unsigned char*,size_t*,int);
extern int pd_service_test_root(void);
static uid_t service_owner(void){return getuid();}
static int service_root(void){return pd_service_test_root();}
#else
static uid_t service_owner(void){return 0;}
static int service_root(void){return open("/",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);}
#endif
static int64_t service_now(void)
{
    struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t)){return -1;}
    return (int64_t)t.tv_sec*1000+t.tv_nsec/1000000;
}
static int service_command(char* const* args,unsigned char* bytes,size_t* size,int timeout)
{
#ifdef PD_SERVICE_TEST
    return pd_service_test_command(args,bytes,size,timeout);
#else
    *size=0;if(getuid()!=0 || geteuid()!=0){return 0;}
    int pipes[2];if(pipe2(pipes,O_CLOEXEC)){return 0;}pid_t parent=getpid(),child=fork();
    if(!child){
        if(prctl(PR_SET_PDEATHSIG,SIGKILL) || getppid()!=parent || setpgid(0,0) || dup2(pipes[1],1)<0){_exit(126);}
        int null=open("/dev/null",O_RDWR|O_CLOEXEC);
        if(null<0 || dup2(null,0)<0 || dup2(null,2)<0){_exit(126);}
        long limit=sysconf(_SC_OPEN_MAX);if(limit<0 || limit>1048576){_exit(126);}
        for(int fd=3;fd<limit;++fd){close(fd);}
        char* const env[]={"PATH=/usr/sbin:/usr/bin:/sbin:/bin","LC_ALL=C","SYSTEMD_COLORS=0",NULL};
        execve("/usr/bin/systemctl",args,env);_exit(127);
    }
    close(pipes[1]);if(child<0){close(pipes[0]);return 0;}(void)setpgid(child,child);
    int flags=fcntl(pipes[0],F_GETFL);int ok=flags>=0 && !fcntl(pipes[0],F_SETFL,flags|O_NONBLOCK);
    int64_t now=service_now(),deadline=now+timeout;ok=ok && now>=0;
    int status=0,done=0,eof=0;
    while(ok && !(done && eof)){
        ssize_t n=read(pipes[0],bytes+*size,kServiceMax-*size);
        if(n>0){*size+=(size_t)n;if(*size==kServiceMax){ok=0;break;}}
        else if(!n){eof=1;}
        else if(errno!=EINTR && errno!=EAGAIN){ok=0;break;}
        if(!done){pid_t waited=waitpid(child,&status,WNOHANG);done=waited==child;if(waited<0 && errno!=EINTR){ok=0;break;}}
        if(done && eof){break;}now=service_now();if(now<0 || now>=deadline){ok=0;break;}
        struct pollfd p={pipes[0],POLLIN,0};(void)poll(&p,1,20);
    }
    close(pipes[0]);if(!ok){(void)kill(-child,SIGKILL);if(!done){(void)kill(child,SIGKILL);}}
    if(!done){while(waitpid(child,&status,0)<0 && errno==EINTR){}}
    return ok && WIFEXITED(status) && WEXITSTATUS(status)==0;
#endif
}
static const char* service_boot_path(void)
{
#if defined(__arm__) && !defined(__aarch64__)
    return "/user/aetherpd/pd-boot";
#else
    return "/var/lib/smartsdr/aetherpd/pd-boot";
#endif
}
static const char* service_upgrade_path(void)
{
#if defined(__arm__) && !defined(__aarch64__)
    return "/user/aetherpd-upgrade/worker";
#else
    return "/var/lib/smartsdr/aetherpd-upgrade/worker";
#endif
}
static int service_number(const char* value,pid_t* pid)
{
    unsigned number=0;size_t n=strlen(value);if(!n || n>7 || (n>1 && *value=='0')){return 0;}
    for(size_t i=0;i<n;++i){if(value[i]<'0' || value[i]>'9'){return 0;}number=number*10+(unsigned)(value[i]-'0');}
    if(number>4194304 || number==1){return 0;}*pid=(pid_t)number;return 1;
}
static int service_exec(const char* value,const char* path,const char* argument)
{
    char prefix[512];int n=snprintf(prefix,sizeof(prefix),"{ path=%s ; argv[]=%s%s%s ; ignore_errors=no ; start_time=",path,path,*argument?" ":"",argument);
    if(n<0 || (size_t)n>=sizeof(prefix) || strncmp(value,prefix,(size_t)n)){return 0;}
    const char* p=value+n;const char* const fields[]={" ; stop_time="," ; pid="," ; code="," ; status="," }"};
    for(unsigned i=0;i<5;++i){
        const char* end=strstr(p,fields[i]);if(!end || end==p || (size_t)(end-p)>128){return 0;}
        for(const char* q=p;q<end;++q){if((unsigned char)*q<32 || (unsigned char)*q>126 || *q=='{' || *q=='}' || *q==';'){return 0;}}
        if(i==2){char number[16];size_t size=(size_t)(end-p);pid_t pid;if(size>=sizeof(number)){return 0;}memcpy(number,p,size);number[size]=0;if(!service_number(number,&pid)){return 0;}}
        p=end+strlen(fields[i]);
    }
    return !*p;
}
static int service_parse(unsigned kind,const char* name,const unsigned char* bytes,size_t size,struct pd_service_state* result)
{
    memset(result,0,sizeof(*result));result->control_pid=-1;
    if(kind>7 || !size || size>=kServiceMax || bytes[size-1]!='\n' || memchr(bytes,0,size)){return 0;}
    static const char* const keys[]={"Id","Names","LoadState","ActiveState","SubState","MainPID","ControlPID","ControlGroup","FragmentPath","SourcePath","DropInPaths","NeedDaemonReload","Transient","Type","ExecStart","ExecStartPre","ExecStartPost","ExecStop","ExecStopPost","ExecReload","User","Group","Environment","EnvironmentFiles","WorkingDirectory","RootDirectory","KillMode","RemainAfterExit","Listen","Accept","StandardInput","SendSIGKILL"};
    char text[kServiceMax];memcpy(text,bytes,size);text[size]=0;char* values[32]={0};char* cursor=text;
    while(*cursor){
        char* end=strchr(cursor,'\n');if(!end || end==cursor){return 0;}*end=0;char* equal=strchr(cursor,'=');if(!equal){return 0;}*equal=0;
        unsigned index=0;for(;index<32 && strcmp(cursor,keys[index]);++index){}
        if(index==32 || values[index]){return 0;}values[index]=equal+1;cursor=end+1;
    }
    const unsigned required[]={0,1,2,3,4,8,9,10,11,12};
    for(unsigned i=0;i<sizeof(required)/sizeof(required[0]);++i){if(!values[required[i]]){return 0;}}
    char fragment[128];snprintf(fragment,sizeof(fragment),"/etc/systemd/system/%s",kind==PD_SERVICE_SESSION?"aetherpd-start@.service":name);
    if(strcmp(values[0],name) || strcmp(values[1],name) || strcmp(values[2],"loaded") ||
        strcmp(values[8],fragment) || *values[9] || *values[10] || strcmp(values[11],"no") || strcmp(values[12],"no")){return 0;}
    const unsigned empty[]={15,17,18,19,20,21,22,23,24,25};
    for(unsigned i=0;i<sizeof(empty)/sizeof(empty[0]);++i){if(values[empty[i]] && *values[empty[i]]){return 0;}}
    int active=!strcmp(values[3],"active"),quiet=!strcmp(values[3],"inactive") || !strcmp(values[3],"failed");
    if(!active && !quiet && strcmp(values[3],"activating") && strcmp(values[3],"deactivating")){return 0;}
    if(kind==PD_SERVICE_SOCKET){
        if(values[6] && !service_number(values[6],&result->control_pid)){return 0;}
        if(!values[28] || strcmp(values[28],"/run/aetherpd/start.sock (Stream)") || !values[29] || strcmp(values[29],"yes") ||
            !values[16] || !service_exec(values[16],service_boot_path(),"--socket-owner") || (values[14] && *values[14])){return 0;}
        if(active && strcmp(values[4],"listening")){return 0;}
    }else{
        const char* path=kind>=6?service_upgrade_path():(kind==PD_SERVICE_BOOT || kind>=4)?service_boot_path():kind==PD_SERVICE_SESSION?"/run/aetherpd/bin/pd-start-session":"/run/aetherpd/bin/pd-primary-launch";
        const unsigned present[]={5,6,7,13,14,26,27,30};
        for(unsigned i=0;i<sizeof(present)/sizeof(present[0]);++i){if(!values[present[i]]){return 0;}}
        const char* argument=kind==4?"--install-approved":kind==5?"--recover-installation":kind==7?"--recover":"";
        if(kind==PD_SERVICE_PRIMARY || kind==PD_SERVICE_SESSION){
            const char* managed_argument=kind==PD_SERVICE_PRIMARY?"--primary":"--session";
            if(service_exec(values[14],service_boot_path(),managed_argument)){
                result->managed_startup=1;path=service_boot_path();argument=managed_argument;
            }
        }
        if(!service_number(values[5],&result->pid) || strcmp(values[6],"0") ||
            strcmp(values[13],(kind==PD_SERVICE_BOOT || kind>=4)?"oneshot":"simple") ||
            !service_exec(values[14],path,argument) ||
            (values[16] && *values[16]) || strcmp(values[26],"control-group") ||
            strcmp(values[27],kind==PD_SERVICE_BOOT?"yes":"no") || strcmp(values[30],kind==PD_SERVICE_SESSION?"socket":"null")){return 0;}
        if(kind==PD_SERVICE_SESSION){
            if(!values[31] || (strcmp(values[31],"no") && strcmp(values[31],"yes"))){return 0;}
            result->guardian_hold=!strcmp(values[31],"no");
        }
        result->populated=*values[7]!=0;
        result->control_pid=0;
        char group[192];snprintf(group,sizeof(group),kind==PD_SERVICE_SESSION?"/system.slice/system-aetherpd\\x2dstart.slice/%s":"/system.slice/%s",name);
        if(*values[7] && strcmp(values[7],group)){return 0;}
        if(active && (strcmp(values[4],kind==PD_SERVICE_BOOT?"exited":"running") ||
            (kind!=PD_SERVICE_BOOT && (!result->pid || !*values[7])) || (kind==PD_SERVICE_BOOT && result->pid))){return 0;}
    }
    if(quiet && (result->pid || (strcmp(values[4],"dead") && strcmp(values[4],"failed")))){return 0;}
    result->active=active;result->quiet=quiet;return 1;
}
int pd_service_parse(unsigned kind,const unsigned char* bytes,size_t size,struct pd_service_state* result)
{
    if(kind>PD_SERVICE_PRIMARY){memset(result,0,sizeof(*result));return 0;}
    return service_parse(kind,kNames[kind],bytes,size,result);
}
int pd_service_upgrade_cleanup_idle(void)
{
    struct pd_service_state boot,socket;
    return pd_service_files() && pd_service_read(PD_SERVICE_BOOT,&boot) && boot.active && !boot.pid &&
        !boot.control_pid && pd_service_read(PD_SERVICE_SOCKET,&socket) && socket.active && !socket.control_pid;
}
static int service_instance(const char* name)
{
    const char prefix[]="aetherpd-start@";size_t length=strlen(name);
    if(length>=96 || strncmp(name,prefix,sizeof(prefix)-1)){return 0;}
    const char* p=name+sizeof(prefix)-1;const char* end=strchr(p,'-');
    if(!end || end==p || end-p>20 || (end-p>1 && *p=='0')){return 0;}
    uint64_t serial=0;
    for(;p<end;++p){if(*p<'0' || *p>'9' || serial>(UINT64_MAX-(unsigned)(*p-'0'))/10){return 0;}serial=serial*10+(unsigned)(*p-'0');}
    p=end+1;end=strchr(p,'-');if(!end || end==p || end-p>7 || strcmp(end,"-78666.service")){return 0;}
    char pid_text[8];memcpy(pid_text,p,(size_t)(end-p));pid_text[end-p]=0;pid_t pid;
    return service_number(pid_text,&pid) && pid>=2;
}
int pd_service_sessions_parse(const unsigned char* bytes,size_t size,struct pd_service_sessions* sessions)
{
    memset(sessions,0,sizeof(*sessions));if(!size){return 1;}
    if(size>=kServiceMax || bytes[size-1]!='\n' || memchr(bytes,0,size)){return 0;}
    char text[kServiceMax];memcpy(text,bytes,size);text[size]=0;char* block=text;
    while(*block){
        char* end=strstr(block,"\n\n");size_t length=end?(size_t)(end-block)+1:strlen(block);
        if(sessions->count==PD_SERVICE_SESSIONS_MAX || !length){return 0;}
        char* id=!strncmp(block,"Id=",3)?block:strstr(block,"\nId=");
        if(!id || (size_t)(id-block)>=length){return 0;}if(*id=='\n'){++id;}id+=3;
        char* last=strchr(id,'\n');if(!last || (size_t)(last-block)>=length || last-id>=96){return 0;}
        char* name=sessions->names[sessions->count];memcpy(name,id,(size_t)(last-id));name[last-id]=0;
        if(!service_instance(name)){return 0;}
        for(unsigned i=0;i<sessions->count;++i){if(!strcmp(name,sessions->names[i])){return 0;}}
        if(!service_parse(PD_SERVICE_SESSION,name,(const unsigned char*)block,length,&sessions->states[sessions->count])){return 0;}
        if(sessions->count && (sessions->states[sessions->count].managed_startup!=sessions->states[0].managed_startup ||
            sessions->states[sessions->count].guardian_hold!=sessions->states[0].guardian_hold)){return 0;}
        ++sessions->count;if(!end){break;}block=end+2;
    }
    return 1;
}
static int service_directory(int fd)
{
    struct stat st;return fd>=0 && !fstat(fd,&st) && S_ISDIR(st.st_mode) && st.st_uid==service_owner() && !(st.st_mode&022);
}
/* Only complete fixed entry/stop-policy layouts are recognized. Older stop
 * policies may be quiesced only when their session groups are already empty. */
static int service_file_layout(void)
{
    int fd=service_root();if(!service_directory(fd)){if(fd>=0){close(fd);}return -1;}
    const char* const components[]={"etc","systemd","system"};
    for(unsigned i=0;i<3;++i){int next=openat(fd,components[i],O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);close(fd);fd=next;if(!service_directory(fd)){if(fd>=0){close(fd);}return -1;}}
    const char* const names[]={"aetherpd-boot.service","aetherpd-start.socket","aetherpd-start@.service","aetherpd-shim.service"};
#if defined(__arm__) && !defined(__aarch64__)
    const char* const* definitions[]={pd_units_armv7_legacy_stop,pd_units_armv7_managed_legacy_stop,pd_units_armv7,pd_units_armv7_managed};
#else
    const char* const* definitions[]={pd_units_arm64_legacy_stop,pd_units_arm64_managed_legacy_stop,pd_units_arm64,pd_units_arm64_managed};
#endif
    int matches[4]={1,1,1,1};
    for(unsigned i=0;(matches[0] || matches[1] || matches[2] || matches[3]) && i<4;++i){
        int file=openat(fd,names[i],O_RDONLY|O_NONBLOCK|O_NOFOLLOW|O_CLOEXEC);struct stat st;char bytes[2048];
        int ok=file>=0 && !fstat(file,&st) && S_ISREG(st.st_mode) && st.st_uid==service_owner() && st.st_nlink==1 &&
            (st.st_mode&07777)==0644 && st.st_size>0 && st.st_size<=(off_t)sizeof(bytes) &&
            pread(file,bytes,(size_t)st.st_size,0)==st.st_size;
        for(unsigned layout=0;layout<4;++layout){
            size_t n=strlen(definitions[layout][i]);
            matches[layout]=matches[layout] && ok && st.st_size==(off_t)n && !memcmp(bytes,definitions[layout][i],n);
        }
        if(file>=0){close(file);}
    }
    close(fd);return matches[3]?3:matches[2]?2:matches[1]?1:matches[0]?0:-1;
}
int pd_service_files(void)
{
    return service_file_layout()>=0;
}
int pd_service_read(unsigned kind,struct pd_service_state* state)
{
    if(kind>PD_SERVICE_PRIMARY){return 0;}unsigned char bytes[kServiceMax];size_t size=0;
    char* const args[]={"/usr/bin/systemctl","show","--all",(char*)kNames[kind],(char*)kProperties,NULL};
    int layout=service_file_layout();
    return layout>=0 && service_command(args,bytes,&size,8000) && pd_service_parse(kind,bytes,size,state) &&
        (kind!=PD_SERVICE_PRIMARY || state->managed_startup==(layout&1)) && service_file_layout()==layout;
}
static int service_loaded(void)
{
    struct pd_service_state state;int layout=service_file_layout();if(layout<0){return 0;}
    for(unsigned i=0;i<3;++i){if(!pd_service_read(i,&state)){return 0;}}
    /* Load the template without starting it. PID2 cannot be the admitted Go
     * client, so this fixed inspection instance cannot be a real session. */
    const char* inspection="aetherpd-start@0-2-78666.service";
    unsigned char bytes[kServiceMax];size_t size=0;
    char* const args[]={"/usr/bin/systemctl","show","--all",(char*)inspection,(char*)kProperties,NULL};
    return service_command(args,bytes,&size,8000) &&
        service_parse(PD_SERVICE_SESSION,inspection,bytes,size,&state) && state.quiet && !state.pid && state.managed_startup==(layout&1) && state.guardian_hold==(layout>>1) && service_file_layout()==layout;
}
int pd_service_reload(void)
{
    if(!pd_service_files()){return 0;}unsigned char bytes[kServiceMax];size_t size=0;
    char* const args[]={"/usr/bin/systemctl","daemon-reload",NULL};
    return service_command(args,bytes,&size,15000) && service_loaded();
}
static int service_activation_phase(int store,const unsigned char* approval,int upgrade)
{
    if(!upgrade){return pd_transaction_at_phase(store,approval,PD_TRANSACTION_ACTIVATING);}
    int layout=service_file_layout();
    return layout>=0 && (layout&1)==1 && (pd_upgrade_at_phase(store,approval,PD_UPGRADE_ACTIVATING) ||
        pd_upgrade_at_phase(store,approval,PD_UPGRADE_RESTORING));
}
static int service_prepare_runtime(int store,const unsigned char* approval,int upgrade)
{
    if(!service_activation_phase(store,approval,upgrade) || !pd_service_files() || !service_loaded()){return 0;}
    unsigned char bytes[kServiceMax];size_t size=0;struct pd_service_state state;
    char* const args[]={"/usr/bin/systemctl","start","aetherpd-boot.service",NULL};
    return service_command(args,bytes,&size,upgrade?100000:40000) && pd_service_read(PD_SERVICE_BOOT,&state) && state.active &&
        service_loaded() && service_activation_phase(store,approval,upgrade);
}
static int service_activate(int store,const unsigned char* approval,int upgrade,int executable,
                             const char* address,const struct pd_readiness* expected,struct pd_primary* primary)
{
    memset(primary,0,sizeof(*primary));primary->process=primary->executable=-1;
    if(!service_activation_phase(store,approval,upgrade) || !pd_service_files() || !service_loaded()){return 0;}
    unsigned char bytes[kServiceMax];size_t size=0;
    char* const args[]={"/usr/bin/systemctl","start","aetherpd-shim.service",NULL};
    if(!service_command(args,bytes,&size,45000)){return 0;}
    int64_t now=service_now(),deadline=now+30000;if(now<0){return 0;}
    struct pd_service_state state;
    while((now=service_now())>=0 && now<deadline){
        if(!pd_service_read(PD_SERVICE_PRIMARY,&state)){return 0;}
        if(state.active && pd_primary_open(state.pid,executable,primary)){break;}
        if(state.quiet){return 0;}struct timespec delay={0,100000000};nanosleep(&delay,NULL);
    }
    struct pd_service_state boot,socket;
    int ok=primary->process>=0 && pd_primary_wait(primary,address,expected) && service_loaded() &&
        pd_service_read(PD_SERVICE_BOOT,&boot) && boot.active && pd_service_read(PD_SERVICE_SOCKET,&socket) && socket.active &&
        pd_service_read(PD_SERVICE_PRIMARY,&state) && state.active && state.pid==primary->pid && pd_primary_check(primary) &&
        service_activation_phase(store,approval,upgrade);
    if(!ok){pd_primary_close(primary);}return ok;
}
int pd_service_prepare_runtime(int store,const unsigned char plan[PD_MIGRATION_SIZE])
{
    return service_prepare_runtime(store,plan,0);
}
int pd_service_upgrade_prepare_runtime(int store,const unsigned char approval[PD_UPGRADE_APPROVAL_SIZE])
{
    return service_prepare_runtime(store,approval,1);
}
int pd_service_activate(int store,const unsigned char plan[PD_MIGRATION_SIZE],int executable,
                        const char* address,const struct pd_readiness* expected,struct pd_primary* primary)
{
    return service_activate(store,plan,0,executable,address,expected,primary);
}
int pd_service_upgrade_activate(int store,const unsigned char approval[PD_UPGRADE_APPROVAL_SIZE],int executable,
                                const char* address,const struct pd_readiness* expected,struct pd_primary* primary)
{
    return service_activate(store,approval,1,executable,address,expected,primary);
}
int pd_service_stop_primary(void)
{
    struct pd_service_state state;if(!pd_service_files() || !pd_service_read(PD_SERVICE_PRIMARY,&state)){return 0;}
    unsigned char bytes[kServiceMax];size_t size=0;char* const args[]={"/usr/bin/systemctl","stop","aetherpd-shim.service",NULL};
    return service_command(args,bytes,&size,20000) && pd_service_read(PD_SERVICE_PRIMARY,&state) && state.quiet && !state.pid;
}
static int service_sessions(struct pd_service_sessions* sessions)
{
    unsigned char bytes[kServiceMax];size_t size=0;
    char* const args[]={"/usr/bin/systemctl","show","--all","aetherpd-start@*.service",(char*)kProperties,NULL};
    int layout=service_file_layout();
    if(layout<0 || !service_command(args,bytes,&size,8000) || !pd_service_sessions_parse(bytes,size,sessions)){return 0;}
    for(unsigned i=0;i<sessions->count;++i){if(sessions->states[i].managed_startup!=(layout&1) || sessions->states[i].guardian_hold!=(layout>>1)){return 0;}}
    return service_file_layout()==layout;
}
int pd_service_session_policy(void)
{
    int layout=service_file_layout();struct pd_service_sessions sessions;
    if(layout<2 || !service_loaded() || !service_sessions(&sessions)){return 0;}
    unsigned current=0;
    for(unsigned i=0;i<sessions.count;++i){
        if(sessions.states[i].pid==getpid() && sessions.states[i].active){++current;}
    }
    return current==1 && service_file_layout()==layout;
}
static int service_owned_path(const char* path,int sessions_only)
{
    for(unsigned i=0;!sessions_only && i<3;++i){
        char expected[128];snprintf(expected,sizeof(expected),"/system.slice/%s",kNames[i]);size_t n=strlen(expected);
        if(!strncmp(path,expected,n) && (!path[n] || path[n]=='/')){return 1;}
    }
    const char prefix[]="/system.slice/system-aetherpd\\x2dstart.slice/";
    if(strncmp(path,prefix,sizeof(prefix)-1)){return 0;}
    const char* name=path+sizeof(prefix)-1;const char* end=strchr(name,'/');size_t n=end?(size_t)(end-name):strlen(name);
    if(n>=96){return 0;}char unit[96];memcpy(unit,name,n);unit[n]=0;return service_instance(unit);
}
static int service_cgroup(const unsigned char* bytes,size_t size,int sessions_only)
{
    if(!size || size>=8192 || bytes[size-1]!='\n' || memchr(bytes,0,size)){return -1;}
    char text[8192];memcpy(text,bytes,size);text[size]=0;char* line=text;int owned=0;
    while(*line){
        char* end=strchr(line,'\n');if(!end || end==line){return -1;}*end=0;
        char* first=strchr(line,':');char* second=first?strchr(first+1,':'):NULL;
        if(!first || !second || first==line || second[1]!='/'){return -1;}
        *first=*second=0;
        for(const char* p=line;*p;++p){if(*p<'0' || *p>'9'){return -1;}}
        if((!strcmp(line,"0") && !first[1]) || !strcmp(first+1,"name=systemd")){owned|=service_owned_path(second+1,sessions_only);}
        line=end+1;
    }
    return owned;
}
int pd_service_owned_cgroup(const unsigned char* bytes,size_t size)
{
    return service_cgroup(bytes,size,0);
}
static int service_proc_read(int directory,unsigned char bytes[8192],size_t* size)
{
    *size=0;int fd=openat(directory,"cgroup",O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    if(fd<0){return errno==ENOENT?0:-1;}
    int ok=1;
    while(*size<8192){
        ssize_t n=read(fd,bytes+*size,8192-*size);if(n<0 && errno==EINTR){continue;}
        if(n<0){ok=0;break;}if(!n){break;}*size+=(size_t)n;
    }
    close(fd);return ok && *size<8192?1:-1;
}
static int service_processes_quiet(int sessions_only)
{
#ifndef PD_SERVICE_TEST
    if(getuid()!=0 || geteuid()!=0){return 0;}
#endif
    int root=service_root();if(root<0){return 0;}
    int proc=openat(root,"proc",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);close(root);if(proc<0){return 0;}
#ifndef PD_SERVICE_TEST
    struct statfs filesystem;
    if(fstatfs(proc,&filesystem) || filesystem.f_type!=0x9fa0){close(proc);return 0;}
    const char* const namespaces[]={"pid","net","cgroup"};
    for(unsigned i=0;i<3;++i){
        char self[32],init[32];snprintf(self,sizeof(self),"self/ns/%s",namespaces[i]);snprintf(init,sizeof(init),"1/ns/%s",namespaces[i]);
        struct stat a,b;int left=fstatat(proc,self,&a,0),left_error=errno,right=fstatat(proc,init,&b,0),right_error=errno;
        if(i==2 && left && right && left_error==ENOENT && right_error==ENOENT){continue;}
        if(left || right || a.st_dev!=b.st_dev || a.st_ino!=b.st_ino){close(proc);return 0;}
    }
#endif
    DIR* directory=fdopendir(proc);if(!directory){close(proc);return 0;}
    int64_t now=service_now(),deadline=now+5000;int ok=now>=0;unsigned count=0;
    while(ok){
        errno=0;struct dirent* entry=readdir(directory);if(!entry){if(errno){ok=0;}break;}
        if(entry->d_name[0]<'0' || entry->d_name[0]>'9'){continue;}
        if(++count>16384 || (now=service_now())<0 || now>=deadline){ok=0;break;}
        for(const char* p=entry->d_name;*p;++p){if(*p<'0' || *p>'9'){ok=0;break;}}if(!ok){break;}
        int process=openat(proc,entry->d_name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        if(process<0){if(errno==ENOENT){continue;}ok=0;break;}
        unsigned char bytes[8192];size_t size=0;int read_result=service_proc_read(process,bytes,&size);
        if(read_result<0){ok=0;}else if(read_result>0 && service_cgroup(bytes,size,sessions_only)!=0){ok=0;}
        char executable[4096];ssize_t n=readlinkat(process,"exe",executable,sizeof(executable));
        if(n<0){if(errno!=ENOENT){ok=0;}}
        else if(n==(ssize_t)sizeof(executable)){ok=0;}
        else if(n>=14 && !memcmp(executable,"/run/aetherpd/",14)){
            executable[n]=0;
            int primary=sessions_only && read_result>0 && pd_service_owned_cgroup(bytes,size)==1 &&
                (!strcmp(executable,"/run/aetherpd/bin/flex-tailnet-shim") || !strcmp(executable,"/run/aetherpd/bin/pd-primary-launch"));
            if(!primary){ok=0;}
        }
        close(process);
    }
    closedir(directory);return ok;
}
int pd_service_runtime_quiet(void)
{
    return service_processes_quiet(0);
}
static int service_legacy_sessions_quiet(const struct pd_service_sessions* sessions)
{
    int layout=service_file_layout();if(layout<0){return 0;}
    if(layout>=2){return 1;}
    for(unsigned i=0;i<sessions->count;++i){
        if(!sessions->states[i].quiet || sessions->states[i].populated){return 0;}
    }
    return service_processes_quiet(1);
}
int pd_service_quiesce(void)
{
    struct pd_service_sessions sessions;
    if(!pd_service_files() || !service_loaded() || !service_sessions(&sessions) || !service_legacy_sessions_quiet(&sessions)){return 0;}
    unsigned char bytes[kServiceMax];size_t size=0;
    char* const intake[]={"/usr/bin/systemctl","stop","aetherpd-shim.service","aetherpd-start.socket",NULL};
    if(!service_command(intake,bytes,&size,30000)){return 0;}
    struct pd_service_state state;
    for(unsigned i=PD_SERVICE_SOCKET;i<=PD_SERVICE_PRIMARY;++i){if(!pd_service_read(i,&state) || !state.quiet){return 0;}}
    /* A final connection can be accepted while the intake units are stopping.
     * Inventory again after their stop jobs complete; never stop an unverified
     * instance through a wildcard command. */
    if(!service_sessions(&sessions) || !service_legacy_sessions_quiet(&sessions)){return 0;}
    if(sessions.count){
        char* args[PD_SERVICE_SESSIONS_MAX+3]={"/usr/bin/systemctl","stop"};
        for(unsigned i=0;i<sessions.count;++i){args[i+2]=sessions.names[i];}args[sessions.count+2]=NULL;
        if(!service_command(args,bytes,&size,30000)){return 0;}
    }
    char* const boot[]={"/usr/bin/systemctl","stop","aetherpd-boot.service",NULL};
    if(!service_command(boot,bytes,&size,20000) || !pd_service_files()){return 0;}
    for(unsigned i=0;i<3;++i){if(!pd_service_read(i,&state) || !state.quiet){return 0;}}
    if(!service_sessions(&sessions)){return 0;}
    for(unsigned i=0;i<sessions.count;++i){if(!sessions.states[i].quiet){return 0;}}
    return pd_service_runtime_quiet();
}

int pd_service_worker_parse(int recovery,const unsigned char* bytes,size_t size,struct pd_service_state* state)
{
    if(recovery!=0 && recovery!=1){return 0;}
    return service_parse(recovery?5:4,recovery?"aetherpd-recover.service":"aetherpd-install.service",bytes,size,state);
}
int pd_service_upgrade_worker_parse(int recovery,const unsigned char* bytes,size_t size,struct pd_service_state* state)
{
    if(recovery!=0 && recovery!=1){return 0;}
    return service_parse(recovery?7:6,kUpgradeWorkers[recovery],bytes,size,state);
}
static int service_worker_check(int units,int recovery,int upgrade)
{
    if((recovery<0 || recovery>2) || !(upgrade?pd_upgrade_units_verify(units):pd_worker_units_verify(units))){return 0;}
    for(int i=0;i<2;++i){
        unsigned char bytes[kServiceMax];size_t size=0;struct pd_service_state state;
        char* const args[]={"/usr/bin/systemctl","show","--all",upgrade?(char*)kUpgradeWorkers[i]:i?"aetherpd-recover.service":"aetherpd-install.service",(char*)kProperties,NULL};
        if(!service_command(args,bytes,&size,8000) || !(upgrade?pd_service_upgrade_worker_parse(i,bytes,size,&state):pd_service_worker_parse(i,bytes,size,&state)) ||
            (i==recovery?(state.quiet || state.pid!=getpid()):(!state.quiet || state.pid))){return 0;}
    }
    return 1;
}

int pd_service_worker_ready(int units,int recovery)
{
    return (recovery==0 || recovery==1) && service_worker_check(units,recovery,0);
}
int pd_service_worker_launch(int units)
{
    if(!pd_service_reload() || !service_worker_check(units,2,0)){return 0;}
    unsigned char bytes[kServiceMax];size_t size=0;
    char* const args[]={"/usr/bin/systemctl","start","--no-block","aetherpd-install.service",NULL};
    return service_command(args,bytes,&size,8000);
}
int pd_service_upgrade_worker_ready(int units,int recovery)
{
    return (recovery==0 || recovery==1) && service_worker_check(units,recovery,1);
}
int pd_service_upgrade_worker_launch(int units)
{
    unsigned char bytes[kServiceMax];size_t size=0;
    char* const reload[]={"/usr/bin/systemctl","daemon-reload",NULL};
    /* Interrupted management replacement can leave mixed runtime definitions.
     * Admission here concerns only the independent, authenticated worker units;
     * that worker repairs and verifies runtime definitions before activation. */
    if(!pd_upgrade_units_verify(units) || !service_command(reload,bytes,&size,15000) ||
        !service_worker_check(units,2,1)){return 0;}
    char* const args[]={"/usr/bin/systemctl","start","--no-block",(char*)kUpgradeWorkers[0],NULL};
    return service_command(args,bytes,&size,8000);
}
static int service_primary_link(int wants,const char* name)
{
    struct stat st;char target[128];const char expected[]="../aetherpd-shim.service";
    if(fstatat(wants,name,&st,AT_SYMLINK_NOFOLLOW)){return errno==ENOENT?0:-1;}
    return S_ISLNK(st.st_mode) && st.st_uid==service_owner() && st.st_nlink==1 &&
        readlinkat(wants,name,target,sizeof(target))==(ssize_t)(sizeof(expected)-1) &&
        !memcmp(target,expected,sizeof(expected)-1)?1:-1;
}
int pd_service_primary_autostart(int units,int enabled)
{
    struct stat st;
    if((enabled!=0 && enabled!=1) || fstat(units,&st) || !S_ISDIR(st.st_mode) ||
        st.st_uid!=service_owner() || (st.st_mode&022) || !pd_service_files()){return 0;}
    if(enabled && mkdirat(units,"multi-user.target.wants",0755) && errno!=EEXIST){return 0;}
    int wants=openat(units,"multi-user.target.wants",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(wants<0){return !enabled && errno==ENOENT;}
    const char* name="aetherpd-shim.service";const char* partial=".aetherpd-shim.service.new";
    int ok=!fstat(wants,&st) && S_ISDIR(st.st_mode) && st.st_uid==service_owner() && !(st.st_mode&022);
    int existing=ok?service_primary_link(wants,name):-1,staged=ok?service_primary_link(wants,partial):-1;
    ok=ok && existing>=0 && staged>=0;
    if(ok && enabled && !existing){
        ok=(staged || !symlinkat("../aetherpd-shim.service",wants,partial)) && !fsync(wants) &&
            !renameat(wants,partial,wants,name);
    }
    if(ok && !enabled && unlinkat(wants,name,0) && errno!=ENOENT){ok=0;}
    if(ok && unlinkat(wants,partial,0) && errno!=ENOENT){ok=0;}
    ok=ok && !fsync(wants) && !fsync(units) && service_primary_link(wants,name)==enabled;
    close(wants);return ok;
}
