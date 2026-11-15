#define _GNU_SOURCE
#include "pd_transport.h"
#include "monocypher.h"
#include <fcntl.h>
#include <linux/capability.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>
int main(int argc,char** argv)
{
    int relay=argc==2 && !strcmp(argv[1],"--relay");
    uid_t owner=relay?0:78666;
    if((argc!=1 && !relay) || getuid()!=owner || geteuid()!=owner){return 2;}
    struct __user_cap_header_struct header={_LINUX_CAPABILITY_VERSION_3,0};struct __user_cap_data_struct caps[2];
    if(syscall(SYS_capget,&header,caps)){return 3;}
    for(unsigned i=0;i<2;++i){if(caps[i].effective || caps[i].permitted || caps[i].inheritable){return 3;}}
    unsigned char key[32],receipt[80];if(!pd_transport_config(3,owner,key,receipt)){return 3;}
    close(3);int root=relay?8:open("/",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    int files[5]={root,0,4,5,6};
    int sent=root>=0 && pd_transport_send(9,0,key,receipt,files);
    crypto_wipe(key,sizeof(key));if(root>=0){close(root);}
    if(relay){
        int result=3;int completed=sent && pd_transport_result_receive(9,&result);close(9);
        printf("{\"descriptors_sent\":%s,\"completed\":%s,\"delivery_result\":%d}\n",
            sent?"true":"false",completed?"true":"false",result);
        return completed?result:6;
    }
    close(9);
    printf("{\"descriptors_sent\":%s}\n",sent?"true":"false");return sent?0:3;
}
