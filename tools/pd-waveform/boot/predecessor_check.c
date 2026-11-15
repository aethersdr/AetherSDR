#define _GNU_SOURCE
#include "pd_predecessor.h"
#include "pd_recovery.h"
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
int main(int argc,char** argv)
{
    if(argc<1 || argc>3){return 2;}
    if(argc==2 && !strcmp(argv[1],"--image-format")){puts(pd_predecessor_image_format());return 0;}
    if(argc==2 && (!strcmp(argv[1],"--format") || !strcmp(argv[1],"--restored-format"))){
        char format[4096];if(!pd_predecessor_format(!strcmp(argv[1],"--restored-format"),format,sizeof(format))){return 2;}
        puts(format);return 0;
    }
    unsigned char bytes[8193];size_t size=fread(bytes,1,sizeof(bytes),stdin);
    if(ferror(stdin)){return 2;}struct pd_predecessor p;
    if(argc==2 && (!strcmp(argv[1],"--verify-image") || !strcmp(argv[1],"--verify-overlay"))){
        struct pd_recovery recipe;
        if(!pd_recovery_parse(bytes,size,&recipe)){return 1;}
        return (!strcmp(argv[1],"--verify-overlay")?pd_predecessor_overlay_verify(&recipe.migration.predecessor):
            pd_predecessor_image_verify(&recipe.migration.predecessor))?0:1;
    }
    if(argc==2 && !strcmp(argv[1],"--recipe")){
        struct pd_recovery recipe;if(!pd_recovery_parse(bytes,size,&recipe)){return 1;}
        printf("%s\n%s\n",recipe.radio_address,recipe.hostname);return 0;
    }
    if(argc==3 && !strcmp(argv[1],"--read-recipe")){
        if(size!=PD_MIGRATION_SIZE){return 1;}
        int store=open(argv[2],O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);struct pd_recovery recipe;
        int ok=pd_recovery_read(store,bytes,&recipe);if(store>=0){close(store);}return ok?0:1;
    }
    if(argc==3 && (!strcmp(argv[1],"--restored") || !strcmp(argv[1],"--image-parse"))){
        FILE* file=fopen(argv[2],"rb");if(!file){return 2;}
        unsigned char record[PD_RECOVERY_SIZE+1];size_t used=fread(record,1,sizeof(record),file);
        int ok=!ferror(file);fclose(file);struct pd_recovery recipe;
        if(!ok || !pd_recovery_parse(record,used,&recipe)){return 1;}
        if(!strcmp(argv[1],"--image-parse")){char layer[65];return pd_predecessor_image_parse(bytes,size,&recipe.migration.predecessor,layer)?0:1;}
        return pd_predecessor_parse_restored(bytes,size,&recipe,&p)?0:1;
    }
    if(argc>2 || (argc==2 && strcmp(argv[1],"--status") && strcmp(argv[1],"--mount-path"))){return 2;}
    if(argc==2 && !strcmp(argv[1],"--mount-path")){char layer[65];return pd_predecessor_mount_path(bytes,size,layer)?0:1;}
    int ok=argc==2?pd_predecessor_parse_status(bytes,size,&p):pd_predecessor_parse(bytes,size,&p);
    if(ok){puts(p.radio_address);}return ok?0:1;
}
