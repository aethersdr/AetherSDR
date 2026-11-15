#include "pd_identity.h"
#include <stdio.h>
#include <string.h>
static int check(int condition){if(!condition){fputs("identity assertion failed\n",stderr);}return condition;}
int main(void)
{
    struct pd_identity id;
    if(!check(pd_identity_parse("R970201|0|model=\"FLEX-8600\",chassis_serial=\"2125-1213-8600-8895\",name=\"a,b\",software_ver=4.3.16.42034",&id)) ||
       !check(pd_identity_matches(&id,1,"4.3.16.42034","2125-1213-8600-8895")) ||
       pd_identity_matches(&id,2,"4.3.16.42034","2125-1213-8600-8895") ||
       pd_identity_matches(&id,1,"4.3.16.42035","2125-1213-8600-8895") ||
       pd_identity_matches(&id,1,"4.3.16.42034","2125-1213-8600-8896") ||
       pd_identity_matches(&id,1,"4.3.16.42034","")){return 1;}
    const char* bad[]={"R970201|1|model=FLEX-8600,chassis_serial=x,software_ver=4.3.16.42034",
        "R970202|0|model=FLEX-8600,chassis_serial=x,software_ver=4.3.16.42034",
        "R970201|0|model=FLEX-8600,software_ver=4.3.16.42034",
        "R970201|0|model=FLEX-8600,model=FLEX-6300,chassis_serial=x,software_ver=4.3.16.42034",
        "R970201|0|model=FLEX-8600,chassis_serial=\"bad serial\",software_ver=4.3.16.42034",
        "R970201|0|model=FLEX-8600,chassis_serial=x,software_ver=4.3.16.42034,",
        "R970201|0|model=FLEX-8600,chassis_serial=x,software_ver=4.3.16.42034,name=\"broken"};
    for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);++i){if(pd_identity_parse(bad[i],&id)){return 1;}}
    if(!pd_identity_parse("R970201|0|software_ver=4.2.18.41174,model=FLEX-6300,chassis_serial=6300-test",&id) ||
        !pd_identity_matches(&id,2,"4.2.18.41174","6300-test")){return 1;}
    if(!pd_identity_parse("R970201|0|model=FLEX-8600,chassis_serial=fixture,software_ver=4.3.16.42034,ip=192.168.50.100",&id) ||
        strcmp(id.address,"192.168.50.100")){return 1;}
    const char* addresses[]={"0192.168.50.100","192.168.50.999","192.168.50.100,ip=192.168.50.101","","::1","1.2.3.4/24"};
    for(unsigned i=0;i<sizeof(addresses)/sizeof(addresses[0]);++i){
        char reply[256];snprintf(reply,sizeof(reply),"R970201|0|model=FLEX-8600,chassis_serial=fixture,software_ver=4.3.16.42034,ip=%s",addresses[i]);
        if(pd_identity_parse(reply,&id)){return 1;}
    }
    puts("identity boundary cases passed");return 0;
}
