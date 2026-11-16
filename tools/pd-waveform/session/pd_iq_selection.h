#ifndef AETHER_PD_IQ_SELECTION_H
#define AETHER_PD_IQ_SELECTION_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct pd_iq_selection { uint32_t channel,pan; };
static inline int pd_iq_selection_valid(struct pd_iq_selection selection)
{
    return selection.channel>=1 && selection.channel<=4 && (selection.pan || selection.channel==1);
}
/* Absent selection preserves the legacy channel-1 discovery. Partial or
 * malformed selection never falls back to that default. */
static inline int pd_iq_selection_parse(const char* channel,const char* pan,struct pd_iq_selection* out)
{
    *out=(struct pd_iq_selection){1,0};
    if(!channel && !pan){return 1;}
    if(!channel || !pan || strlen(channel)!=1 || channel[0]<'1' || channel[0]>'4' ||
       strlen(pan)!=10 || pan[0]!='0' || pan[1]!='x'){return 0;}
    uint32_t value=0;
    for(unsigned i=2;i<10;++i){
        unsigned c=(unsigned char)pan[i];
        if(c>='0' && c<='9'){c-='0';}
        else if(c>='a' && c<='f'){c=c-'a'+10;}
        else if(c>='A' && c<='F'){c=c-'A'+10;}
        else{return 0;}
        value=(value<<4)|c;
    }
    *out=(struct pd_iq_selection){(uint32_t)(channel[0]-'0'),value};
    return pd_iq_selection_valid(*out);
}
static inline int pd_iq_selection_environment(struct pd_iq_selection* out)
{
    return pd_iq_selection_parse(getenv("SHIM_PD_IQ_CHANNEL"),getenv("SHIM_PD_FEEDBACK_PAN"),out);
}
#endif
