#define PD_SERVICE_MAIN service_program_main
#include "pd_service_launch.c"
#undef PD_SERVICE_MAIN
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <sys/file.h>
int main(void)
{
    char file[]="/tmp/pd-service-lock-XXXXXX";int writer=mkstemp(file);assert(writer>=0);
    int lease=open(file,O_RDONLY|O_CLOEXEC),other=open(file,O_RDONLY|O_CLOEXEC);
    assert(lease>=0 && other>=0 && !unlink(file) && !flock(lease,LOCK_SH|LOCK_NB));
    pid_t retained=fork();assert(retained>=0);
    if(!retained){
        assert(service_keep_descriptors(0,1,lease));
        assert((fcntl(lease,F_GETFL)&O_ACCMODE)==O_RDONLY);
        assert(fcntl(lease,F_GETFD)&FD_CLOEXEC);
        assert(fcntl(other,F_GETFD)<0 && errno==EBADF);
        assert(write(lease,"x",1)<0 && errno==EBADF);_exit(0);
    }
    int status;assert(waitpid(retained,&status,0)==retained && WIFEXITED(status) && !WEXITSTATUS(status));
    assert(flock(writer,LOCK_EX|LOCK_NB)<0 && errno==EWOULDBLOCK);
    close(lease);assert(!flock(writer,LOCK_EX|LOCK_NB));close(writer);close(other);
    assert(service_uuid("0D3EA0E8-B052-4AC6-9D32-D96918634469"));
    assert(!service_uuid("0D3EA0E8/B052-4AC6-9D32-D96918634469"));
    assert(!service_uuid("0D3EA0E8-B052-4AC6-9D32-D96918634469;"));
    assert(!service_uuid(NULL));
    int out[2],err[2];assert(!pipe(out) && !pipe(err));
    pid_t child=fork();assert(child>=0);
    if(!child){
        close(out[0]);close(err[0]);char block[4096];memset(block,'x',sizeof(block));
        for(unsigned i=0;i<64;++i){assert(write(out[1],block,sizeof(block))==(ssize_t)sizeof(block));assert(write(err[1],block,sizeof(block))==(ssize_t)sizeof(block));}
        _exit(7);
    }
    close(out[1]);close(err[1]);
    int sink[2];assert(!pipe(sink));assert(service_nonblock(sink[1]));
    char full[4096];memset(full,'x',sizeof(full));
    while(write(sink[1],full,sizeof(full))>0){}
    assert(errno==EAGAIN);int flags=fcntl(sink[1],F_GETFL);assert(flags>=0);
    assert(!fcntl(sink[1],F_SETFL,flags&~O_NONBLOCK));
    assert(dup2(sink[1],1)==1 && dup2(sink[1],2)==2);close(sink[1]);
    /* A full service-manager log sink must not stop draining either child pipe. */
    alarm(3);assert(service_relay(child,out[0],err[0])==7);alarm(0);close(sink[0]);
    int null=open("/dev/null",O_WRONLY);assert(null>=0);assert(dup2(null,1)==1 && dup2(null,2)==2);close(null);
    assert(waitpid(child,NULL,WNOHANG)==-1 && errno==ECHILD);
    assert(!pipe(out) && !pipe(err));child=fork();assert(child>=0);
    if(!child){close(out[0]);close(err[0]);signal(SIGTERM,SIG_DFL);for(;;){pause();}}
    close(out[1]);close(err[1]);service_stopping=1;
    assert(service_relay(child,out[0],err[0])==128+SIGTERM);
    assert(waitpid(child,NULL,WNOHANG)==-1 && errno==ECHILD);
    return 0;
}
