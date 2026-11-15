#include "pd_predecessor.h"
#include <stdio.h>
int main(int argc,char** argv)
{
    (void)argv;if(argc!=1){return 2;}
#if !defined(__aarch64__)
    return 2;
#else
    struct pd_predecessor p;if(!pd_predecessor_probe(&p)){return 3;}
    printf("{\"container_id\":\"%s\",\"image\":\"%s\",\"pid\":%ld,\"executable_blake2b256\":\"",p.container,p.image,(long)p.pid);
    for(unsigned i=0;i<32;++i){printf("%02x",p.executable[i]);}
    puts("\",\"read_only\":true,\"migration_authorized\":false}");return 0;
#endif
}
