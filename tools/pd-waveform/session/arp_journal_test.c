#define PD_ARP_JOURNAL_TEST
#define main fixed_arp_program_main
#include "pd_fixed_arp.c"
#undef main
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>

static int fixture(char path[64])
{
    strcpy(path,"/tmp/pd-arp-journal-XXXXXX");int fd=mkstemp(path);assert(fd>=0);return fd;
}
static void rewrite(int fd,const unsigned char* bytes,size_t size)
{
    assert(!ftruncate(fd,0));assert(pwrite(fd,bytes,size,0)==(ssize_t)size);assert(!fsync(fd));
}
int main(void)
{
    alarm(10);char path[64];int fd=fixture(path),owned=0;unsigned char record[kJournalSize],boot[36];
    struct profile profile={.mac={2,1,2,3,4,5},.index=5};
    assert(journal_boot_id(boot));journal_record(record,&profile,boot);
    assert(journal_admit(fd,record,0,&owned) && owned);
    assert(journal_admit(fd,record,1,&owned) && !owned);
    assert(journal_claim(fd,record));
    /* Both owners can die after the ARP add. A new process must reclaim the
     * protected intent instead of treating that entry as borrowed. */
    pid_t child=fork();assert(child>=0);
    if(!child){int acquired=0;_exit(journal_admit(fd,record,1,&acquired) && acquired?0:1);}
    int status;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
    assert(journal_admit(fd,record,0,&owned) && owned);
    assert(journal_clear(fd));assert(journal_admit(fd,record,1,&owned) && !owned);
    for(size_t length=1;length<kJournalSize;++length){
        rewrite(fd,record,length);assert(journal_admit(fd,record,0,&owned) && owned);
        assert(!journal_admit(fd,record,1,&owned));
    }
    rewrite(fd,record,kJournalSize);
    for(unsigned i=0;i<kJournalSize;++i){
        unsigned char changed[kJournalSize];memcpy(changed,record,sizeof(changed));changed[i]^=1;
        assert(!journal_admit(fd,changed,0,&owned) && !journal_admit(fd,changed,1,&owned));
    }
    assert(!fchmod(fd,0644));assert(!journal_admit(fd,record,0,&owned));assert(!fchmod(fd,0600));
    char second[80];snprintf(second,sizeof(second),"%s.link",path);assert(!link(path,second));
    assert(!journal_admit(fd,record,0,&owned));assert(!unlink(second));
    assert(!ftruncate(fd,kJournalSize+1));assert(!journal_admit(fd,record,0,&owned));
    assert(journal_clear(fd));assert(journal_admit(fd,record,0,&owned) && owned);
    close(fd);assert(!unlink(path));
    puts("PASS: absent claim, matching borrowed preservation, restart-owned claim, clear, every partial prefix, profile/boot mismatch, mode, hardlink and size boundaries");
}
