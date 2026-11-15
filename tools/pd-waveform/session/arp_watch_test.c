#define PD_ARP_JOURNAL_TEST
#define ioctl test_ioctl
#define main fixed_arp_program_main
#include "pd_fixed_arp.c"
#undef main
#undef ioctl
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdarg.h>
#include <sys/mman.h>

struct simulated_network { int present,deleted,foreign; };
static struct simulated_network* network;
static int journal_fixture(void)
{
    char path[]="/tmp/pd-arp-watch-XXXXXX";int fd=mkstemp(path);assert(fd>=0 && !unlink(path));
    unsigned char record[kJournalSize]={0};memcpy(record,"PDARP001",8);
    assert(journal_claim(fd,record));return fd;
}
#ifdef __GLIBC__
int test_ioctl(int fd,unsigned long op,...)
#else
int test_ioctl(int fd,int op,...)
#endif
{
    (void)fd;va_list args;va_start(args,op);void* data=va_arg(args,void*);va_end(args);
    struct ifreq* iface=data;struct arpreq* arp=data;
    const unsigned char mac[6]={2,1,2,3,4,5};
    switch(op){
    case SIOCGIFINDEX:iface->ifr_ifindex=5;return 0;
    case SIOCGIFHWADDR:iface->ifr_hwaddr.sa_family=ARPHRD_ETHER;memcpy(iface->ifr_hwaddr.sa_data,mac,6);return 0;
    case SIOCGIFADDR:iface->ifr_addr.sa_family=AF_INET;inet_pton(AF_INET,kCpuAddress,&((struct sockaddr_in*)&iface->ifr_addr)->sin_addr);return 0;
    case SIOCGARP:
        if(!network->present){errno=ENXIO;return -1;}
        arp->arp_flags=ATF_PERM|ATF_COM;arp->arp_ha.sa_family=ARPHRD_ETHER;memcpy(arp->arp_ha.sa_data,mac,6);
        if(network->foreign){arp->arp_ha.sa_data[5]^=1;}return 0;
    case SIOCDARP:assert(network->present && !network->foreign);network->present=0;++network->deleted;return 0;
    default:assert(0);return -1;
    }
}
static void check(int owned,int present,int foreign,int success,int deletes)
{
    *network=(struct simulated_network){present,0,foreign};
    int life[2],lock[2],writer=-1;assert(!pipe(life) && !pipe(lock));
    struct profile p={.mac={2,1,2,3,4,5},.index=5};
    int journal=journal_fixture();pid_t child=cleanup_watch_start(life[0],-1,lock[1],owned,&p,journal,&writer);assert(child>0);
    assert(cleanup_watch_finish(child,writer)==success);
    assert(network->deleted==deletes);
    struct stat st;assert(!fstat(journal,&st) && st.st_size==(owned && success?0:kJournalSize));close(journal);
    close(life[0]);close(life[1]);close(lock[0]);close(lock[1]);
}
static void parent_death(void)
{
    assert(!prctl(PR_SET_CHILD_SUBREAPER,1,0,0,0));
    *network=(struct simulated_network){1,0,0};
    int identity[2],lock[2];assert(!pipe(identity) && !pipe(lock));
    pid_t parent=fork();assert(parent>=0);
    if(!parent){
        close(identity[0]);close(lock[0]);int life[2],writer=-1;assert(!pipe(life));
        struct profile p={.mac={2,1,2,3,4,5},.index=5};
        int journal=journal_fixture();pid_t watcher=cleanup_watch_start(life[0],-1,lock[1],1,&p,journal,&writer);assert(watcher>0);
        assert(write(identity[1],&watcher,sizeof(watcher))==sizeof(watcher));
        for(;;){pause();}
    }
    close(identity[1]);close(lock[1]);pid_t watcher=0;assert(read(identity[0],&watcher,sizeof(watcher))==sizeof(watcher));
    assert(network->present && network->deleted==0);
    assert(!kill(parent,SIGKILL));int status;assert(waitpid(parent,&status,0)==parent && WIFSIGNALED(status));
    struct pollfd ready={lock[0],POLLIN,0};assert(poll(&ready,1,2000)==1 && (ready.revents&POLLHUP));
    assert(waitpid(watcher,&status,0)==watcher && WIFEXITED(status) && WEXITSTATUS(status)==0);
    assert(!network->present && network->deleted==1);close(identity[0]);close(lock[0]);
}
static void both_owners_die(void)
{
    char path[]="/tmp/pd-arp-restart-XXXXXX";int journal=mkstemp(path);assert(journal>=0);
    unsigned char boot[36],record[kJournalSize];struct profile profile={.mac={2,1,2,3,4,5},.index=5};
    assert(journal_boot_id(boot));journal_record(record,&profile,boot);assert(journal_claim(journal,record));
    *network=(struct simulated_network){1,0,0};
    int identity[2],lock[2];assert(!pipe(identity) && !pipe(lock));pid_t parent=fork();assert(parent>=0);
    if(!parent){
        assert(!setpgid(0,0));close(identity[0]);close(lock[0]);int life[2],writer=-1;assert(!pipe(life));
        pid_t watcher=cleanup_watch_start(life[0],-1,lock[1],1,&profile,journal,&writer);assert(watcher>0);
        assert(write(identity[1],&watcher,sizeof(watcher))==sizeof(watcher));for(;;){pause();}
    }
    close(identity[1]);close(lock[1]);pid_t watcher=0;assert(read(identity[0],&watcher,sizeof(watcher))==sizeof(watcher));
    assert(!kill(-parent,SIGKILL));int status;
    assert(waitpid(parent,&status,0)==parent && WIFSIGNALED(status));
    assert(waitpid(watcher,&status,0)==watcher && WIFSIGNALED(status));
    assert(network->present && !network->deleted);
    pid_t successor=fork();assert(successor>=0);
    if(!successor){
        close(journal);int reopened=open(path,O_RDWR|O_NOFOLLOW),owned=0;
        assert(journal_admit(reopened,record,network->present,&owned) && owned);
        assert(restore_journal(-1,owned,&profile,reopened));close(reopened);_exit(0);
    }
    assert(waitpid(successor,&status,0)==successor && WIFEXITED(status) && WEXITSTATUS(status)==0);
    assert(!network->present && network->deleted==1);struct stat st;assert(!fstat(journal,&st) && !st.st_size);
    close(journal);assert(!unlink(path));close(identity[0]);close(lock[0]);
}
int main(void)
{
    alarm(10);
    network=mmap(NULL,sizeof(*network),PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);assert(network!=MAP_FAILED);
    check(1,1,0,1,1);check(1,0,0,1,0);check(0,1,0,1,0);check(1,1,1,0,0);
    parent_death();both_owners_die();munmap(network,sizeof(*network));
    puts("PASS: owned cleanup, borrowed/changed protection, parent SIGKILL and both-owner SIGKILL with fresh-process journal recovery; simulated ARP, no network mutation");
}
