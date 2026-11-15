#define PD_FIRST_REQUEST_TEST
#define main inherited_first_main
#include "pd_first_request.c"
#undef main
#include <stdlib.h>
int main(int argc,char** argv)
{
    if(argc!=3){return 2;}alarm(15);
    int public=open(argv[1],O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    int ram=open("/dev/shm",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    unsigned char record[120],secret[120];
    if(public<0 || ram<0 || pread(public,record,120,0)!=120){return 2;}close(public);
    memcpy(secret,"PDXFER01",8);memset(secret+8,42,32);memcpy(secret+40,record+8,80);
    int config=-1,writer=pd_upload_memory_file(ram,&config);close(ram);
    if(writer<0 || write(writer,secret,120)!=120){return 2;}close(writer);
    int broker=broker_open(record);if(broker<0){return 2;}
    puts("ready");fflush(stdout);unsigned roles=0;
    for(unsigned i=0;i<(unsigned)atoi(argv[2]);++i){
        int channel=accept4(broker,NULL,NULL,SOCK_CLOEXEC);
        if(channel<0){return 3;}broker_reply(channel,config,record,&roles);close(channel);
    }
    close(config);close(broker);return 0;
}
