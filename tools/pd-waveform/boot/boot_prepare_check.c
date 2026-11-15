/* Native fixture: exercise boot preparation only in caller-owned temporary FDs. */
#define PD_BOOT_MAIN inherited_boot_entry
#include "pd_boot.c"
#undef PD_BOOT_MAIN
int main(int argc,char** argv)
{
    if((argc!=4 && argc!=5) || getuid()!=0 || geteuid()!=0 || !kBundleArch){return 2;}
    int store=open(argv[1],O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    int runtime=open(argv[2],O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    int legacy=!strcmp(argv[3],"legacy");
    int payload=legacy?-1:open(argv[3],O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    unsigned char key[32];int key_ok=argc==4;
    if(argc==5){
        int fd=open(argv[4],O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);struct stat st;
        key_ok=fd>=0 && !fstat(fd,&st) && S_ISREG(st.st_mode) && st.st_size==32 && pread(fd,key,32,0)==32;
        if(fd>=0){close(fd);}
    }
    int ok=store>=0 && runtime>=0 && (legacy || payload>=0) && key_ok &&
        (argc==5?boot_prepare_key(store,runtime,payload,key):boot_prepare(store,runtime,payload));
    if(payload>=0){close(payload);}if(runtime>=0){close(runtime);}if(store>=0){close(store);}
    return ok?0:1;
}
