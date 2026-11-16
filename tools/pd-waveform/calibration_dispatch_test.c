#include <sys/socket.h>
static ssize_t discard_send(int fd,const void* bytes,size_t size,int flags,const struct sockaddr* peer,socklen_t peer_size);
#define sendto discard_send
#define main pd_waveform_program_main
#include "pd_waveform.c"
#undef main
#undef sendto
#undef NDEBUG
#include <assert.h>

static unsigned sends;
static double output_peak;
enum { kSpectrumSamples = 48000 };
static float spectrum_samples[2*kSpectrumSamples];
static unsigned spectrum_count;
static int collect_spectrum;
static ssize_t discard_send(int fd,const void* bytes,size_t size,int flags,const struct sockaddr* peer,socklen_t peer_size)
{
    assert(fd==42);(void)flags;(void)peer;(void)peer_size;
    const unsigned char* packet=bytes;
    assert(size>=28);
    for (size_t offset=28;offset+4<=size;offset+=4) {
        double value=fabs(word_float(packet+offset));
        if (value>output_peak) { output_peak=value; }
    }
    if (collect_spectrum) {
        for (size_t offset=28;offset+8<=size && spectrum_count<kSpectrumSamples;offset+=8) {
            spectrum_samples[2*spectrum_count]=word_float(packet+offset);
            spectrum_samples[2*spectrum_count+1]=word_float(packet+offset+4);
            ++spectrum_count;
        }
    }
    ++sends;
    return (ssize_t)size;
}
static void mic_packet_value(wf_t* w,unsigned count,double level)
{
    float input[256]={0};unsigned char packet[28+sizeof(input)];
    for (int i=0;i<128;++i) {
        input[2*i]=input[2*i+1]=(float)(level*sin(2*M_PI*1000*(count*128+i)/RATE));
    }
    int size=vita_build(packet,w->tx_in,CLASS_AUDIO_24K,(int)count,input,128);
    w->tx_arrival_ns=pd_iq_monotonic_ns();w->tx_age_ns=0;
    on_packet(w,packet,size);
}
static void mic_packet(wf_t* w,unsigned count) { mic_packet_value(w,count,0); }
static struct pd_calibration_reply arm_source(wf_t* w,unsigned char opcode,unsigned spacing)
{
    unsigned char arm[11]={opcode,0,0,0,0,0,0,0,1,(unsigned char)(spacing>>8),(unsigned char)spacing};
    return operator_calibration_command(w,arm,opcode=='Q'?11:9,pd_iq_monotonic_ns());
}
static void check_retirement(unsigned char opcode,int ending,unsigned spacing)
{
    sends=0;output_peak=0;
    unsetenv("AETHER_PD_TEST_TONES");unsetenv("AETHER_PD_TEST_SECONDS");
    setenv("AETHER_PD_FEEDBACK","local",1);setenv("AETHER_PD_TX_DEADLINE_MS","25",1);
    wf_t w;assert(wf_init(&w));w.api=-1;w.api_leased=1;w.tx_in=0x81000001;w.active_slice=0;
    w.udp=42;w.tx_mhz=w.pan_mhz=7.15;w.daxiq_sid=0x20000000;w.local_iq.frames=1;
    w.calibration_gui=0x5678;w.calibration_slice_owner[0]=0x5678;w.calibration_control=1;
    char idle[]="S0|interlock state=READY tx_client_handle=0x00000000 source= reason= tx_allowed=1";
    char key[]="S0|interlock state=TRANSMITTING tx_client_handle=0x00005678 source=SW reason= tx_allowed=1";
    api_line(&w,idle);w.api=1;w.local_iq.last_valid_ns=pd_iq_monotonic_ns();
    assert(arm_source(&w,opcode,spacing).result==PD_CAL_OK);
    api_line(&w,key);assert(w.keyed && w.calibration.phase==PD_CAL_RUNNING);
    for (unsigned i=0;i<16;++i) {
        w.tx_last_send_ns=pd_iq_monotonic_ns();mic_packet_value(&w,i,.3);
    }
    assert(output_peak>1e-8 && sends>0);
    if (ending==0) { w.calibration.deadline_ns=pd_iq_monotonic_ns()-1; }
    else if (ending==1) { w.test_left=w.calibration.samples_left=1; }
    else {
        unsigned char cancel[9]={'X',0,0,0,0,0,0,0,2};
        assert(operator_calibration_command(&w,cancel,sizeof(cancel),pd_iq_monotonic_ns()).applied);
    }
    output_peak=0;
    for (unsigned i=16;i<32;++i) {
        w.tx_last_send_ns=pd_iq_monotonic_ns();mic_packet_value(&w,i,.3);
    }
    assert(w.keyed && !w.test_f1 && !w.tx_deadline_faults);
    if (opcode!='A') {
        assert(output_peak==0); // Measurement retirement cannot expose live microphone input.
        float prepared[256];
        for (unsigned i=0;i<256;++i) { prepared[i]=.2f; }
        // A packet can have been generated before expiry and dispatched afterward.
        w.calibration.phase=PD_CAL_RUNNING;w.calibration.samples_left=128;
        w.calibration.deadline_ns=pd_iq_monotonic_ns()-1;w.test_f1=710;w.test_left=128;
        send_pairs(&w,w.tx_in,&w.tx_count,prepared,128);
        assert(output_peak==0 && w.calibration.phase==PD_CAL_EXPIRED && !w.test_f1);
        char unknown[]="S0|interlock state=READY";
        api_line(&w,unknown);api_line(&w,key);
        for (unsigned i=32;i<40;++i) {
            w.tx_last_send_ns=pd_iq_monotonic_ns();mic_packet_value(&w,i,.3);
        }
        assert(output_peak==0); // Incomplete status cannot release the hold.
    } else { assert(output_peak>1e-4); } // Explicit voice calibration retains its handoff.
    api_line(&w,idle);assert(!w.keyed);
    w.local_iq.last_valid_ns=pd_iq_monotonic_ns();api_line(&w,key);
    output_peak=0;
    for (unsigned i=40;i<56;++i) {
        w.tx_last_send_ns=pd_iq_monotonic_ns();mic_packet_value(&w,i,.3);
    }
    assert(!w.test_f1 && output_peak>1e-4); // A subsequent operator key can carry voice.
    api_line(&w,idle);
    pd_acquire_destroy(w.acquire);pd_core_destroy(w.core);
    pd_align_destroy(w.align);pd_eq_destroy(w.eq);pd_eq_destroy(w.trusted_eq);
    free(w.ref.buf);free(w.fbk.buf);free(w.acq_tx);free(w.acq_rx);
}
static void check_source(unsigned char opcode)
{
    sends=0;output_peak=0;
    unsetenv("AETHER_PD_TEST_TONES");unsetenv("AETHER_PD_TEST_SECONDS");
    setenv("AETHER_PD_FEEDBACK","local",1);setenv("AETHER_PD_TX_DEADLINE_MS","12",1);
    wf_t w;assert(wf_init(&w));w.api=-1;w.api_leased=1;w.tx_in=0x81000001;w.active_slice=0;
    w.udp=42; // Sentinel for discard_send; no socket is opened.
    w.tx_mhz=w.pan_mhz=7.15;w.daxiq_sid=0x20000000;w.local_iq.frames=1;
    w.calibration_gui=0x5678;w.calibration_slice_owner[0]=0x5678;w.calibration_control=1;
    char idle[]="S0|interlock state=READY tx_client_handle=0x00000000 source= reason= tx_allowed=1";
    api_line(&w,idle);w.api=1;w.local_iq.last_valid_ns=pd_iq_monotonic_ns();
    assert(arm_source(&w,opcode,10000).result==PD_CAL_OK);
    char key[]="S0|interlock state=TRANSMITTING tx_client_handle=0x00005678 source=SW reason= tx_allowed=1";
    api_line(&w,key);assert(w.keyed && w.calibration.phase==PD_CAL_RUNNING);
    mic_packet(&w,0);assert(sends>0 && output_peak>1e-8 && test_source_active(&w));
    unsigned before=sends;output_peak=0;
    uint32_t source_events_before=w.source_end_event.count;
    w.tx_last_send_ns=pd_iq_monotonic_ns()-INT64_C(20000000);
    mic_packet(&w,1);
    assert(w.tx_deadline_faults==1 && !w.tx_send_errors && !w.tx_rejected);
    assert(w.keyed && w.calibration.phase==PD_CAL_CANCELED && !w.test_f1 && !w.test_left);
    assert(w.source_end_event.count==source_events_before+1 && !w.source_end_event.truncated);
    assert(strstr(w.source_end_event.text,"reason=feedback_invalidated") &&
        strstr(w.source_end_event.text,"phase=running") && strstr(w.source_end_event.text,"keyed=1") &&
        strstr(w.source_end_event.text,"software_owner=1") && strstr(w.source_end_event.text,"gui=00005678"));
    char diagnostic_status[8192];
    int diagnostic_size=status_json(&w,diagnostic_status,sizeof(diagnostic_status));
    assert(diagnostic_size>0 && (size_t)diagnostic_size<sizeof(diagnostic_status));
    assert(strstr(diagnostic_status,w.source_end_event.text));
    printf("DIAGNOSTIC_FIXTURE %s",diagnostic_status);
    assert(sends>before && output_peak==0 && !correction_ready(&w));
    mic_packet(&w,2);assert(w.calibration.phase==PD_CAL_CANCELED && !w.test_f1 && output_peak==0);
    api_line(&w,idle);assert(!w.keyed);
    assert(w.source_end_event.count==source_events_before+1 && strstr(w.source_end_event.text,"phase=running"));
    pd_acquire_destroy(w.acquire);pd_core_destroy(w.core);
    pd_align_destroy(w.align);pd_eq_destroy(w.eq);pd_eq_destroy(w.trusted_eq);
    free(w.ref.buf);free(w.fbk.buf);free(w.acq_tx);free(w.acq_rx);
}
static double projected_amplitude(double frequency)
{
    double real=0,imag=0;
    for (unsigned n=0;n<spectrum_count;++n) {
        double angle=2*M_PI*frequency*n/RATE;
        real+=spectrum_samples[2*n]*cos(angle)+spectrum_samples[2*n+1]*sin(angle);
        imag+=spectrum_samples[2*n+1]*cos(angle)-spectrum_samples[2*n]*sin(angle);
    }
    return hypot(real,imag)/spectrum_count;
}
static void check_configured_spectrum(unsigned spacing,double lower,double upper)
{
    setenv("AETHER_PD_FEEDBACK","local",1);setenv("AETHER_PD_TX_DEADLINE_MS","25",1);
    unsetenv("AETHER_PD_TEST_TONES");unsetenv("AETHER_PD_TEST_SECONDS");
    wf_t w;assert(wf_init(&w));w.api=1;w.api_leased=1;w.tx_in=0x81000001;w.active_slice=0;
    w.udp=42;w.tx_mhz=w.pan_mhz=7.15;w.daxiq_sid=0x20000000;w.local_iq.frames=1;w.correct=0;
    w.calibration_gui=0x5678;w.calibration_slice_owner[0]=0x5678;w.calibration_control=1;
    char idle[]="S0|interlock state=READY tx_client_handle=0x00000000 source= reason= tx_allowed=1";
    char key[]="S0|interlock state=TRANSMITTING tx_client_handle=0x00005678 source=SW reason= tx_allowed=1";
    api_line(&w,idle);w.local_iq.last_valid_ns=pd_iq_monotonic_ns();
    assert(arm_source(&w,'Q',spacing).applied && !w.keyed && !test_source_active(&w));
    api_line(&w,key);
    assert(w.test_noise_left==96000 && w.test_left==192000 && w.measurement_key);
    // The fixed preamble is checked above; inspect only the generated tone interval.
    w.test_noise_left=0;spectrum_count=0;collect_spectrum=1;output_peak=0;
    for (unsigned n=0;n<kSpectrumSamples/128;++n) {
        w.local_iq.last_valid_ns=w.tx_last_send_ns=pd_iq_monotonic_ns();
        mic_packet_value(&w,n,.3);
    }
    collect_spectrum=0;
    assert(spectrum_count==kSpectrumSamples && w.calibration.phase==PD_CAL_RUNNING && !w.tx_deadline_faults);
    double a=projected_amplitude(lower),b=projected_amplitude(upper);
    assert(fabs(a-w.peak/2)<1e-6 && fabs(b-w.peak/2)<1e-6 && output_peak<=w.peak+1e-6);
    assert(projected_amplitude(lower+1)<1e-6 && projected_amplitude(upper+1)<1e-6);
    printf("spacing %u Hz: emitted %.1f/%.1f Hz, amplitudes %.6f/%.6f, peak %.6f\n",spacing,lower,upper,a,b,output_peak);
    api_line(&w,idle);
    pd_acquire_destroy(w.acquire);pd_core_destroy(w.core);
    pd_align_destroy(w.align);pd_eq_destroy(w.eq);pd_eq_destroy(w.trusted_eq);
    free(w.ref.buf);free(w.fbk.buf);free(w.acq_tx);free(w.acq_rx);
}
static void check_binding_status(const wf_t* w,unsigned channel,int valid,int failed)
{
    char status[4096],expected[192];int size=status_json(w,status,sizeof(status));
    assert(size>0 && size<(int)sizeof(status));
    snprintf(expected,sizeof(expected),"\"feedback_binding\":{\"transport\":\"local_dax_iq\",\"channel\":%u,\"pan\":1073741825,\"gui_handle\":287454020,\"valid\":%s,\"failed\":%s}",channel,valid?"true":"false",failed?"true":"false");
    assert(strstr(status,expected));
}
static void check_feedback_binding(unsigned channel)
{
    setenv("AETHER_PD_FEEDBACK","local",1);
    setenv("AETHER_PD_GUI_HANDLE","0x11223344",1);
    setenv("AETHER_PD_FEEDBACK_PAN","0x40000001",1);
    unsetenv("AETHER_PD_TEST_TONES");unsetenv("AETHER_PD_TEST_SECONDS");
    char selected[2]={(char)('0'+channel),0};setenv("AETHER_PD_DAXIQ",selected,1);
    char wrong_channel[96];snprintf(wrong_channel,sizeof(wrong_channel),"S0|display pan 0x40000001 daxiq_channel=%u",channel==4?1:channel+1);
    const char* faults[]={
        "S0|display pan 0x40000001 client_handle=0x55667788",
        wrong_channel,
        "S0|display pan 0x40000001 removed",
        "S0|display pan 0x40000001 client_handle=0x11223344 client_handle=0x11223344",
        "S0|display pan 0x40000001 daxiq_channel=1 daxiq_channel=1",
        "S0|display pan 0x40000001garbage daxiq_channel=1"
    };
    for(unsigned scenario=0;scenario<2*sizeof(faults)/sizeof(faults[0]);++scenario){
        unsigned f=scenario/2;
        wf_t w;assert(wf_init(&w) && w.daxiq_ch==(int)channel);w.api=-1;w.api_leased=1;w.tx_in=0x81000001;w.active_slice=0;
        w.tx_mhz=7.15;w.daxiq_sid=0x20000000;w.local_iq.frames=1;
        w.calibration_slice_owner[0]=w.calibration_gui;w.calibration_control=1;
        char idle[]="S0|interlock state=READY tx_client_handle=0x00000000 source= reason= tx_allowed=1";
        api_line(&w,idle);
        char partial[128];snprintf(partial,sizeof(partial),"S0|display pan 0x40000001 center=7.150000 daxiq_channel=%u",channel);
        api_line(&w,partial);assert(!feedback_binding_valid(&w));check_binding_status(&w,channel,0,0);
        char owned[]="S0|display pan 0x40000001 client_handle=0x11223344";
        api_line(&w,owned);assert(feedback_binding_valid(&w));check_binding_status(&w,channel,1,0);
        uint64_t resets=w.correction_resets;
        char foreign[]="S0|display pan 0x40000002 client_handle=0x55667788 daxiq_channel=1 center=50.000000";
        api_line(&w,foreign);assert(feedback_binding_valid(&w) && w.feedback_pan==0x40000001 && w.pan_mhz==7.15 && w.correction_resets==resets);
        w.api=1;w.local_iq.last_valid_ns=pd_iq_monotonic_ns();
        assert(arm_source(&w,'M',1190).result==PD_CAL_OK && w.calibration.phase==PD_CAL_ARMED);
        if(scenario%2){
            char key[]="S0|interlock state=TRANSMITTING tx_client_handle=0x11223344 source=SW reason= tx_allowed=1";
            api_line(&w,key);assert(w.keyed && w.calibration.phase==PD_CAL_RUNNING);
        }
        w.feedback_ok=1;
        char fault[160];snprintf(fault,sizeof(fault),"%s",faults[f]);api_line(&w,fault);
        assert(w.feedback_binding_failed && !feedback_binding_valid(&w) && !w.feedback_ok &&
            w.calibration.phase!=PD_CAL_ARMED && w.calibration.phase!=PD_CAL_RUNNING);
        check_binding_status(&w,channel,0,1);
        api_line(&w,idle);
        char restored[160];snprintf(restored,sizeof(restored),"S0|display pan 0x40000001 client_handle=0x11223344 daxiq_channel=%u center=7.150000",channel);
        api_line(&w,restored);assert(!feedback_binding_valid(&w));
        unsigned char retry[9]={'M',0,0,0,0,0,0,0,2};
        w.local_iq.last_valid_ns=pd_iq_monotonic_ns();
        assert(operator_calibration_command(&w,retry,sizeof(retry),pd_iq_monotonic_ns()).result==PD_CAL_BLOCKED);
        pd_acquire_destroy(w.acquire);pd_core_destroy(w.core);
        pd_align_destroy(w.align);pd_eq_destroy(w.eq);pd_eq_destroy(w.trusted_eq);
        free(w.ref.buf);free(w.fbk.buf);free(w.acq_tx);free(w.acq_rx);
    }
    unsetenv("AETHER_PD_DAXIQ");
    unsetenv("AETHER_PD_FEEDBACK_PAN");unsetenv("AETHER_PD_GUI_HANDLE");
}
static void check_acquisition_evidence(void)
{
    unsetenv("AETHER_PD_TEST_TONES");unsetenv("AETHER_PD_TEST_SECONDS");
    setenv("AETHER_PD_FEEDBACK","local",1);
    setenv("AETHER_PD_MAX_DELAY_MS","25",1);
    wf_t w;assert(wf_init(&w));w.api=-1;
    w.acquisition_generation=17;w.calibration.source_id=0x1234;
    w.key_started_ns=pd_iq_monotonic_ns();w.settle_left=0;
    uint32_t seed=13;
    int64_t deadline=pd_iq_monotonic_ns()+10000000000LL;
    while (!w.aligned) {
        for (unsigned i=0;i<BLOCK;++i) {
            seed=seed*1664525u+1013904223u;
            double value=(seed>>8)/16777216.0-0.5;
            fifo_push(&w.ref,value,0);fifo_push(&w.fbk,value,0);
        }
        calibrate(&w);
        assert(pd_iq_monotonic_ns()<deadline);
        if (w.acquisition_pending) {
            struct timespec pause={0,1000000};nanosleep(&pause,NULL);
        }
    }
    assert(w.alignment_event.count==1 && !w.alignment_event.truncated);
    assert(strstr(w.alignment_event.text,"alignment evidence: reason=acquisition source=0000000000001234 generation=17"));
    assert(strstr(w.alignment_event.text,"submitted_ns=") && strstr(w.alignment_event.text,"consumed_ns="));
    char json[8192];int size=status_json(&w,json,sizeof(json));
    assert(size>0 && (size_t)size<sizeof(json) && strstr(json,w.alignment_event.text));
    printf("ACQUISITION_FIXTURE %s",json);
    // Failed estimates are also evidence; they must not be hidden by the
    // reset-event guard that requires an already aligned reference.
    struct pd_acquire_result rejected={.generation=18,.delay=-1,.envelope_correlation=0.2};
    w.aligned=0;log_alignment_acquisition(&w,&rejected);
    assert(w.alignment_event.count==2 && strstr(w.alignment_event.text,"delay=-1.000"));
    w.alignment_event.count=UINT32_MAX;
    log_alignment_acquisition(&w,&rejected);assert(w.alignment_event.count==UINT32_MAX);
    pd_acquire_destroy(w.acquire);pd_core_destroy(w.core);
    pd_align_destroy(w.align);pd_eq_destroy(w.eq);pd_eq_destroy(w.trusted_eq);
    free(w.ref.buf);free(w.fbk.buf);free(w.acq_tx);free(w.acq_rx);
    unsetenv("AETHER_PD_MAX_DELAY_MS");
}

