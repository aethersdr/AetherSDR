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
static void mic_packet(wf_t* w,unsigned count)
{
    float silence[256]={0};unsigned char packet[28+sizeof(silence)];
    int size=vita_build(packet,w->tx_in,CLASS_AUDIO_24K,(int)count,silence,128);
    w->tx_arrival_ns=pd_iq_monotonic_ns();w->tx_age_ns=0;
    on_packet(w,packet,size);
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
    puts("on_packet deadline fault cancels calibration and measurement tones before generated output; no automatic rearm; socket-free");
    return 0;
}
