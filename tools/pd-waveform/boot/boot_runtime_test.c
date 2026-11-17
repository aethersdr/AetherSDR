/* Fixed boot-directory policy with injected syscalls; no filesystem or radio. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>

int fixture_openat(int parent,const char* name,int flags,...);
int fixture_stat(int fd,struct stat* info);
int fixture_statfs(int fd,struct statfs* info);
int fixture_mkdirat(int fd,const char* name,mode_t mode);
int fixture_chmod(int fd,mode_t mode);
int fixture_close(int fd);

#define openat fixture_openat
#define fstat fixture_stat
#define fstatfs fixture_statfs
#define mkdirat fixture_mkdirat
#define fchmod fixture_chmod
#define close fixture_close
#define PD_BOOT_MAIN unused_boot_entry
#include "pd_boot.c"
#undef close
#undef fchmod
#undef mkdirat
#undef fstatfs
#undef fstat
#undef openat

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>

static struct {
    unsigned long parent_type,child_type;
    int parent_error,child_error,existing,opened,created,changed,closed_parent,closed_child;
} fixture;

int fixture_openat(int parent,const char* name,int flags,...)
{
    assert(flags==(O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC));
    if(parent==40){assert(!strcmp(name,"run"));return 41;}
    assert(parent==41 && !strcmp(name,"aetherpd"));++fixture.opened;return 42;
}
int fixture_stat(int fd,struct stat* info)
{
    assert(fd==41 || fd==42);memset(info,0,sizeof(*info));info->st_mode=S_IFDIR|0755;return 0;
}
int fixture_statfs(int fd,struct statfs* info)
{
    assert(fd==41 || fd==42);
    int error=fd==41?fixture.parent_error:fixture.child_error;
    memset(info,0,sizeof(*info));info->f_type=fd==41?fixture.parent_type:fixture.child_type;
    if(error){errno=error;return -1;}return 0;
}
int fixture_mkdirat(int fd,const char* name,mode_t mode)
{
    assert(fd==41 && !strcmp(name,"aetherpd") && mode==0755);
    assert(fixture.parent_type==0x01021994UL && !fixture.parent_error);
    ++fixture.created;if(fixture.existing){errno=EEXIST;return -1;}return 0;
}
int fixture_chmod(int fd,mode_t mode)
{
    assert(fd==42 && mode==0755 && fixture.child_type==0x01021994UL && !fixture.child_error);
    ++fixture.changed;return 0;
}
int fixture_close(int fd)
{
    assert(fd==41 || fd==42);if(fd==41){++fixture.closed_parent;}else{++fixture.closed_child;}return 0;
}
static void reset(void)
{
    memset(&fixture,0,sizeof(fixture));fixture.parent_type=fixture.child_type=0x01021994UL;
}
int main(void)
{
    for(unsigned i=0;i<3;++i){
        reset();fixture.parent_type=i==0?0xef53UL:i==1?0x794c7630UL:0;
        assert(boot_runtime(40)==-1);
        assert(!fixture.created && !fixture.opened && !fixture.changed && fixture.closed_parent==1);
    }
    reset();fixture.parent_error=EIO;
    assert(boot_runtime(40)==-1 && !fixture.created && !fixture.opened && fixture.closed_parent==1);
    reset();assert(boot_runtime(40)==42);
    assert(fixture.created==1 && fixture.opened==1 && fixture.changed==1 && fixture.closed_parent==1 && !fixture.closed_child);
    reset();fixture.existing=1;assert(boot_runtime(40)==42);
    assert(fixture.created==1 && fixture.opened==1 && !fixture.changed && fixture.closed_parent==1 && !fixture.closed_child);
    reset();fixture.existing=1;fixture.child_type=0xef53;
    assert(boot_runtime(40)==-1 && !fixture.changed && fixture.closed_parent==1 && fixture.closed_child==1);
    reset();fixture.existing=1;fixture.child_error=EIO;
    assert(boot_runtime(40)==-1 && !fixture.changed && fixture.closed_parent==1 && fixture.closed_child==1);
    return 0;
}
