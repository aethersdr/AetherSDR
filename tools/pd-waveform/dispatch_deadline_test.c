// Exercise the production packet/parser paths with injected datagram I/O.
// No socket, listener, radio peer or RF control is created by this test.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/types.h>
static unsigned char queued_frame[8192];
static size_t queued_bytes, sent_bytes;
static int fail_send;
static void queue_frame(const void* bytes,size_t size)
{
    assert(!queued_bytes && size<=sizeof(queued_frame));
    memcpy(queued_frame,bytes,size);queued_bytes=size;
}
static ssize_t fixture_receive(int fd,struct msghdr* message,int flags)
{
    assert(fd==123 && flags==MSG_DONTWAIT && message->msg_iovlen==1);
    if (!queued_bytes) { errno=EAGAIN;return -1; }
    size_t size=queued_bytes<message->msg_iov[0].iov_len?queued_bytes:message->msg_iov[0].iov_len;
    message->msg_flags=queued_bytes>size?MSG_TRUNC:0;
    memcpy(message->msg_iov[0].iov_base,queued_frame,size);queued_bytes=0;
    return (ssize_t)size;
}
static ssize_t fixture_send(int fd,const void* bytes,size_t size,int flags,
                            const struct sockaddr* peer,socklen_t peer_size)
{
    assert(fd==42 && bytes && size>=28 && flags==0 && peer && peer_size==sizeof(struct sockaddr_in));
    if (fail_send) { errno=EBADF;return -1; }
    sent_bytes=size;return (ssize_t)size;
}
#define recvmsg fixture_receive
#include "pd_local_iq.c"
#undef recvmsg
#define sendto fixture_send
#define main waveform_program_main
#include "pd_waveform.c"
#undef main
#undef sendto
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
extern void warm_seed_curve(void);
struct fixture { wf_t w;int sender;unsigned iq_count,mic_count; };
static void line(struct fixture* f,const char* value)
{
    char copy[256];snprintf(copy,sizeof(copy),"%s",value);api_line(&f->w,copy);
}
static void mic(struct fixture* f)
{
    float sample[256];for(unsigned i=0;i<256;++i){sample[i]=(float)(0.2*cos(i*0.13));}
    unsigned char packet[28+sizeof(sample)];
    int bytes=vita_build(packet,f->w.tx_in,CLASS_AUDIO_24K,(int)f->mic_count++,sample,128);
    on_packet(&f->w,packet,bytes);
}
static double tap_peak;
static void capture_timed_test(void* context,const float* iq,int n)
{
    (void)context;tap_peak=0;
    for(int i=0;i<2*n;++i){if(fabs(iq[i])>tap_peak){tap_peak=fabs(iq[i]);}}
}
static void zero_mic(struct fixture* f)
{
    float sample[256]={0};unsigned char packet[28+sizeof(sample)];
    int bytes=vita_build(packet,f->w.tx_in,CLASS_AUDIO_24K,(int)f->mic_count++,sample,128);
    on_packet(&f->w,packet,bytes);
}
static void iq_age(struct fixture* f, uint64_t age)
{
    unsigned char packet[36]={0},envelope[PD_IQ_HEADER+36];
    unsigned count=f->iq_count++;
    pd_iq_put32(packet,0x18600009u|((count%16)<<16));pd_iq_put32(packet+4,f->w.daxiq_sid);
    pd_iq_put32(packet+8,0x1c2d);pd_iq_put32(packet+12,0x534c02e4);pd_iq_put32(packet+16,1794440000);
    pd_iq_put64(packet+20,(uint64_t)count*UINT64_C(1000000000000)/48000);
    memcpy(envelope,"PDIQ",4);pd_iq_put32(envelope+4,1);pd_iq_put64(envelope+8,1);
    struct timespec ts;clock_gettime(CLOCK_REALTIME,&ts);uint64_t rt=(uint64_t)ts.tv_sec*1000000000+ts.tv_nsec;
    uint64_t mt=(uint64_t)pd_iq_monotonic_ns();
    pd_iq_put64(envelope+16,rt-age);pd_iq_put64(envelope+24,rt);pd_iq_put64(envelope+32,mt);pd_iq_put64(envelope+40,mt);
    pd_iq_put32(envelope+48,36);pd_iq_put32(envelope+52,0);memcpy(envelope+56,packet,36);
    queue_frame(envelope,sizeof(envelope));assert(local_feedback_poll(&f->w));
}
static void iq(struct fixture* f) { iq_age(f,0); }
static void init(struct fixture* f)
{
    memset(f,0,sizeof(*f));assert(wf_init(&f->w));f->w.fb_audio=0;f->w.local_iq_mode=1;
    f->w.api=open("/dev/null",O_WRONLY);assert(f->w.api>=0);
    assert(!queued_bytes);f->sender=-1;f->w.local_iq.fd=123;
    f->w.seq_create=3;line(f,"R3|0|tx_stream_in_id=0x84000001 rx_stream_in_id=0x84000000");
    f->w.daxiq_sid=0x20000001;assert(pd_guard_init(&f->w.local_iq.guard,f->w.daxiq_sid,1,48000,25000000));
    line(f,"S1|slice 1 mode=PD RF_frequency=7.150000");
    line(f,"S1|display pan 0x40000001 center=7.151000 daxiq_channel=1 rxant=RX_A");
    line(f,"S1|transmit rfpower=5 lo=100 hi=2800");
    iq(f);line(f,"S1|interlock state=TRANSMITTING");mic(f);
    double tx[1024],rx[1024];uint32_t seed=7;
    for(int i=0;i<1024;++i){seed=seed*1664525u+1013904223u;rx[i]=tx[i]=(seed>>8)/16777216.0-0.5;}
    pd_eq_train(f->w.eq,tx,rx,512);assert(pd_eq_solve(f->w.eq));f->w.aligned=1;
    warm_seed_curve();mic(f);assert(correction_ready(&f->w) && !f->w.local_iq_fault);
}
static void pause_clean(struct fixture* f)
{
    line(f,"S1|interlock state=UNKEY_REQUESTED");line(f,"S1|interlock state=READY");
    assert(pd_core_cached_solution(f->w.core) && !f->w.tx_on && !f->w.keyed);
}
static void destroy(struct fixture* f)
{
    close(f->w.api);assert(!queued_bytes);
    pd_acquire_destroy(f->w.acquire);pd_core_destroy(f->w.core);pd_align_destroy(f->w.align);pd_eq_destroy(f->w.eq);pd_eq_destroy(f->w.trusted_eq);
    free(f->w.ref.buf);free(f->w.fbk.buf);free(f->w.acq_tx);free(f->w.acq_rx);
}
int main(void)
{
    alarm(20);struct fixture f;
    init(&f);pause_clean(&f);
    line(&f,"S1|display pan 0x40000002 center=50.0");assert(pd_core_cached_solution(f.w.core));
    line(&f,"S1|display pan 0x40000001 rxant=RX_A");assert(pd_core_cached_solution(f.w.core));
    line(&f,"S1|interlock state=PTT_REQUESTED");line(&f,"S1|interlock state=TRANSMITTING");
    assert(!correction_ready(&f.w));uint64_t packets=f.w.cached_packets;mic(&f);assert(f.w.cached_packets==packets);
    --f.iq_count;iq(&f);mic(&f);assert(f.w.cached_packets==packets && !correction_ready(&f.w));
    iq(&f);mic(&f);assert(f.w.cached_packets==packets+1 && f.w.warm_starts==1);
    ++f.mic_count;mic(&f);assert(!pd_core_cached_solution(f.w.core));destroy(&f);
    init(&f);line(&f,"S1|interlock state=READY");assert(!pd_core_cached_solution(f.w.core));destroy(&f);
    init(&f);pause_clean(&f);f.w.local_iq.last_valid_ns=pd_iq_monotonic_ns()-100000001;
    assert(local_feedback_poll(&f.w));assert(!pd_core_cached_solution(f.w.core));destroy(&f);
    init(&f);pause_clean(&f);line(&f,"S1|interlock state=TRANSMITTING");iq(&f);
    f.w.tx_age_ns=150000000;mic(&f);assert(!pd_core_cached_solution(f.w.core));destroy(&f);
    init(&f);pause_clean(&f);queue_frame("bad",3);assert(local_feedback_poll(&f.w));
    assert(!pd_core_cached_solution(f.w.core));destroy(&f);
    init(&f);pause_clean(&f);iq_age(&f,150000000);
    assert(f.w.local_iq.stale==1 && !pd_core_cached_solution(f.w.core));destroy(&f);
    init(&f);pause_clean(&f);++f.iq_count;iq(&f);
    assert(!pd_core_cached_solution(f.w.core));destroy(&f);
    const char* changes[]={"S1|interlock state=TX_FAULT","S1|transmit rfpower=8",
        "S1|transmit lo=200","S1|transmit hi=2900",
        "S1|apd enable=1","S1|apd equalizer_active=1",
        "S1|slice 1 RF_frequency=7.151000","S1|slice 1 txant=ANT2",
        "S1|display pan 0x40000001 rfgain=10","S1|display pan 0x40000001 daxiq_channel=0",
        "S1|radio full_duplex_enabled=0","S1|slice 1 mode=USB"};
    for(unsigned i=0;i<sizeof(changes)/sizeof(changes[0]);++i) {
        init(&f);pause_clean(&f);line(&f,changes[i]);assert(!pd_core_cached_solution(f.w.core));destroy(&f);
    }
    init(&f);pause_clean(&f);
    line(&f,"S1|transmit rfpower=5 lo=100 hi=2800 tx_filter_changes_allowed=0");
    line(&f,"S1|apd sampler enable=1 equalizer_active=1");
    line(&f,"S1|apd enable=0 equalizer_active=0 configurable=1 state=OFF");
    assert(!native_apd_blocked(&f.w) && pd_core_cached_solution(f.w.core));destroy(&f);
    init(&f);line(&f,"S1|apd enable=1 equalizer_active=0");
    assert(native_apd_blocked(&f.w) && !correction_ready(&f.w) && !pd_core_cached_solution(f.w.core));
    for(int i=0;i<BLOCK;++i){fifo_push(&f.w.ref,.2,.1);fifo_push(&f.w.fbk,.2,.1);}
    f.w.settle_left=0;int ref_count=f.w.ref.n,fb_count=f.w.fbk.n;
    calibrate(&f.w);assert(f.w.ref.n==ref_count && f.w.fbk.n==fb_count && f.w.acq_n==0);
    line(&f,"S1|interlock state=UNKEY_REQUESTED");line(&f,"S1|interlock state=READY");
    line(&f,"S1|interlock state=TRANSMITTING");iq(&f);mic(&f);
    assert(native_apd_blocked(&f.w) && !correction_ready(&f.w) && f.w.cached_packets==0);
    line(&f,"S1|apd enable=0");
    assert(!native_apd_blocked(&f.w) && !pd_core_cached_solution(f.w.core) && !correction_ready(&f.w));
    assert(f.w.ref.n==0 && f.w.fbk.n==0 && f.w.acq_n==0);destroy(&f);
    init(&f);line(&f,"S1|apd enable=0 equalizer_active=1");
    assert(native_apd_blocked(&f.w) && !correction_ready(&f.w));
    line(&f,"S1|apd enable=0");assert(native_apd_blocked(&f.w));
    line(&f,"S1|apd sampler equalizer_active=0");assert(native_apd_blocked(&f.w));
    line(&f,"S1|apd equalizer_active=0");assert(!native_apd_blocked(&f.w) && !correction_ready(&f.w));destroy(&f);
    const char* bad_apd[]={"S1|apd enable=unknown","S1|apd enable=", "S1|apd equalizer_active=-1", "S1|apd enable=2", "S1|apd enable=00000000000000000000000000000000000000000000000000000000000000001"};
    for(unsigned i=0;i<sizeof(bad_apd)/sizeof(bad_apd[0]);++i){
        init(&f);line(&f,bad_apd[i]);assert(native_apd_blocked(&f.w) && !correction_ready(&f.w));
        line(&f,"S1|apd enable=00 equalizer_active=0");assert(!native_apd_blocked(&f.w) && !correction_ready(&f.w));destroy(&f);
    }
    puts("PASS: TX filter edges retire cache; native APD blocks correction/training until observed off; sampler and repeated status preserve valid cache");
    init(&f);f.w.tx_deadline_ns=12000000;f.w.tx_last_send_ns=pd_iq_monotonic_ns()-5000000;
    mic(&f);assert(f.w.tx_deadline_faults==0 && correction_ready(&f.w));
    f.w.tx_last_send_ns=pd_iq_monotonic_ns()-20000000;mic(&f);
    assert(f.w.tx_deadline_faults==1 && f.w.local_iq_fault && !f.w.aligned);
    assert(!correction_ready(&f.w) && !pd_core_cached_solution(f.w.core));destroy(&f);
    init(&f);f.w.tx_deadline_ns=12000000;pause_clean(&f);
    f.w.tx_last_send_ns=pd_iq_monotonic_ns()-20000000;
    line(&f,"S1|interlock state=PTT_REQUESTED");assert(f.w.tx_last_send_ns==0);
    line(&f,"S1|interlock state=TRANSMITTING");iq(&f);mic(&f);
    assert(f.w.tx_deadline_faults==0 && f.w.cached_packets>0 && correction_ready(&f.w));destroy(&f);
    init(&f);f.w.tx_last_send_ns=pd_iq_monotonic_ns()-20000000;mic(&f);
    assert(f.w.tx_deadline_faults==0 && correction_ready(&f.w));destroy(&f);
    init(&f);
    char status[2048];assert(status_json(&f.w,status,sizeof(status))>0);
    assert(strstr(status,"\"scheduler_policy\":0") && strstr(status,"\"scheduler_priority\":0"));
    f.w.udp=42;sent_bytes=0;
    f.w.tx_deadline_ns=12000000;f.w.tx_last_send_ns=pd_iq_monotonic_ns()-20000000;f.w.tx_deadline_in_packet=0;
    float output[256]={0};int count=0;send_pairs(&f.w,f.w.tx_in,&count,output,128);
    assert(sent_bytes==1052);
    assert(f.w.tx_deadline_faults==1 && f.w.local_iq_fault && !correction_ready(&f.w));
    assert(f.w.ref.n==0 && f.w.fbk.n==0);f.w.udp=-1;destroy(&f);
    init(&f);f.w.udp=42;fail_send=1;
    count=0;send_pairs(&f.w,f.w.tx_in,&count,output,128);
    assert(f.w.tx_send_errors==1 && f.w.local_iq_fault && !correction_ready(&f.w));
    assert(f.w.ref.n==0 && f.w.fbk.n==0);fail_send=0;f.w.udp=-1;destroy(&f);
    init(&f);f.w.ref.n=f.w.fbk.n=0;f.w.settle_left=0;
    assert(!pd_align_set_reference_floor(f.w.align,NAN) && !pd_align_set_reference_floor(f.w.align,-1));
    for(int i=0;i<BLOCK;++i){fifo_push(&f.w.ref,.2*cos(i*.13),.2*sin(i*.13));fifo_push(&f.w.fbk,.004*cos(i*.13),.004*sin(i*.13));}
    calibrate(&f.w);f.w.weak_blocks=WEAK_BLOCKS-1;
    double offset_before=pd_align_offset_hz(f.w.align,RATE);uint32_t quiet_seed=7;
    for(int block=0;block<WEAK_BLOCKS+3;++block){
        for(int i=0;i<BLOCK;++i){
            quiet_seed=quiet_seed*1664525u+1013904223u;double nr=(quiet_seed>>8)/16777216.0-.5;
            quiet_seed=quiet_seed*1664525u+1013904223u;double ni=(quiet_seed>>8)/16777216.0-.5;
            fifo_push(&f.w.ref,1e-8*cos(i*.13),1e-8*sin(i*.13));fifo_push(&f.w.fbk,nr*.02,ni*.02);
        }
        calibrate(&f.w);
    }
    assert(f.w.aligned && pd_core_current_solution(f.w.core) && !f.w.local_iq_fault);
    assert(fabs(pd_align_offset_hz(f.w.align,RATE)-offset_before)<1e-12 && f.w.weak_blocks==WEAK_BLOCKS-1);
    double zeros[128]={0},quiet_tx[128],quiet_rx[128];
    int quiet_valid=pd_eq_apply(f.w.eq,zeros,zeros,64,quiet_tx,quiet_rx);
    assert(quiet_valid>0);for(int i=0;i<quiet_valid*2;++i){assert(fabs(quiet_tx[i])<1e-6);}
    for(int i=0;i<BLOCK;++i){fifo_push(&f.w.ref,.2*cos(i*.13),.2*sin(i*.13));fifo_push(&f.w.fbk,.004*cos(i*.13),.004*sin(i*.13));}
    calibrate(&f.w);assert(f.w.weak_blocks==0 && f.w.aligned && pd_core_current_solution(f.w.core));
    for(int block=0;block<WEAK_BLOCKS+2;++block){
        for(int i=0;i<BLOCK;++i){
            quiet_seed=quiet_seed*1664525u+1013904223u;double nr=(quiet_seed>>8)/16777216.0-.5;
            quiet_seed=quiet_seed*1664525u+1013904223u;double ni=(quiet_seed>>8)/16777216.0-.5;
            fifo_push(&f.w.ref,.2*cos(i*.13),.2*sin(i*.13));fifo_push(&f.w.fbk,nr*.2,ni*.2);
        }
        calibrate(&f.w);
    }
    assert(!f.w.aligned && !pd_core_current_solution(f.w.core) && f.w.local_iq_fault);destroy(&f);
    init(&f);f.w.ref.n=f.w.fbk.n=0;f.w.settle_left=0;pd_eq_reset(f.w.eq);
    for(int block=0;block<f.w.eq_train_len/BLOCK+2;++block){
        for(int i=0;i<BLOCK;++i){fifo_push(&f.w.ref,1e-8,0);fifo_push(&f.w.fbk,.01*cos(i*.7),.01*sin(i*.7));}
        calibrate(&f.w);
    }
    assert(f.w.aligned && !pd_eq_ready(f.w.eq) && !f.w.eq_pending && f.w.quiet_blocks>0);destroy(&f);
    puts("PASS: quiet feedback holds model/frequency, advances FIR histories and cannot train noise-only EQ; coherent resume works and active mismatch still retires correction");
    init(&f);f.w.gain=10;mic(&f);
    assert(f.w.mic_peak>0.1 && f.w.prelimit_peak>f.w.peak && f.w.limited_samples>0 && f.w.generated_samples>=128);
    int status_size=status_json(&f.w,status,sizeof(status));assert(status_size>0 && (size_t)status_size<sizeof(status));
    assert(strstr(status,"\"prelimit_peak\"") && strstr(status,"\"limited_samples\""));destroy(&f);
    setenv("AETHER_PD_TEST_TONES","710,1900",1);setenv("AETHER_PD_TEST_SECONDS",".25",1);
    init(&f);unsetenv("AETHER_PD_TEST_TONES");unsetenv("AETHER_PD_TEST_SECONDS");
    assert(f.w.test_f1>0 && f.w.test_left>0);
    cbp_t expected_mod;cbp_init(&expected_mod,200,2800);
    for(int block=0;block<2;++block){for(int i=0;i<128;++i){
        double expected_i,expected_q;float input=(float)(.2*cos((2*i)*.13));
        cbp_step(&expected_mod,f.w.gain*input,&expected_i,&expected_q);
    }}
    assert(expected_mod.pos==f.w.mod.pos && !memcmp(expected_mod.z,f.w.mod.z,sizeof(expected_mod.z)));
    f.w.tx_tap=capture_timed_test;
    zero_mic(&f);assert(tap_peak>1e-4);int test_packets=(f.w.test_left+127)/128+3;
    for(int i=0;i<test_packets;++i){zero_mic(&f);}
    assert(f.w.test_f1==0 && f.w.test_left==0 && tap_peak<1e-7);
    line(&f,"S1|interlock state=UNKEY_REQUESTED");line(&f,"S1|interlock state=READY");
    line(&f,"S1|interlock state=TRANSMITTING");iq(&f);zero_mic(&f);assert(f.w.test_f1==0 && tap_peak<1e-7);destroy(&f);
    for(int fault=0;fault<2;++fault){
        setenv("AETHER_PD_TEST_TONES","710,1900",1);setenv("AETHER_PD_TEST_SECONDS",".25",1);
        init(&f);unsetenv("AETHER_PD_TEST_TONES");unsetenv("AETHER_PD_TEST_SECONDS");assert(f.w.test_left>0);
        if(fault){local_feedback_invalidate(&f.w,1);}else{line(&f,"S1|interlock state=UNKEY_REQUESTED");}
        assert(f.w.test_left==0 && f.w.test_f1==0);destroy(&f);
    }
    setenv("AETHER_PD_TEST_TONES","710,1900",1);setenv("AETHER_PD_TEST_SECONDS",".25",1);
    init(&f);unsetenv("AETHER_PD_TEST_TONES");unsetenv("AETHER_PD_TEST_SECONDS");
    f.w.keyed=0;f.w.tx_on=0;pd_core_set_keyed(f.w.core,0);
    line(&f,"S1|interlock state=PTT_REQUESTED");assert(f.w.test_f1>0 && !f.w.keyed);
    line(&f,"S1|interlock state=READY");assert(f.w.test_left==0 && f.w.test_f1==0);
    line(&f,"S1|interlock state=PTT_REQUESTED");assert(f.w.test_f1==0);destroy(&f);
    setenv("AETHER_PD_TEST_TONES","710,1900",1);init(&f);unsetenv("AETHER_PD_TEST_TONES");
    line(&f,"S1|interlock state=UNKEY_REQUESTED");line(&f,"S1|interlock state=TRANSMITTING");
    assert(f.w.test_duration_samples==0 && f.w.test_f1>0);destroy(&f);
    setenv("AETHER_PD_TEST_TONES","710,1900",1);setenv("AETHER_PD_TEST_SECONDS",".25",1);
    init(&f);unsetenv("AETHER_PD_TEST_TONES");unsetenv("AETHER_PD_TEST_SECONDS");
    f.w.test_started=0;f.w.keyed=0;f.w.tx_on=1;pd_core_set_keyed(f.w.core,0);
    line(&f,"S1|slice 1 mode=USB");assert(f.w.test_f1==0 && f.w.test_left==0);destroy(&f);
    const char* bad_seconds[]={"0",".1","16","nan","inf","8garbage"};
    for(unsigned i=0;i<sizeof(bad_seconds)/sizeof(bad_seconds[0]);++i){
        wf_t bad;setenv("AETHER_PD_TEST_TONES","710,1900",1);setenv("AETHER_PD_TEST_SECONDS",bad_seconds[i],1);
        assert(!wf_init(&bad));unsetenv("AETHER_PD_TEST_TONES");unsetenv("AETHER_PD_TEST_SECONDS");
    }
    wf_t missing_tones;setenv("AETHER_PD_TEST_SECONDS","8",1);assert(!wf_init(&missing_tones));unsetenv("AETHER_PD_TEST_SECONDS");
    puts("PASS: bounded diagnostic source fades to normal mic, disarms on unkey/fault, never rearms, validates duration; legacy test mode preserved");
    puts("PASS: diagnostic mic peak and limiter counts expose normal-mic overdrive without changing the limiter");
    puts("PASS: final-send late dispatch and injected send failure retire paired histories; pthread scheduler query works in static musl");
    puts("PASS: 20 ms dispatch gap retires correction before output; 5 ms dispatch and warm-key idle preserve valid correction; opt-in guard leaves legacy mode unchanged");
    puts("PASS: fresh post-key IQ required; gap, stale TX/IQ, malformed IQ, interlock/configuration/mode changes invalidate cache; unrelated status preserves cache");
    return 0;
}
