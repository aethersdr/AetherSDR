#include "pd_upgrade.h"
#include <string.h>

int pd_upgrade_parse(const unsigned char* bytes,size_t size,struct pd_upgrade* upgrade)
{
    memset(upgrade,0,sizeof(*upgrade));
    if(size!=PD_UPGRADE_APPROVAL_SIZE || memcmp(bytes,"PDUPGR01",8) ||
        !pd_approval_parse(bytes+8,PD_APPROVAL_SIZE,&upgrade->previous) ||
        !pd_approval_parse(bytes+8+PD_APPROVAL_SIZE,PD_APPROVAL_SIZE,&upgrade->candidate)){return 0;}
    const struct pd_approval* old=&upgrade->previous;const struct pd_approval* next=&upgrade->candidate;
    if(old->profile!=next->profile || strcmp(old->serial,next->serial) || strcmp(old->firmware,next->firmware) ||
        !memcmp(old->digest,next->digest,32)){return 0;}
    memcpy(upgrade->record,bytes,size);
    memcpy(upgrade->previous_key,bytes+8+2*PD_APPROVAL_SIZE,32);
    memcpy(upgrade->candidate_key,bytes+8+2*PD_APPROVAL_SIZE+32,32);return 1;
}
