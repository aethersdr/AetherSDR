/* Read-only native carrier probe, never shipped as an installer. */
#define _GNU_SOURCE
#include "pd_carrier.h"
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
int main(int argc,char** argv)
{
    int after=argc==2 && !strcmp(argv[1],"--after-exec");
    if(argc!=1 && !after){return 2;}
    int entered=after || pd_carrier_enter(8);close(8);
    printf("{\"entered\":%s,\"pid\":%ld",entered?"true":"false",(long)getpid());
    if(entered){
        FILE* status=fopen("/proc/self/status","r");char line[256];
        if(!status){puts("}");return 4;}
        while(fgets(line,sizeof(line),status)){
            char* colon=strchr(line,':');if(!colon){continue;}*colon=0;
            if(!strcmp(line,"CapEff") || !strcmp(line,"CapPrm") || !strcmp(line,"CapBnd") || !strcmp(line,"NoNewPrivs")){
                char value[64];if(sscanf(colon+1,"%63s",value)==1){printf(",\"%s\":\"%s\"",line,value);}
            }
        }
        fclose(status);
    }
    puts("}");fflush(stdout);
    if(entered && !after){execl("/proc/self/exe","carrier-check","--after-exec",(char*)NULL);return 5;}
    return entered?0:3;
}
