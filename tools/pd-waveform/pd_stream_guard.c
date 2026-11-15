#include "pd_stream_guard.h"
#include <string.h>

static uint32_t be32(const unsigned char *p)
{
    return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 | (uint32_t)p[2]<<8 | p[3];
}

static uint64_t be64(const unsigned char *p)
{
    return (uint64_t)be32(p)<<32 | be32(p+4);
}

int pd_guard_init(struct pd_stream_guard *g, uint32_t sid, uint64_t generation,
                  unsigned rate, int64_t max_age_ns)
{
    if (!g || !sid || !generation ||
        (rate != 24000 && rate != 48000 && rate != 96000 && rate != 192000) || max_age_ns <= 0) {
        return 0;
    }
    memset(g, 0, sizeof(*g));
    g->sid=sid; g->generation=generation; g->rate=rate;
    g->max_age_ns=max_age_ns; g->reset_pending=1;
    return 1;
}

static enum pd_guard_result invalidate(struct pd_stream_guard *g, enum pd_guard_result result)
{
    g->reset_pending=1;
    return result;
}

enum pd_guard_result pd_guard_check(struct pd_stream_guard *g, uint64_t generation,
                                    const unsigned char *p, size_t bytes,
                                    int64_t age_ns, struct pd_guard_frame *frame)
{
    if (!g || !p || !frame) { return PD_GUARD_INVALID; }
    memset(frame, 0, sizeof(*frame));
    if (generation != g->generation) { return PD_GUARD_FOREIGN; }
    if (bytes < 28 || bytes > 4096) { return invalidate(g, PD_GUARD_INVALID); }
    uint32_t w=be32(p);
    if (be32(p+4) != g->sid) { return PD_GUARD_FOREIGN; }
    unsigned words=w & 65535;
    unsigned trailer=(w & 0x04000000) ? 1 : 0;
    unsigned expectedClass=g->rate==24000 ? 0x02e3 : g->rate==48000 ? 0x02e4 :
                           g->rate==96000 ? 0x02e5 : 0x02e6;
    /* Qualified format: IF data, class ID, UTC seconds and realtime fraction. */
    if (w>>28 != 1 || !(w & 0x08000000) || ((w>>22)&3) != 1 ||
        ((w>>20)&3) != 2 || words*4 != bytes || words < 9+trailer ||
        (words-7-trailer)%2 || be32(p+8) != 0x1c2d ||
        be32(p+12) != (0x534c0000 | expectedClass)) {
        return invalidate(g, PD_GUARD_INVALID);
    }
    uint32_t seconds=be32(p+16);
    uint64_t ps=be64(p+20);
    if (ps >= UINT64_C(1000000000000)) { return invalidate(g, PD_GUARD_INVALID); }
    unsigned samples=(words-7-trailer)/2;
    for (unsigned i=0; i<samples*2; ++i) {
        const unsigned char *v=p+28+4*i;
        uint32_t bits=(uint32_t)v[3]<<24 | (uint32_t)v[2]<<16 | (uint32_t)v[1]<<8 | v[0];
        if ((bits & 0x7f800000) == 0x7f800000) { return invalidate(g, PD_GUARD_INVALID); }
    }
    if (age_ns < 0 || age_ns > g->max_age_ns) { return invalidate(g, PD_GUARD_STALE); }
    unsigned count=(w>>16)&15;
    int start=g->reset_pending || !g->seen;
    if (g->seen) {
        int64_t ds=(int64_t)seconds-g->last_seconds;
        if (ds==0 && ps==g->last_picoseconds) { return PD_GUARD_DUPLICATE; }
        if (ds<0 || (ds==0 && ps<g->last_picoseconds)) { return PD_GUARD_OLD; }
        if (!start) {
            double actual=ds>86400 ? 1e30 : (double)ds*1e12+(double)ps-(double)g->last_picoseconds;
            double expected=(double)g->last_samples*1e12/g->rate;
            double difference=actual-expected;
            double halfSample=5e11/g->rate;
            if (count != (g->last_count+1)%16 || difference>halfSample || difference<-halfSample) {
                start=1;
                ++g->discontinuities;
            }
        }
    }
    g->seen=1; g->reset_pending=0; g->last_seconds=seconds;
    g->last_picoseconds=ps; g->last_samples=samples; g->last_count=count;
    ++g->accepted;
    frame->payload=p+28; frame->complex_samples=samples;
    frame->seconds=seconds; frame->picoseconds=ps;
    return start ? PD_GUARD_START : PD_GUARD_ACCEPT;
}
