#define PD_FIXED_RUNTIME_DIRECTORY
#define main managed_directory_inherited_main
#include "../session/pd_shim_launch.c"
#undef main
int main(int argc,char** argv)
{
    (void)argv;if(argc!=1 || getuid()!=0 || geteuid()!=0){return 2;}
    char path[PATH_MAX];int directory=executable_directory(path);
    if(directory<0){return 3;}
    int program=open_program(directory,kShim);close(directory);
    if(program<0){return 4;}
    char bytes[64];ssize_t count=read(program,bytes,sizeof(bytes));close(program);
    if(count<4 || write(1,bytes+4,(size_t)(count-4))!=count-4){return 5;}return 0;
}
