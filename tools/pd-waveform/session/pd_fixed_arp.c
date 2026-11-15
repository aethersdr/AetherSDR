#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <linux/capability.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <poll.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Measured 8600 bench profile only. Unknown inventories fail closed; this is
 * not a FLEX6000 discovery/provisioning implementation. No caller addresses. */
static const char kInterface[]="eth0.10",kAddress[]="172.30.2.1",kCpuAddress[]="172.30.2.1";
static const char kLock[]="\0aetherpd.fixed-arp.8600.v1";
static const char kJournal[]=".aetherpd-fixed-arp.8600";
enum { kJournalSize=64 };
static volatile sig_atomic_t stopping;
static void stop_signal(int signal_number){(void)signal_number;stopping=1;}
static int lifetime_fd(const char* text)
{
    if(!text || !*text || strlen(text)>4){return -1;}
    for(const char* p=text;*p;++p){if(*p<'0' || *p>'9'){return -1;}}
    char* end;errno=0;long value=strtol(text,&end,10);
    return errno || *end || value<3 || value>1023 ? -1 : (int)value;
}
static int lifetime_ended(short events)
{
    return (events&(POLLHUP|POLLERR|POLLNVAL))!=0;
}
struct profile { unsigned char mac[6];int index; };
struct neighbour { int present,flags,family;unsigned char mac[6]; };
static int journal_boot_id(unsigned char id[36])
{
    int fd=open("/proc/sys/kernel/random/boot_id",O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    unsigned char bytes[38];ssize_t size=fd>=0?read(fd,bytes,sizeof(bytes)):-1;if(fd>=0){close(fd);}
    if(size!=37 || bytes[36]!='\n'){return 0;}
    for(unsigned i=0;i<36;++i){
        int dash=i==8 || i==13 || i==18 || i==23;
        if(dash?bytes[i]!='-':!((bytes[i]>='0' && bytes[i]<='9') || (bytes[i]>='a' && bytes[i]<='f'))){return 0;}
    }
    memcpy(id,bytes,36);return 1;
}
static void journal_record(unsigned char bytes[kJournalSize],const struct profile* profile,const unsigned char boot[36])
{
    memset(bytes,0,kJournalSize);memcpy(bytes,"PDARP001",8);unsigned index=(unsigned)profile->index;
    bytes[8]=(unsigned char)(index>>24);bytes[9]=(unsigned char)(index>>16);
    bytes[10]=(unsigned char)(index>>8);bytes[11]=(unsigned char)index;
    memcpy(bytes+12,profile->mac,6);memcpy(bytes+20,boot,36);
}
static uid_t journal_owner(void)
{
#ifdef PD_ARP_JOURNAL_TEST
    return getuid();
#else
    return 0;
#endif
}
static int journal_admit(int fd,const unsigned char expected[kJournalSize],int present,int* owned)
{
    struct stat st;unsigned char bytes[kJournalSize];*owned=0;
    if(fd<0 || fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_uid!=journal_owner() || st.st_nlink!=1 ||
        (st.st_mode&07777)!=0600 || st.st_size<0 || st.st_size>kJournalSize ||
        pread(fd,bytes,(size_t)st.st_size,0)!=st.st_size || memcmp(bytes,expected,(size_t)st.st_size)){return 0;}
    /* A partial intent cannot have preceded a successful add. Repair it only
     * while the entry is absent; matching existing state remains unowned. */
    if(st.st_size && st.st_size<kJournalSize && present){return 0;}
    *owned=st.st_size==kJournalSize || !present;return 1;
}
static int journal_open(void)
{
    int root=open("/",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);struct stat st;struct statfs fs;
    int ok=root>=0 && !fstat(root,&st) && st.st_uid==0 && !(st.st_mode&022);
    int run=ok?openat(root,"run",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC):-1;if(root>=0){close(root);}
    ok=ok && run>=0 && !fstat(run,&st) && st.st_uid==0 && !(st.st_mode&022) &&
        !fstatfs(run,&fs) && (unsigned long)fs.f_type==0x01021994UL;
    int fd=ok?openat(run,kJournal,O_RDWR|O_CREAT|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC,0600):-1;
    if(run>=0){close(run);}return fd;
}
static int journal_claim(int fd,const unsigned char bytes[kJournalSize])
{
    size_t used=0;
    while(used<kJournalSize){
        ssize_t size=pwrite(fd,bytes+used,kJournalSize-used,(off_t)used);
        if(size<0 && errno==EINTR){continue;}if(size<=0){return 0;}used+=(size_t)size;
    }
    return !fsync(fd);
}
static int journal_clear(int fd)
{
    /* Truncation is one syscall, so process death cannot expose a partially
     * cleared record. The record and ARP state are both volatile across boot. */
    return !ftruncate(fd,0) && !fsync(fd);
}
static void request(struct arpreq* r)
{
    memset(r,0,sizeof(*r));struct sockaddr_in* pa=(struct sockaddr_in*)&r->arp_pa;
    pa->sin_family=AF_INET;inet_pton(AF_INET,kAddress,&pa->sin_addr);
    memcpy(r->arp_dev,kInterface,sizeof(kInterface));
}
static int snapshot(int control,struct neighbour* n)
{
    memset(n,0,sizeof(*n));struct arpreq r;request(&r);
    if(ioctl(control,SIOCGARP,&r)){
        if(errno==ENXIO || errno==ENOENT){return 1;}return 0;
    }
    n->present=1;n->flags=r.arp_flags;n->family=r.arp_ha.sa_family;memcpy(n->mac,r.arp_ha.sa_data,6);return 1;
}
static int matches(const struct neighbour* n,const struct profile* p)
{
    return n->present && n->flags==(ATF_PERM|ATF_COM) && n->family==ARPHRD_ETHER && !memcmp(n->mac,p->mac,6);
}
static int inventory(int control,struct profile* p)
{
    struct ifreq r={0};memcpy(r.ifr_name,kInterface,sizeof(kInterface));
    if(ioctl(control,SIOCGIFINDEX,&r) || r.ifr_ifindex<=0){return 0;}p->index=r.ifr_ifindex;
    if(ioctl(control,SIOCGIFHWADDR,&r) || r.ifr_hwaddr.sa_family!=ARPHRD_ETHER){return 0;}
    memcpy(p->mac,r.ifr_hwaddr.sa_data,6);unsigned nonzero=0;
    for(unsigned i=0;i<6;++i){nonzero|=p->mac[i];}if(!nonzero || p->mac[0]&1){return 0;}
    if(ioctl(control,SIOCGIFADDR,&r) || r.ifr_addr.sa_family!=AF_INET){return 0;}
    struct in_addr expected;if(inet_pton(AF_INET,kCpuAddress,&expected)!=1){return 0;}
    return ((struct sockaddr_in*)&r.ifr_addr)->sin_addr.s_addr==expected.s_addr;
}
static int add_entry(int control,const struct profile* p)
{
    struct arpreq r;request(&r);r.arp_flags=ATF_PERM|ATF_COM;r.arp_ha.sa_family=ARPHRD_ETHER;memcpy(r.arp_ha.sa_data,p->mac,6);
    return !ioctl(control,SIOCSARP,&r);
}
/* Cooperating sessions serialize through the abstract socket. Read/check /
 * delete is not an atomic CAS against an unrelated administrator's write. */
static int restore(int control,int owned,const struct profile* p)
{
    if(!owned){return 1;}
    struct profile now;if(!inventory(control,&now) || now.index!=p->index || memcmp(now.mac,p->mac,6)){return 0;}
    struct neighbour n;if(!snapshot(control,&n)){return 0;}if(!n.present){return 1;}if(!matches(&n,p)){return 0;}
    struct arpreq r;request(&r);
    if(ioctl(control,SIOCDARP,&r) && errno!=ENXIO && errno!=ENOENT){return 0;}
    return snapshot(control,&n) && !n.present;
}
static int restore_journal(int control,int owned,const struct profile* profile,int journal)
{
    return restore(control,owned,profile) && (!owned || journal_clear(journal));
}
static int close_others(int life,int control,int lock,int journal)
{
    DIR* directory=opendir("/proc/self/fd");if(!directory){return 0;}int directory_fd=dirfd(directory);struct dirent* entry;
    while((entry=readdir(directory))){
        if(entry->d_name[0]=='.'){continue;}char* end;errno=0;long fd=strtol(entry->d_name,&end,10);
        if(errno || *end || fd<0 || fd>INT_MAX){closedir(directory);return 0;}
        if(fd!=1 && fd!=life && fd!=control && fd!=lock && fd!=journal && fd!=directory_fd){close((int)fd);}
    }
    return !closedir(directory);
}
static int drop_authority(int needs_cleanup)
{
    for(int cap=0;cap<64;++cap){
        if(prctl(PR_CAPBSET_DROP,cap,0,0,0) && errno!=EINVAL){return 0;}
    }
    if(prctl(PR_SET_KEEPCAPS,1,0,0,0) || setgroups(0,NULL) || setresgid(65534,65534,65534) || setresuid(65534,65534,65534)){return 0;}
    struct __user_cap_header_struct h={_LINUX_CAPABILITY_VERSION_3,0};struct __user_cap_data_struct caps[2]={{0}};
    unsigned mask=needs_cleanup?1u<<CAP_NET_ADMIN:0;caps[0].effective=caps[0].permitted=mask;
    if(syscall(SYS_capset,&h,caps) || prctl(PR_SET_KEEPCAPS,0,0,0,0) || prctl(PR_SET_DUMPABLE,0,0,0,0)){return 0;}
    memset(caps,0,sizeof(caps));
    return geteuid()==65534 && !syscall(SYS_capget,&h,caps) && caps[0].effective==mask &&
        caps[0].permitted==mask && !caps[0].inheritable && !caps[1].effective && !caps[1].permitted && !caps[1].inheritable;
}
static void event(const char* name,int owned,int success)
{
    char out[256];int n=snprintf(out,sizeof(out),"{\"event\":\"%s\",\"uid\":%lu,\"owned\":%s,\"success\":%s,\"rf_commands\":[]}\n",name,(unsigned long)geteuid(),owned?"true":"false",success?"true":"false");
    if(n>0 && n<(int)sizeof(out)){(void)write(1,out,(size_t)n);}
}
/* The watcher inherits the verified pre-mutation ownership and the lock.
 * The keeper alone owns its pipe writer, so keeper death triggers cleanup.
 * Both processes retain only NET_ADMIN; neither accepts cleanup requests. */
static pid_t cleanup_watch_start(int life,int control,int lock,int owned,
                                 const struct profile* profile,int journal,int* writer)
{
    int watch[2];if(pipe(watch)){return -1;}
    pid_t child=fork();
    if(child<0){close(watch[0]);close(watch[1]);return -1;}
    if(child==0){
        close(watch[1]);close(life);
        struct sigaction ignored={.sa_handler=SIG_IGN};sigemptyset(&ignored.sa_mask);
        if(sigaction(SIGTERM,&ignored,NULL) || sigaction(SIGINT,&ignored,NULL)){_exit(7);}
        for(;;){
            struct pollfd p={watch[0],0,0};int ready=poll(&p,1,-1);
            if(ready<0 && errno==EINTR){continue;}
            if(ready<0 || (ready>0 && lifetime_ended(p.revents))){break;}
        }
        int restored=restore_journal(control,owned,profile,journal);
        close(watch[0]);close(control);close(lock);close(journal);_exit(restored?0:7);
    }
    close(watch[0]);*writer=watch[1];return child;
}
static int cleanup_watch_finish(pid_t child,int writer)
{
    close(writer);
    /* A stalled cleanup keeps its lock; never kill the last cleanup owner. */
    for(unsigned attempt=0;attempt<200;++attempt){
        int status=0;pid_t got=waitpid(child,&status,WNOHANG);
        if(got==child){return WIFEXITED(status) && WEXITSTATUS(status)==0;}
        if(got<0 && errno!=EINTR){return 0;}
        struct timespec delay={0,10000000};nanosleep(&delay,NULL);
    }
    return 0;
}
int main(int argc,char** argv)
{
    if(argc!=2 || geteuid()!=0){return 2;}
    int life=lifetime_fd(argv[1]);if(life<0){return 2;}
    struct stat st;if(fstat(life,&st) || !S_ISFIFO(st.st_mode) || (fcntl(life,F_GETFL)&O_ACCMODE)!=O_RDONLY || fstat(1,&st) || !S_ISFIFO(st.st_mode)){return 2;}
    int flags=fcntl(1,F_GETFL);if(flags<0 || fcntl(1,F_SETFL,flags|O_NONBLOCK)){return 2;}
    struct sigaction action={.sa_handler=stop_signal};sigemptyset(&action.sa_mask);if(sigaction(SIGINT,&action,NULL) || sigaction(SIGTERM,&action,NULL) || signal(SIGPIPE,SIG_IGN)==SIG_ERR){return 2;}
    int lock=socket(AF_UNIX,SOCK_STREAM,0);if(lock<0){return 3;}
    struct sockaddr_un address={.sun_family=AF_UNIX};memcpy(address.sun_path,kLock,sizeof(kLock)-1);
    if(bind(lock,(struct sockaddr*)&address,offsetof(struct sockaddr_un,sun_path)+sizeof(kLock)-1)){event("busy",0,0);return 3;}
    int control=socket(AF_INET,SOCK_DGRAM,0);struct profile profile;struct neighbour before;
    if(control<0 || !inventory(control,&profile) || !snapshot(control,&before)){event("profile_rejected",0,0);return 4;}
    if(before.present && !matches(&before,&profile)){event("existing_entry_rejected",0,0);return 4;}
    unsigned char boot[36],record[kJournalSize];int journal=journal_open(),owned=0;
    if(journal<0 || !journal_boot_id(boot)){event("journal_unavailable",0,0);return 4;}
    journal_record(record,&profile,boot);
    if(!journal_admit(journal,record,before.present,&owned)){event("journal_rejected",0,0);return 4;}
    /* No network mutation until root and every unrelated capability are gone. */
    if(!close_others(life,control,lock,journal) || !drop_authority(owned)){event("authority_rejected",0,0);return 6;}
    struct neighbour after;
    if(!snapshot(control,&after) || after.present!=before.present || (after.present && !matches(&after,&profile))){event("entry_changed_before_setup",0,0);return 6;}
    if(owned && !journal_claim(journal,record)){event("journal_write_failed",owned,0);return 6;}
    int watch_writer=-1;pid_t watcher=cleanup_watch_start(life,control,lock,owned,&profile,journal,&watch_writer);
    if(watcher<0){event("cleanup_watch_failed",owned,restore_journal(control,owned,&profile,journal));return 6;}
    if(!before.present && !add_entry(control,&profile)){
        int restored=cleanup_watch_finish(watcher,watch_writer);event("setup_failed",owned,restored);return 5;
    }
    if(!snapshot(control,&after) || !matches(&after,&profile)){
        int restored=restore_journal(control,owned,&profile,journal);int watched=cleanup_watch_finish(watcher,watch_writer);event("constructor_failed",owned,restored && watched);return 6;
    }
    event("ready",owned,1);
    /* Observe writer lifetime without consuming payload. A filled pipe must
     * neither spin this privileged process nor delay cleanup after EOF. */
    int watcher_failed=0;
    while(!stopping){
        int status=0;pid_t got=waitpid(watcher,&status,WNOHANG);
        if(got==watcher || (got<0 && errno!=EINTR)){watcher_failed=1;break;}
        struct pollfd p={life,0,0};int ready=poll(&p,1,500);
        if(ready<0 && errno==EINTR){continue;}if(ready<0){break;}
        if(ready>0 && lifetime_ended(p.revents)){break;}
    }
    int restored=restore_journal(control,owned,&profile,journal);
    int watched=0;
    if(watcher_failed){close(watch_writer);}else{watched=cleanup_watch_finish(watcher,watch_writer);}
    event("complete",owned,restored && watched);
    struct __user_cap_header_struct h={_LINUX_CAPABILITY_VERSION_3,0};struct __user_cap_data_struct caps[2]={{0}};
    int retired=!syscall(SYS_capset,&h,caps);close(control);close(lock);close(life);close(journal);return restored && watched && retired?0:7;
}
