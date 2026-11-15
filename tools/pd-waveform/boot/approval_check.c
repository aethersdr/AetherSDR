#define _GNU_SOURCE
#include "pd_approval.h"
#include "pd_identity.h"
#include <fcntl.h>
#include <unistd.h>
int main(int argc,char** argv)
{
    if(argc!=3){return 2;}
    int fd=open(argv[1],O_RDONLY|O_NONBLOCK|O_NOFOLLOW|O_CLOEXEC);
    if(fd<0){return 1;}
    unsigned char record[PD_APPROVAL_SIZE];struct pd_approval approval;struct pd_identity identity;
    int ok=pd_approval_read(fd,record,&approval) && pd_identity_parse(argv[2],&identity) &&
        pd_identity_matches(&identity,approval.profile,approval.firmware,approval.serial);
    close(fd);return ok?0:1;
}
