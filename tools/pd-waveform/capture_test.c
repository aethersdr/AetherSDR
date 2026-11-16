#define main waveform_program_main
#include "pd_waveform.c"
#undef main
#undef NDEBUG
#include <assert.h>

static const struct pd_capture_meta kMeta = {7, 7150000, 0x40000001, 192000, 1, 1, 0, 0, 0, 0, 0, 0, 0};
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

static struct pd_capture_meta batch_meta(void)
{
    struct pd_capture_meta m=kMeta;
    m.rate=48000;m.measurement_id=31;m.measurement_tones=1;m.measurement_spacing_hz=1190;
    m.iq_channel=4;m.feedback_pan=0x40000000;m.gui_handle=7;m.binding_valid=1;
    return m;
}

static void batch_samples(unsigned char* out, unsigned first, unsigned count)
{
    for(unsigned i=0;i<count;++i){
        uint32_t value=0x3f000000+first+i;
        for(unsigned j=0;j<4;++j){out[i*8+j]=(unsigned char)(value>>(j*8));out[i*8+4+j]=0;}
    }
}

static void batch_fill(struct pd_capture_batch* b, const struct pd_capture_meta* m, unsigned count)
{
    unsigned char samples[4096];
    for(unsigned first=0;first<count;){
        unsigned n=count-first;if(n>375){n=375;}
        batch_samples(samples,first,n);
        pd_capture_batch_feed(b,m,samples,8*n,kNow+(int64_t)(first+n)*1000000000/48000);
        first+=n;
    }
}

