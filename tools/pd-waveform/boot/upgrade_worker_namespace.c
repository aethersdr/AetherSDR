#define _GNU_SOURCE
/* Test-only inherited descriptor setup inside the disposable namespace. */
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
static int copy_input(const char* source,const char* destination)
{
    int input=open(source,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    int output=input>=0?open(destination,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600):-1;
    int ok=input>=0 && output>=0;unsigned total=0;char bytes[8192];
    while(ok){
        ssize_t count=read(input,bytes,sizeof(bytes));if(count<0 && errno==EINTR){continue;}
        if(count<0){ok=0;break;}if(!count){break;}
        total+=(unsigned)count;if(total>18*1024*1024){ok=0;break;}
        ssize_t offset=0;
        while(offset<count){ssize_t n=write(output,bytes+offset,(size_t)(count-offset));
            if(n<0 && errno==EINTR){continue;}if(n<=0){ok=0;break;}offset+=n;}
    }
    if(output>=0){ok=ok && !fsync(output);if(close(output)){ok=0;}}
    if(input>=0){close(input);}return ok;
}
int main(void)
{
    int persistent=!access("/persistent-inputs",F_OK);
    if(!persistent && (!copy_input("/fixture/previous.release","/release-inputs/previous.release") ||
        !copy_input("/fixture/candidate.release","/release-inputs/candidate.release"))){return 3;}
    const char* const names[]={"/fixture/approved",persistent?"/persistent-inputs/previous.release":"/release-inputs/previous.release",
        persistent?"/persistent-inputs/candidate.release":"/release-inputs/candidate.release"};
    int pinned[3];
    for(unsigned i=0;i<3;++i){
        int fd=open(names[i],O_RDONLY|O_NOFOLLOW|O_CLOEXEC);pinned[i]=fd>=0?fcntl(fd,F_DUPFD_CLOEXEC,10):-1;
        if(fd>=0){close(fd);}if(pinned[i]<0){return 3;}
    }
    for(unsigned i=0;i<3;++i){if(dup2(pinned[i],3+(int)i)<0){return 3;}close(pinned[i]);}
    char* const args[]={"/fixture/copy","--bootstrap",NULL};char* const env[]={"PATH=/usr/bin:/bin",NULL};
    execve(args[0],args,env);return 4;
}
