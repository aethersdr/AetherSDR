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
static ssize_t discard_send(int fd,const void* bytes,size_t size,int flags,const struct sockaddr* peer,socklen_t peer_size)
{
    assert(fd==42);(void)flags;(void)peer;(void)peer_size;
    const unsigned char* packet=bytes;
    assert(size>=28);
    for (size_t offset=28;offset+4<=size;offset+=4) {
        double value=fabs(word_float(packet+offset));
        if (value>output_peak) { output_peak=value; }
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
static void check_retirement(unsigned char opcode,int ending)
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
    unsigned char arm[9]={opcode,0,0,0,0,0,0,0,1};
    assert(operator_calibration_command(&w,arm,sizeof(arm),pd_iq_monotonic_ns()).result==PD_CAL_OK);
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
    if (opcode=='M') {
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
    unsigned char arm[9]={opcode,0,0,0,0,0,0,0,1};
    assert(operator_calibration_command(&w,arm,sizeof(arm),pd_iq_monotonic_ns()).result==PD_CAL_OK);
    char key[]="S0|interlock state=TRANSMITTING tx_client_handle=0x00005678 source=SW reason= tx_allowed=1";
    api_line(&w,key);assert(w.keyed && w.calibration.phase==PD_CAL_RUNNING);
    mic_packet(&w,0);assert(sends>0 && output_peak>1e-8 && w.test_f1>0);
    unsigned before=sends;output_peak=0;
    w.tx_last_send_ns=pd_iq_monotonic_ns()-INT64_C(20000000);
    mic_packet(&w,1);
    assert(w.tx_deadline_faults==1 && !w.tx_send_errors && !w.tx_rejected);
    assert(w.keyed && w.calibration.phase==PD_CAL_CANCELED && !w.test_f1 && !w.test_left);
    assert(sends>before && output_peak==0 && !correction_ready(&w));
    mic_packet(&w,2);assert(w.calibration.phase==PD_CAL_CANCELED && !w.test_f1 && output_peak==0);
    api_line(&w,idle);assert(!w.keyed);
    pd_acquire_destroy(w.acquire);pd_core_destroy(w.core);
    pd_align_destroy(w.align);pd_eq_destroy(w.eq);pd_eq_destroy(w.trusted_eq);
    free(w.ref.buf);free(w.fbk.buf);free(w.acq_tx);free(w.acq_rx);
}
int main(void)
{
    check_source('A');check_source('M');
    for (int ending=0;ending<3;++ending) { check_retirement('M',ending);check_retirement('A',ending); }
    puts("deadline fault cancels bounded sources; measurement retirement/late dispatch holds silence with live mic until READY; calibration returns to voice; socket-free");
    return 0;
}