static void check_bypass_measurement_readiness(void)
{
    unsetenv("AETHER_PD_TEST_TONES");unsetenv("AETHER_PD_TEST_SECONDS");
    setenv("AETHER_PD_FEEDBACK","local",1);
    wf_t w;assert(wf_init(&w));w.api=-1;
    int64_t now=pd_iq_monotonic_ns();
    w.calibration.phase=PD_CAL_RUNNING;w.calibration.purpose=PD_SOURCE_MEASUREMENT;
    w.calibration.source_id=23;w.calibration.deadline_ns=now+2000000000;
    w.keyed=w.aligned=w.feedback_ok=1;w.correct=0;w.local_iq_fault=1;
    w.tx_guard.seen=1;w.test_noise_left=0;w.test_left=RATE;
    w.test_f1=pd_calibration_tone1(&w.calibration);w.test_f2=pd_calibration_tone2(&w.calibration);
    w.measurement_tone_sent_ns=now-w.measurement_guard_ns-1000000;
    w.local_iq.last_valid_ns=now;w.local_iq.last_age_ns=0;
    assert(!pd_core_current_solution(w.core) && !pd_eq_ready(w.eq));
    assert(measurement_tones_ready(&w,now));
    assert(!capture_meta(&w).correcting);
    w.correct=1;assert(!measurement_tones_ready(&w,now));
    w.local_iq_fault=0;assert(!measurement_tones_ready(&w,now));
    w.correct=0;w.local_iq_fault=1;
    w.native_apd_enabled=1;assert(!measurement_tones_ready(&w,now));w.native_apd_enabled=0;
    w.native_apd_active=1;assert(!measurement_tones_ready(&w,now));w.native_apd_active=0;
    w.tx_guard.seen=0;assert(!measurement_tones_ready(&w,now));w.tx_guard.seen=1;
    w.tx_guard.reset_pending=1;assert(!measurement_tones_ready(&w,now));w.tx_guard.reset_pending=0;
    w.local_iq.last_valid_ns=now-100000001;assert(!measurement_tones_ready(&w,now));w.local_iq.last_valid_ns=now;
    w.local_iq.last_age_ns=25000001;assert(!measurement_tones_ready(&w,now));w.local_iq.last_age_ns=0;
    w.aligned=0;assert(!measurement_tones_ready(&w,now));w.aligned=1;
    w.test_noise_left=1;assert(!measurement_tones_ready(&w,now));w.test_noise_left=0;
    assert(measurement_tones_ready(&w,now));
    local_feedback_invalidate(&w,1);
    assert(w.calibration.phase==PD_CAL_CANCELED && !measurement_tones_ready(&w,now));
    pd_acquire_destroy(w.acquire);pd_core_destroy(w.core);
    pd_align_destroy(w.align);pd_eq_destroy(w.eq);pd_eq_destroy(w.trusted_eq);
    free(w.ref.buf);free(w.fbk.buf);free(w.acq_tx);free(w.acq_rx);
}

int main(void)
{
    check_bypass_measurement_readiness();
    check_acquisition_evidence();
    for(unsigned channel=1;channel<=4;++channel){check_feedback_binding(channel);}
    check_source('A');check_source('M');check_source('Q');
    for (int ending=0;ending<3;++ending) {
        check_retirement('M',ending,1190);check_retirement('A',ending,1190);
        check_retirement('Q',ending,10000);check_retirement('Q',ending,2610);
    }
    check_configured_spectrum(200,1205,1405);
    check_configured_spectrum(1001,804.5,1805.5);
    check_configured_spectrum(1190,710,1900);
    check_configured_spectrum(2610,0,2610);
    check_configured_spectrum(10000,-3695,6305);
    puts("deadline fault cancels bounded sources; measurement retirement/late dispatch holds silence with live mic until READY; calibration returns to voice; socket-free");
    return 0;
}
