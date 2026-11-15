#define _GNU_SOURCE
#include "pd_approval.h"
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int approval_text(const unsigned char* bytes,size_t capacity,int firmware)
{
    size_t length=0;unsigned components=1,digits=0;
    while(length<capacity && bytes[length]){
        unsigned char c=bytes[length++];
        if(c>='0' && c<='9'){++digits;continue;}
        if(firmware){
            if(c!='.' || !digits || components>=4){return 0;}
            ++components;digits=0;
        }else if(!((c>='A' && c<='Z') || (c>='a' && c<='z') || c=='-' || c=='.')){return 0;}
    }
    if(!length || length==capacity || (firmware && (components!=4 || !digits))){return 0;}
    for(size_t i=length;i<capacity;++i){if(bytes[i]){return 0;}}
    return 1;
}
int pd_approval_parse(const unsigned char* record,size_t size,struct pd_approval* approval)
{
    memset(approval,0,sizeof(*approval));
    if(size!=PD_APPROVAL_SIZE || memcmp(record,"PDAPRV02",8) || record[8] || record[9] || record[10] ||
        (record[11]!=1 && record[11]!=2) || record[12] || record[13] || record[14] || record[15] ||
        !approval_text(record+48,28,1) || !approval_text(record+76,64,0)){return 0;}
    approval->profile=record[11];memcpy(approval->digest,record+16,32);
    memcpy(approval->firmware,record+48,28);memcpy(approval->serial,record+76,64);return 1;
}
int pd_approval_read(int fd,unsigned char record[PD_APPROVAL_SIZE],struct pd_approval* approval)
{
    struct stat st;
#ifdef PD_APPROVAL_TEST
    uid_t owner=getuid();
#else
    uid_t owner=0;
#endif
    if(fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_uid!=owner || st.st_nlink!=1 ||
        (st.st_mode&07777)!=0600 || st.st_size!=PD_APPROVAL_SIZE){return 0;}
    size_t used=0;
    while(used<PD_APPROVAL_SIZE){
        ssize_t n=pread(fd,record+used,PD_APPROVAL_SIZE-used,(off_t)used);
        if(n<0 && errno==EINTR){continue;}if(n<=0){return 0;}used+=(size_t)n;
    }
    return pd_approval_parse(record,used,approval);
}
