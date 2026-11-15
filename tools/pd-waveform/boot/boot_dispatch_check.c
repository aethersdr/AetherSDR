/* Native fixture: real dispatch and preparation in a disposable descriptor root.
 * Only radio identity is substituted; no payload is executed. */
#define PD_BOOT_MAIN inherited_boot_entry
#include "pd_boot.c"
#undef PD_BOOT_MAIN
int __wrap_pd_boot_identity_wait(int store,struct pd_identity* identity,int cold)
{
    (void)store;(void)cold;
    return pd_identity_parse("R970201|0|model=FLEX-8600,software_ver=4.3.16.42034,chassis_serial=fixture-8600",identity);
}
int main(int argc,char** argv)
{
    if(argc!=2 || getuid()!=0 || geteuid()!=0 || !kBundleArch){return 2;}
    int root=open(argv[1],O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    int store=root>=0?boot_directory(root,"store"):-1;
    int result=store>=0?boot_dispatch(root,store,0):3;
    if(store>=0){close(store);}if(root>=0){close(root);}return result;
}
