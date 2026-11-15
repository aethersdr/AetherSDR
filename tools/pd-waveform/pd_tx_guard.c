#include "pd_tx_guard.h"
#include <string.h>

static uint32_t be32(const unsigned char *p)
{
    return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];
}
int pd_tx_guard_init(struct pd_tx_guard *g,uint32_t sid,uint64_t generation,int64_t max_age_ns)
{
    if(!g || !sid || !generation || max_age_ns<=0) { return 0; }
    memset(g,0,sizeof(*g));g->sid=sid;g->generation=generation;g->max_age_ns=max_age_ns;
    return 1;
}
enum pd_tx_result pd_tx_guard_check(struct pd_tx_guard *g,uint64_t generation,
                                   const unsigned char *p,size_t bytes,
                                   int64_t arrival_ns,int64_t age_ns)
{
    if(!g || !p || bytes<8) { return PD_TX_INVALID; }
    if(generation!=g->generation || be32(p+4)!=g->sid) { return PD_TX_FOREIGN; }
    if(bytes<28 || bytes>28+360*8+4) { g->reset_pending=1;return PD_TX_INVALID; }
    uint32_t w=be32(p);unsigned trailer=(w&0x04000000) ? 1 : 0;
    unsigned words=w&65535;
    if(w>>28!=1 || !(w&0x08000000) || ((w>>22)&3)!=1 || ((w>>20)&3)!=2 ||
       words*4!=bytes || words<9+trailer || (words-7-trailer)%2 ||
       be32(p+8)!=0x1c2d || be32(p+12)!=0x534c03e3) {
        g->reset_pending=1;return PD_TX_INVALID;
    }
    unsigned samples=(words-7-trailer)/2;
    for(unsigned i=0;i<samples*2;++i) {
        if((be32(p+28+4*i)&0x7f800000)==0x7f800000) { g->reset_pending=1;return PD_TX_INVALID; }
    }
    if(age_ns<0 || age_ns>g->max_age_ns || arrival_ns<0) { g->reset_pending=1;return PD_TX_STALE; }
    uint32_t seconds=be32(p+16);
    uint64_t ps=(uint64_t)be32(p+20)<<32|be32(p+24);
    if(ps>=UINT64_C(1000000000000)) { g->reset_pending=1;return PD_TX_INVALID; }
    unsigned count=(w>>16)&15;
    int start=!g->seen || g->reset_pending;
    if(g->seen) {
        int same=seconds==g->seconds && ps==g->picoseconds;
        int older=seconds<g->seconds || (seconds==g->seconds && ps<g->picoseconds);
        if(count==g->count && same) { ++g->dropped;return PD_TX_DROP; }
        if(older && count!=(g->count+1)%16) { ++g->dropped;return PD_TX_DROP; }
        int gap=count!=(g->count+1)%16 || older;
        if(arrival_ns && g->last_arrival_ns) {
            int64_t limit=(int64_t)g->samples*1000000000/24000+g->max_age_ns;
            if(arrival_ns<g->last_arrival_ns || arrival_ns-g->last_arrival_ns>limit) { gap=1; }
        }
        if(gap && !start) { ++g->discontinuities;start=1; }
    }
    g->seen=1;g->reset_pending=0;g->seconds=seconds;g->picoseconds=ps;
    g->count=count;g->samples=samples;g->last_arrival_ns=arrival_ns;
    return start ? PD_TX_START : PD_TX_ACCEPT;
}
