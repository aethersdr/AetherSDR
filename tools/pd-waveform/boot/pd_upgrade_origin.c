#include "pd_upgrade_origin.h"
#include "monocypher.h"
#include <string.h>
enum { kPrior=40, kHeader=kPrior+PD_UPGRADE_STATE_SIZE };
int pd_upgrade_origin_parse(const unsigned char* bytes,size_t size,const struct pd_upgrade* next,
                             struct pd_upgrade_origin* origin)
{
    memset(origin,0,sizeof(*origin));
    struct pd_upgrade approved;unsigned char digest[32],empty[PD_UPGRADE_STATE_SIZE]={0};
    if(size!=PD_UPGRADE_ORIGIN_SIZE || memcmp(bytes,"PDORIG01",8) ||
        !pd_upgrade_parse(next->record,sizeof(next->record),&approved)){return 0;}
    crypto_blake2b(digest,sizeof(digest),approved.record,sizeof(approved.record));
    if(crypto_verify32(digest,bytes+8)){return 0;}
    const unsigned char* key=approved.previous_key;struct pd_upgrade_state prior;
    if(memcmp(bytes+kPrior,empty,sizeof(empty))){
        if(!pd_upgrade_state_parse(bytes+kPrior,PD_UPGRADE_STATE_SIZE,&prior) ||
            !pd_upgrade_follows(&prior,&approved)){return 0;}
        key=prior.approval.candidate_key;
        origin->predecessor=prior;origin->has_predecessor=1;
    }
    uint32_t arch=approved.previous.profile==1?64:7;
    if(!pd_release_header_parse(bytes+kHeader,PD_RELEASE_PREFIX,key,arch,approved.previous.profile,
        approved.previous.firmware,&origin->management)){return 0;}
    memcpy(origin->record,bytes,size);return 1;
}
int pd_upgrade_origin_make(const struct pd_upgrade* next,const struct pd_upgrade_state* prior,
                            const struct pd_release* management,struct pd_upgrade_origin* origin)
{
    unsigned char bytes[PD_UPGRADE_ORIGIN_SIZE]={0};memcpy(bytes,"PDORIG01",8);
    crypto_blake2b(bytes+8,32,next->record,sizeof(next->record));
    if(prior){
        memcpy(bytes+kPrior,"PDUTXN01",8);
        memcpy(bytes+kPrior+8,prior->approval.record,PD_UPGRADE_APPROVAL_SIZE);
        if(prior->phase>255){return 0;}bytes[kHeader-1]=(unsigned char)prior->phase;
    }
    memcpy(bytes+kHeader,management->manifest,PD_RELEASE_MANIFEST);
    memcpy(bytes+kHeader+PD_RELEASE_MANIFEST,management->signature,64);
    return pd_upgrade_origin_parse(bytes,sizeof(bytes),next,origin);
}