static void check_batch_capture(void)
{
    struct pd_capture_batch* b=calloc(1,sizeof(*b));assert(b);
    struct pd_capture_meta m=batch_meta();unsigned char out[PD_CAPTURE_BATCH_REPLY];
    pd_capture_batch_begin(b,1,4,&m,kNow);
    assert(b->state==PD_CAPTURE_PENDING && b->requested==4);
    assert(pd_capture_batch_read(b,1,3*PD_CAPTURE_SAMPLES,kNow+1,out,sizeof(out))==PD_CAPTURE_BATCH_HEADER);
    assert(be32(out+16)==PD_CAPTURE_PENDING && be32(out+24)==0);
    assert(be32(out+96)==3 && be32(out+100)==4);
    // Collection completes without any reads, including non-packet-aligned boundaries.
    batch_fill(b,&m,4*PD_CAPTURE_SAMPLES);
    assert(b->state==PD_CAPTURE_READY && b->completed==4);
    for(unsigned offset=0;offset<4*PD_CAPTURE_SAMPLES;offset+=PD_CAPTURE_CHUNK){
        assert(pd_capture_batch_read(b,1,offset,kNow+3000000000LL,out,sizeof(out))==PD_CAPTURE_BATCH_REPLY);
        assert(!memcmp(out,"PDIQ0005",8) && pd_iq_get64(out+8)==1);
        assert(be32(out+16)==PD_CAPTURE_READY && be32(out+20)==offset && be32(out+24)==PD_CAPTURE_CHUNK);
        assert(be32(out+96)==offset/PD_CAPTURE_SAMPLES && be32(out+100)==4);
        assert(pd_iq_get64(out+72)==m.measurement_id && be32(out+68)==7);
        unsigned char expected[8*PD_CAPTURE_CHUNK];batch_samples(expected,offset,PD_CAPTURE_CHUNK);
        assert(!memcmp(out+PD_CAPTURE_BATCH_HEADER,expected,sizeof(expected)));
    }
    pd_capture_batch_invalidate(b);assert(b->state==PD_CAPTURE_READY);
    pd_capture_batch_begin(b,1,1,&m,kNow+4000000000LL);
    assert(b->requested==4 && b->started_ns==kNow);
    assert(pd_capture_batch_read(b,1,0,kNow+5000000001LL,out,sizeof(out))==PD_CAPTURE_BATCH_HEADER);
    assert(be32(out+16)==PD_CAPTURE_EXPIRED && be32(out+24)==0);
    assert(pd_capture_batch_read(b,2,0,kNow,out,sizeof(out))==PD_CAPTURE_BATCH_HEADER && be32(out+16)==PD_CAPTURE_UNAVAILABLE);
    assert(pd_capture_batch_read(b,1,1,kNow,out,sizeof(out))==PD_CAPTURE_BATCH_HEADER && be32(out+16)==PD_CAPTURE_UNAVAILABLE);
    assert(pd_capture_batch_read(b,1,4*PD_CAPTURE_SAMPLES,kNow,out,sizeof(out))==PD_CAPTURE_BATCH_HEADER && be32(out+16)==PD_CAPTURE_UNAVAILABLE);
    assert(!pd_capture_batch_read(b,1,0,kNow,out,PD_CAPTURE_BATCH_REPLY-1));

    uint64_t id=2;unsigned char sample[8]={0};
    // An identity or state change between frames invalidates the unfinished batch.
    for(unsigned field=0;field<13;++field){
        pd_capture_batch_begin(b,id++,4,&m,kNow);batch_fill(b,&m,PD_CAPTURE_SAMPLES);
        assert(b->completed==1);
        struct pd_capture_meta changed=m;
        if(field==0){++changed.generation;}if(field==1){++changed.center_hz;}
        if(field==2){++changed.stream;}if(field==3){changed.rate=96000;}
        if(field==4){changed.keyed=0;}if(field==5){changed.correcting=0;}
        if(field==6){++changed.measurement_id;}if(field==7){changed.measurement_tones=0;}
        if(field==8){++changed.measurement_spacing_hz;}if(field==9){changed.iq_channel=3;}
        if(field==10){++changed.feedback_pan;}if(field==11){++changed.gui_handle;}
        if(field==12){changed.binding_valid=0;}
        pd_capture_batch_feed(b,&changed,sample,sizeof(sample),b->last_ns+1);
        assert(b->state==PD_CAPTURE_INVALID);
        assert(pd_capture_batch_read(b,b->id,0,b->last_ns+1,out,sizeof(out))==PD_CAPTURE_BATCH_HEADER);
        assert(be32(out+16)==PD_CAPTURE_INVALID && be32(out+24)==0);
    }
    pd_capture_batch_begin(b,id++,4,&m,kNow);batch_fill(b,&m,PD_CAPTURE_SAMPLES);
    pd_capture_batch_feed(b,&m,sample,sizeof(sample),b->last_ns+100000001);
    assert(b->state==PD_CAPTURE_INVALID);
    pd_capture_batch_begin(b,id++,4,&m,kNow);pd_capture_batch_feed(b,&m,sample,sizeof(sample),kNow-1);
    assert(b->state==PD_CAPTURE_INVALID);
    pd_capture_batch_begin(b,id++,4,&m,kNow);pd_capture_batch_feed(b,&m,sample,sizeof(sample),kNow+1000000001);
    assert(b->state==PD_CAPTURE_EXPIRED);
    pd_capture_batch_begin(b,id++,4,&m,kNow);pd_capture_batch_invalidate(b);batch_fill(b,&m,4*PD_CAPTURE_SAMPLES);
    assert(b->state==PD_CAPTURE_INVALID && !b->completed);
    pd_capture_batch_begin(b,id++,1,&m,kNow);batch_fill(b,&m,PD_CAPTURE_SAMPLES-1);
    unsigned char bad[16]={0};bad[14]=0x80;bad[15]=0x7f;
    pd_capture_batch_feed(b,&m,bad,sizeof(bad),b->last_ns+1);
    assert(b->state==PD_CAPTURE_INVALID && !b->completed);
    pd_capture_batch_begin(b,id++,1,&m,kNow);batch_fill(b,&m,PD_CAPTURE_SAMPLES);
    assert(b->state==PD_CAPTURE_READY && b->completed==1);
    for(unsigned n=0;n<=5;n+=5){pd_capture_batch_begin(b,id++,n,&m,kNow);assert(b->state==PD_CAPTURE_UNAVAILABLE);}
    for(unsigned field=0;field<4;++field){
        struct pd_capture_meta invalid=m;
        if(field==0){invalid.keyed=0;}if(field==1){invalid.measurement_tones=0;}
        if(field==2){invalid.measurement_id=0;}if(field==3){invalid.binding_valid=0;}
        pd_capture_batch_begin(b,id++,4,&invalid,kNow);assert(b->state==PD_CAPTURE_UNAVAILABLE);
    }
    free(b);
}

