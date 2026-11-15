#define main waveform_program_main
#include "pd_waveform.c"
#undef main
#undef NDEBUG
#include <assert.h>

static const struct pd_capture_meta kMeta = {7, 7150000, 0x40000001, 192000, 1, 1, 0, 0};
static const int64_t kNow = 10000000000LL;

static void fill(struct pd_capture* c, const struct pd_capture_meta* meta)
{
    unsigned char samples[4096];
    for (unsigned i=0;i<sizeof(samples);i+=8) {
        const unsigned char sample[8]={0,0,0,0x3f,0,0,0x80,0xbe}; // .5 - .25i
        memcpy(samples+i,sample,8);
    }
    for (unsigned i=0;i<32;++i) {
        pd_capture_feed(c,meta,samples,sizeof(samples),kNow+1000000*(i+1));
    }
}

int main(void)
{
    struct pd_capture c={0};unsigned char out[PD_CAPTURE_REPLY];
    assert(pd_capture_read(&c,1,0,kNow,out,sizeof(out))==PD_CAPTURE_HEADER);
    assert(be32(out+16)==PD_CAPTURE_UNAVAILABLE);
    pd_capture_begin(&c,1,&kMeta,kNow);fill(&c,&kMeta);
    assert(c.state==PD_CAPTURE_READY && c.count==PD_CAPTURE_SAMPLES);
    for (unsigned offset=0;offset<PD_CAPTURE_SAMPLES;offset+=PD_CAPTURE_CHUNK) {
        assert(pd_capture_read(&c,1,offset,kNow+40000000,out,sizeof(out))==PD_CAPTURE_REPLY);
        assert(!memcmp(out,"PDIQ0002",8) && pd_iq_get64(out+8)==1);
        assert(be32(out+16)==PD_CAPTURE_READY && be32(out+20)==offset && be32(out+24)==1024);
        assert(be32(out+28)==192000 && pd_iq_get64(out+32)==7150000);
        assert(pd_iq_get64(out+40)==7 && be32(out+64)==kMeta.stream && be32(out+68)==3);
        assert(pd_iq_get64(out+48)==(uint64_t)(kNow+1000000));
        assert(pd_iq_get64(out+56)==(uint64_t)(kNow+32000000));
        assert(!memcmp(out+PD_CAPTURE_HEADER,c.samples+offset*8,PD_CAPTURE_CHUNK*8));
    }
    // Completed captures survive later unkey/reset, and repeated begin is idempotent.
    pd_capture_invalidate(&c);pd_capture_begin(&c,1,&kMeta,kNow+40000000);
    assert(c.state==PD_CAPTURE_READY && c.started_ns==kNow);
    assert(pd_capture_read(&c,2,0,kNow+40000000,out,sizeof(out))==PD_CAPTURE_HEADER);
    assert(be32(out+16)==PD_CAPTURE_UNAVAILABLE);
    assert(pd_capture_read(&c,1,1,kNow+40000000,out,sizeof(out))==PD_CAPTURE_HEADER);
    assert(be32(out+16)==PD_CAPTURE_UNAVAILABLE);
    assert(pd_capture_read(&c,1,0,kNow+5000000001LL,out,sizeof(out))==PD_CAPTURE_HEADER);
    assert(be32(out+16)==PD_CAPTURE_EXPIRED);

    unsigned char sample[8]={0};
    // Every metadata change invalidates collection; no mixed frame is published.
    for (unsigned field=0;field<6;++field) {
        struct pd_capture_meta changed=kMeta;
        if (field==0) { ++changed.generation; }
        if (field==1) { ++changed.center_hz; }
        if (field==2) { ++changed.stream; }
        if (field==3) { changed.rate=96000; }
        if (field==4) { changed.keyed=0; }
        if (field==5) { changed.correcting=0; }
        pd_capture_begin(&c,2+field,&kMeta,kNow);
        pd_capture_feed(&c,&changed,sample,sizeof(sample),kNow+1);
        assert(c.state==PD_CAPTURE_INVALID && c.count==0);
    }
    pd_capture_begin(&c,8,&kMeta,kNow);
    pd_capture_feed(&c,&kMeta,sample,sizeof(sample),kNow+1);
    pd_capture_feed(&c,&kMeta,sample,sizeof(sample),kNow+100000002);
    assert(c.state==PD_CAPTURE_INVALID);
    pd_capture_begin(&c,9,&kMeta,kNow);pd_capture_invalidate(&c);
    fill(&c,&kMeta);assert(c.state==PD_CAPTURE_INVALID);
    pd_capture_begin(&c,10,&kMeta,kNow);
    pd_capture_feed(&c,&kMeta,sample,sizeof(sample),kNow+1000000001);
    assert(c.state==PD_CAPTURE_EXPIRED);
    pd_capture_begin(&c,11,&kMeta,kNow);
    pd_capture_feed(&c,&kMeta,sample,sizeof(sample),kNow-1);
    assert(c.state==PD_CAPTURE_EXPIRED);
    pd_capture_begin(&c,12,&kMeta,kNow);
    pd_capture_feed(&c,&kMeta,sample,7,kNow+1);assert(c.state==PD_CAPTURE_INVALID);
    // A nonfinite last sample must not publish an otherwise complete frame.
    pd_capture_begin(&c,13,&kMeta,kNow);
    c.count=PD_CAPTURE_SAMPLES-1;
    c.first_ns=c.last_ns=kNow;
    unsigned char bad[16]={0};bad[14]=0x80;bad[15]=0x7f;
    pd_capture_feed(&c,&kMeta,bad,sizeof(bad),kNow+1);
    assert(c.state==PD_CAPTURE_INVALID && c.count==PD_CAPTURE_SAMPLES-1);
    pd_capture_begin(&c,12,&kMeta,kNow);assert(c.id==13);
    struct pd_capture_meta invalid=kMeta;invalid.rate=1;
    pd_capture_begin(&c,14,&invalid,kNow);assert(c.state==PD_CAPTURE_UNAVAILABLE);

    // The real packet handler taps raw bytes even while unkeyed. A foreign
    // stream cannot fill it, and no DAX audio or post-EQ data enters it.
    wf_t w={0};w.local_iq_mode=1;w.daxiq_sid=kMeta.stream;
    w.daxiq_rate=192000;w.pan_mhz=7.15;w.acquisition_generation=2;
    struct pd_capture_meta meta=capture_meta(&w);
    pd_capture_begin(&w.capture,1,&meta,pd_iq_monotonic_ns());
    unsigned char packet[28+8*128]={0};
    put32(packet,0x18500000u+(unsigned)sizeof(packet)/4);
    put32(packet+4,w.daxiq_sid+1);put32(packet+8,FLEX_OUI);
    put32(packet+12,(INFO_CLASS<<16)|CLASS_FLOAT_BIT);
    for (unsigned i=28;i<sizeof(packet);i+=8) { packet[i+3]=0x3f;packet[i+7]=0xbe; }
    on_packet(&w,packet,sizeof(packet));assert(w.capture.count==0);
    put32(packet+4,w.daxiq_sid);
    for (unsigned i=0;i<128;++i) { on_packet(&w,packet,sizeof(packet)); }
    assert(w.capture.state==PD_CAPTURE_READY && w.capture.meta.keyed==0);
    assert(!memcmp(w.capture.samples,packet+28,sizeof(packet)-28));
    assert(!w.ref.n && !w.fbk.n && !w.n_tx_pkts && !w.tx_on);
    pd_capture_begin(&w.capture,2,&meta,pd_iq_monotonic_ns());
    w.pan_mhz=7.16;on_packet(&w,packet,sizeof(packet));
    assert(w.capture.state==PD_CAPTURE_INVALID);
    // Source identity and steady-tone boundaries are immutable for a frame.
    struct pd_capture_meta measuring=kMeta;
    measuring.measurement_id=31;measuring.measurement_tones=1;
    pd_capture_begin(&c,30,&measuring,kNow);fill(&c,&measuring);
    assert(pd_capture_read(&c,30,0,kNow+40000000,out,sizeof(out))==PD_CAPTURE_REPLY);
    assert(pd_iq_get64(out+72)==31 && be32(out+68)==7);
    pd_capture_begin(&c,31,&measuring,kNow);
    struct pd_capture_meta changed=measuring;changed.measurement_id=32;
    pd_capture_feed(&c,&changed,sample,sizeof(sample),kNow+1);assert(c.state==PD_CAPTURE_INVALID);
    pd_capture_begin(&c,32,&measuring,kNow);changed=measuring;changed.measurement_tones=0;
    pd_capture_feed(&c,&changed,sample,sizeof(sample),kNow+1);assert(c.state==PD_CAPTURE_INVALID);
    measuring.measurement_id=0;
    pd_capture_begin(&c,33,&measuring,kNow);assert(c.state==PD_CAPTURE_UNAVAILABLE);

    w.calibration.purpose=PD_SOURCE_MEASUREMENT;w.calibration.source_id=31;
    w.calibration.phase=PD_CAL_RUNNING;w.calibration.deadline_ns=kNow+8000000000LL;
    w.keyed=w.aligned=w.feedback_ok=1;w.test_f1=710;w.test_f2=1900;w.test_left=90000;
    w.measurement_tone_sent_ns=kNow;w.measurement_guard_ns=500000000;
    w.local_iq.last_valid_ns=kNow+510000000;w.local_iq.last_age_ns=20000000;
    assert(!measurement_tones_ready(&w,kNow+510000000)); // queued preamble side
    w.local_iq.last_valid_ns=kNow+520000000;
    assert(measurement_tones_ready(&w,kNow+520000000));
    w.test_noise_left=1;assert(!measurement_tones_ready(&w,kNow+520000000));w.test_noise_left=0;
    w.test_left=RATE/200;assert(!measurement_tones_ready(&w,kNow+520000000));w.test_left=90000;
    w.local_iq.last_age_ns=25000001;assert(!measurement_tones_ready(&w,kNow+520000000));w.local_iq.last_age_ns=0;
    assert(!measurement_tones_ready(&w,kNow+700000000)); // stale IQ
    assert(!measurement_tones_ready(&w,kNow-1)); // clock reversal
    assert(!measurement_tones_ready(&w,w.calibration.deadline_ns));
    disarm_bounded_test(&w, "test_cancel");assert(!measurement_tones_ready(&w,kNow+520000000));
    puts("PASS: raw IQ snapshot, ownership, continuity, expiry and waveform tap");
    return 0;
}
