#define _GNU_SOURCE
/* Test-only descriptor setup inside the disposable namespace. */
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
int main(int argc,char** argv)
{
    if(argc!=2){return 2;}
    int bootstrap=!strcmp(argv[1],"bootstrap"),recover=!strcmp(argv[1],"recover"),copy=!strcmp(argv[1],"copy");
    if(!bootstrap && !recover && !copy && strcmp(argv[1],"forward")){return 2;}
    const char* names[]={"/fixture/release","/fixture/approved","/fixture/plan"};int pinned[3];
    for(unsigned i=0;i<3;++i){
        int fd=open(names[i],O_RDONLY|O_NOFOLLOW|O_CLOEXEC);pinned[i]=fd>=0?fcntl(fd,F_DUPFD_CLOEXEC,10):-1;
        if(fd>=0){close(fd);}if(pinned[i]<0){return 3;}
    }
    const int targets[]={0,3,4};
    for(unsigned i=0;i<3;++i){if(dup2(pinned[i],targets[i])<0){return 3;}close(pinned[i]);}
    char* path=bootstrap || copy?"/fixture/bootstrap":"/var/lib/smartsdr/aetherpd-installation/worker";
    char* args[]={path,bootstrap?"--bootstrap":recover?"--recover":NULL,NULL};
    char* env[]={"PATH=/usr/bin:/bin",NULL};execve(path,args,env);return 4;
}