int main(void)
{
    check_batch_capture();
    struct pd_capture c={0};unsigned char out[PD_CAPTURE_REPLY];
    assert(pd_capture_read(&c,1,0,kNow,out,sizeof(out))==PD_CAPTURE_HEADER);
    assert(be32(out+16)==PD_CAPTURE_UNAVAILABLE);
    pd_capture_begin(&c,1,&kMeta,kNow);fill(&c,&kMeta);
    assert(c.state==PD_CAPTURE_READY && c.count==PD_CAPTURE_SAMPLES);
    for (unsigned offset=0;offset<PD_CAPTURE_SAMPLES;offset+=PD_CAPTURE_CHUNK) {
        assert(pd_capture_read(&c,1,offset,kNow+40000000,out,sizeof(out))==PD_CAPTURE_REPLY);
        assert(!memcmp(out,"PDIQ0004",8) && pd_iq_get64(out+8)==1);
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
    measuring.measurement_id=31;measuring.measurement_tones=1;measuring.measurement_spacing_hz=1190;
    pd_capture_begin(&c,30,&measuring,kNow);fill(&c,&measuring);
    assert(pd_capture_read(&c,30,0,kNow+40000000,out,sizeof(out))==PD_CAPTURE_REPLY);
    assert(pd_iq_get64(out+72)==31 && be32(out+68)==7 && be32(out+80)==1190);
    pd_capture_begin(&c,31,&measuring,kNow);
    struct pd_capture_meta changed=measuring;changed.measurement_id=32;
    pd_capture_feed(&c,&changed,sample,sizeof(sample),kNow+1);assert(c.state==PD_CAPTURE_INVALID);
    pd_capture_begin(&c,32,&measuring,kNow);changed=measuring;changed.measurement_tones=0;
    pd_capture_feed(&c,&changed,sample,sizeof(sample),kNow+1);assert(c.state==PD_CAPTURE_INVALID);
    pd_capture_begin(&c,33,&measuring,kNow);changed=measuring;changed.measurement_spacing_hz=1001;
    pd_capture_feed(&c,&changed,sample,sizeof(sample),kNow+1);assert(c.state==PD_CAPTURE_INVALID);
    measuring.measurement_spacing_hz=199;
    pd_capture_begin(&c,34,&measuring,kNow);assert(c.state==PD_CAPTURE_UNAVAILABLE);
    measuring.measurement_spacing_hz=10001;
    pd_capture_begin(&c,35,&measuring,kNow);assert(c.state==PD_CAPTURE_UNAVAILABLE);
    measuring.measurement_spacing_hz=10000;
    pd_capture_begin(&c,36,&measuring,kNow);fill(&c,&measuring);
    assert(pd_capture_read(&c,36,0,kNow+40000000,out,sizeof(out))==PD_CAPTURE_REPLY && be32(out+80)==10000);
    measuring.measurement_id=0;
    pd_capture_begin(&c,37,&measuring,kNow);assert(c.state==PD_CAPTURE_UNAVAILABLE);

    struct pd_capture_meta bound=kMeta;
    bound.iq_channel=3;bound.feedback_pan=0x40000002;bound.gui_handle=7;bound.binding_valid=1;
    pd_capture_begin(&c,40,&bound,kNow);fill(&c,&bound);
    assert(pd_capture_read(&c,40,0,kNow+40000000,out,sizeof(out))==PD_CAPTURE_REPLY);
    assert(be32(out+84)==3 && be32(out+88)==0x40000002 && be32(out+92)==7);
    for(unsigned field=0;field<4;++field){
        struct pd_capture_meta changed=bound;
        if(field==0){changed.iq_channel=2;}
        if(field==1){++changed.feedback_pan;}
        if(field==2){++changed.gui_handle;}
        if(field==3){changed.binding_valid=0;}
        pd_capture_begin(&c,41+field,&bound,kNow);
        pd_capture_feed(&c,&changed,sample,sizeof(sample),kNow+1);
        assert(c.state==PD_CAPTURE_INVALID);
    }
    bound.binding_valid=0;pd_capture_begin(&c,45,&bound,kNow);assert(c.state==PD_CAPTURE_UNAVAILABLE);
    bound.binding_valid=1;bound.iq_channel=5;pd_capture_begin(&c,46,&bound,kNow);assert(c.state==PD_CAPTURE_UNAVAILABLE);
    w.sealed_feedback_pan=0x40000002;w.calibration_gui=7;w.daxiq_ch=3;
    meta=capture_meta(&w);assert(meta.feedback_pan==w.sealed_feedback_pan && !meta.binding_valid);
    w.feedback_pan=w.sealed_feedback_pan;w.feedback_pan_owner=7;
    meta=capture_meta(&w);assert(meta.binding_valid && meta.iq_channel==3 && meta.gui_handle==7);
    w.feedback_binding_failed=1;meta=capture_meta(&w);assert(!meta.binding_valid);

    w.calibration.purpose=PD_SOURCE_MEASUREMENT;w.calibration.source_id=31;
    w.calibration.tone_spacing_hz=PD_TONE_DEFAULT_SPACING_HZ;
    w.calibration.phase=PD_CAL_RUNNING;w.calibration.deadline_ns=kNow+8000000000LL;
    w.keyed=w.aligned=w.feedback_ok=1;w.test_f1=710;w.test_f2=1900;w.test_left=90000;
    w.tx_guard.seen=1;
    w.measurement_tone_sent_ns=kNow;w.measurement_guard_ns=500000000;
    w.local_iq.last_valid_ns=kNow+510000000;w.local_iq.last_age_ns=20000000;
    assert(!measurement_tones_ready(&w,kNow+510000000)); // queued preamble side
    w.local_iq.last_valid_ns=kNow+520000000;
    assert(measurement_tones_ready(&w,kNow+520000000));
    w.calibration.tone_spacing_hz=10000;
    assert(!measurement_tones_ready(&w,kNow+520000000));
    w.test_f1=-3695;w.test_f2=6305;
    assert(measurement_tones_ready(&w,kNow+520000000));
    w.calibration.tone_spacing_hz=2610;w.test_f1=0;w.test_f2=2610;
    assert(measurement_tones_ready(&w,kNow+520000000));
    w.test_noise_left=1;assert(!measurement_tones_ready(&w,kNow+520000000));w.test_noise_left=0;
    w.test_left=RATE/200;assert(!measurement_tones_ready(&w,kNow+520000000));w.test_left=90000;
    w.local_iq.last_age_ns=25000001;assert(!measurement_tones_ready(&w,kNow+520000000));w.local_iq.last_age_ns=0;
    assert(!measurement_tones_ready(&w,kNow+700000000)); // stale IQ
    assert(!measurement_tones_ready(&w,kNow-1)); // clock reversal
    assert(!measurement_tones_ready(&w,w.calibration.deadline_ns));
    disarm_bounded_test(&w, "test_cancel");assert(!measurement_tones_ready(&w,kNow+520000000));
    // Exercise the real private query dispatcher and raw-IQ packet tap together.
    int64_t now=pd_iq_monotonic_ns();
    w.active_slice=-1;w.feedback_binding_failed=0;w.correct=0;
    w.calibration.phase=PD_CAL_RUNNING;w.calibration.deadline_ns=now+8000000000LL;
    w.calibration.tone_spacing_hz=1190;w.test_f1=710;w.test_f2=1900;
    w.test_left=RATE*4;w.measurement_tone_sent_ns=now-600000000;
    w.local_iq.last_valid_ns=now;w.local_iq.last_age_ns=0;
    unsigned char batch_reply[PD_CAPTURE_BATCH_REPLY];
    struct pd_status_query query={.operation='G',.capture_id=77,.capture_offset=4};
    assert(capture_query(&w,&query,now,batch_reply,sizeof(batch_reply))==PD_CAPTURE_BATCH_HEADER);
    assert(be32(batch_reply+16)==PD_CAPTURE_PENDING && be32(batch_reply+20)==0);
    for(unsigned i=0;i<512;++i){
        w.local_iq.last_valid_ns=pd_iq_monotonic_ns();
        on_packet(&w,packet,sizeof(packet));
    }
    assert(w.capture_batch.completed==4 && w.capture_batch.state==PD_CAPTURE_READY);
    query.operation='H';query.capture_offset=3*PD_CAPTURE_SAMPLES;
    assert(capture_query(&w,&query,pd_iq_monotonic_ns(),batch_reply,sizeof(batch_reply))==PD_CAPTURE_BATCH_REPLY);
    assert(be32(batch_reply+96)==3 && !memcmp(batch_reply+PD_CAPTURE_BATCH_HEADER,packet+28,sizeof(packet)-28));
    assert(!w.n_tx_pkts && !w.ref.n && !w.fbk.n);
    query.operation='G';query.capture_id=78;query.capture_offset=4;
    assert(capture_query(&w,&query,pd_iq_monotonic_ns(),batch_reply,sizeof(batch_reply))==PD_CAPTURE_BATCH_HEADER);
    assert(w.capture_batch.state==PD_CAPTURE_PENDING);
    // A stopped source cannot be silently resumed by incoming IQ or a read.
    disarm_bounded_test(&w,"batch_test_cancel");
    assert(w.capture_batch.state==PD_CAPTURE_INVALID);
    on_packet(&w,packet,sizeof(packet));
    assert(w.capture_batch.state==PD_CAPTURE_INVALID);
    query.operation='H';query.capture_offset=0;
    assert(capture_query(&w,&query,pd_iq_monotonic_ns(),batch_reply,sizeof(batch_reply))==PD_CAPTURE_BATCH_HEADER);
    assert(be32(batch_reply+16)==PD_CAPTURE_INVALID);
    puts("PASS: raw IQ snapshot, ownership, continuity, expiry and waveform tap");
    return 0;
}
