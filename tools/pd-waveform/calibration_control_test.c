#define main pd_waveform_program_main
#include "pd_waveform.c"
#undef main
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>

static struct pd_calibration_reply request(wf_t* w, unsigned char op, unsigned nonce)
{
    unsigned char bytes[9] = {op}; bytes[8] = (unsigned char)nonce;
    return operator_calibration_command(w, bytes, sizeof(bytes), pd_iq_monotonic_ns());
}
int main(void)
{
    unsetenv("AETHER_PD_TEST_TONES"); unsetenv("AETHER_PD_TEST_SECONDS");
    setenv("AETHER_PD_FEEDBACK", "local", 1);
    wf_t w; assert(wf_init(&w));
    w.udp = -1; w.api = 1; w.api_leased = 1; w.tx_in = 0x81000001;
    w.active_slice = 0; w.tx_mhz = w.pan_mhz = 7.15;
    w.daxiq_sid = 0x20000000; w.local_iq.frames = 1;
    w.calibration_gui=0x5678;w.calibration_slice_owner[0]=0x5678;
    w.calibration_idle=1;w.calibration_software_owner=1;
    w.calibration_control = 1; w.local_iq.last_valid_ns = pd_iq_monotonic_ns();
    char live_tx[]="S0|transmit freq=7.150000 rfpower=5 lo=50 hi=4000";
    api_line(&w,live_tx);
    assert(request(&w, 'A', 1).result == PD_CAL_OK);
    assert(w.calibration.phase == PD_CAL_ARMED && !w.keyed && w.test_f1 == 0);
    uint64_t generation = w.acquisition_generation;
    char stored_band[]="S0|transmit band 2 band_name=80 rfpower=9 tunepower=15 hwalc_enabled=0 inhibit=0";
    api_line(&w,stored_band);
    assert(w.calibration.phase==PD_CAL_ARMED && w.acquisition_generation==generation);
    assert(!strcmp(w.model_fields[9],"5"));
    assert(!request(&w, 'A', 1).applied && w.acquisition_generation == generation);
    assert(request(&w, 'A', 2).result == PD_CAL_BUSY);
    set_keyed(&w, 1, 0);
    assert(w.calibration.phase == PD_CAL_RUNNING && w.test_f1 == 710 && w.test_f2 == 1900);
    assert(w.test_left == PD_CAL_SAMPLES);
    assert(request(&w, 'A', 3).result == PD_CAL_BUSY);
    assert(request(&w, 'X', 4).result == PD_CAL_OK);
    assert(w.calibration.phase == PD_CAL_CANCELED && w.test_f1 == 0 && w.test_left == 0);
    assert(w.keyed); // Cancel removes the source; it does not control PTT.
    set_keyed(&w, 0, 1); w.local_iq.last_valid_ns = pd_iq_monotonic_ns();
    assert(request(&w, 'A', 5).applied);
    model_changed(&w); assert(w.calibration.phase == PD_CAL_CANCELED);
    w.local_iq.last_valid_ns = pd_iq_monotonic_ns();
    assert(request(&w, 'A', 6).applied);
    w.local_iq.last_valid_ns = 1; set_keyed(&w, 1, 0);
    assert(w.calibration.phase == PD_CAL_CANCELED && w.test_f1 == 0);
    set_keyed(&w, 0, 1); w.local_iq.last_valid_ns = pd_iq_monotonic_ns();
    assert(request(&w, 'A', 7).applied);
    w.local_iq.last_valid_ns = pd_iq_monotonic_ns(); set_keyed(&w, 1, 0);
    assert(w.test_f1 == 710);
    local_feedback_invalidate(&w, 1);
    assert(w.calibration.phase == PD_CAL_CANCELED && w.test_f1 == 0);
    set_keyed(&w, 0, 0); w.local_iq.last_valid_ns = pd_iq_monotonic_ns();
    w.native_apd_enabled = 1; assert(request(&w, 'A', 8).result == PD_CAL_BLOCKED);
    w.native_apd_enabled = 0; w.calibration_control = 0;
    assert(request(&w, 'A', 9).result == PD_CAL_BLOCKED);
    w.calibration_control=1;w.local_iq.last_valid_ns=pd_iq_monotonic_ns();
    assert(request(&w,'A',10).applied);
    char other[]="S0|interlock state=PTT_REQUESTED tx_client_handle=0x00009999 source=SW reason= tx_allowed=1";
    api_line(&w,other);assert(w.calibration.phase==PD_CAL_CANCELED && w.test_f1==0);
    char idle[]="S0|interlock state=READY tx_client_handle=0x00000000 source= reason= tx_allowed=1";
    api_line(&w,idle);w.local_iq.last_valid_ns=pd_iq_monotonic_ns();assert(request(&w,'A',11).applied);
    char malformed[]="S0|interlock state=PTT_REQUESTED tx_client_handle=0x00005678 source=SW source=MIC reason= tx_allowed=1";
    api_line(&w,malformed);assert(w.calibration.phase==PD_CAL_CANCELED && w.test_f1==0);
    api_line(&w,idle);w.local_iq.last_valid_ns=pd_iq_monotonic_ns();assert(request(&w,'A',12).applied);
    char denied[]="S0|interlock state=TRANSMITTING tx_client_handle=0x00005678 source=SW reason=FAULT tx_allowed=0";
    api_line(&w,denied);assert(w.calibration.phase==PD_CAL_CANCELED && w.test_f1==0);
    api_line(&w,idle);
    w.local_iq.last_valid_ns=pd_iq_monotonic_ns();assert(request(&w,'A',13).applied);
    char own[]="S0|interlock state=TRANSMITTING tx_client_handle=0x00005678 source=SW reason= tx_allowed=1";
    api_line(&w,own);assert(w.calibration.phase==PD_CAL_RUNNING && !w.test_started);
    local_feedback_suspend_transport(&w);assert(w.calibration.phase==PD_CAL_CANCELED && w.test_f1==0);
    api_line(&w,idle);w.local_iq.last_valid_ns=pd_iq_monotonic_ns();assert(request(&w,'A',14).applied);
    char changed_live_tx[]="S0|transmit rfpower=6";
    api_line(&w,changed_live_tx);assert(w.calibration.phase==PD_CAL_CANCELED && !strcmp(w.model_fields[9],"6"));
    w.local_iq.last_valid_ns=pd_iq_monotonic_ns();
    generation=w.acquisition_generation;
    uint64_t resets=w.correction_resets;
    assert(request(&w,'M',15).applied);
    assert(w.calibration.purpose==PD_SOURCE_MEASUREMENT && !w.keyed && !w.test_f1);
    char measurement_status[4096];
    int measurement_size=status_json(&w,measurement_status,sizeof(measurement_status));
    assert(measurement_size>0 && (size_t)measurement_size<sizeof(measurement_status));
    assert(strstr(measurement_status,"\"measurement_control\":true"));
    assert(strstr(measurement_status,"\"source_purpose\":\"measurement\""));

    assert(w.acquisition_generation==generation && w.correction_resets==resets);
    int64_t measurement_deadline=w.calibration.deadline_ns;
    assert(!request(&w,'M',15).applied && w.calibration.deadline_ns==measurement_deadline);
    assert(request(&w,'A',16).result==PD_CAL_BUSY);
    assert(w.calibration.purpose==PD_SOURCE_MEASUREMENT && w.acquisition_generation==generation);
    api_line(&w,own);
    assert(w.keyed && w.calibration.phase==PD_CAL_RUNNING && w.test_f1==710 && w.test_f2==1900);
    assert(w.test_noise_left==TEST_NOISE_SAMPLES);
    assert(request(&w,'X',17).applied && w.keyed && !w.test_f1);
    api_line(&w,idle);w.local_iq.last_valid_ns=pd_iq_monotonic_ns();
    w.correct=0;
    assert(request(&w,'A',18).result==PD_CAL_BLOCKED);
    assert(request(&w,'M',19).applied);
    api_line(&w,own);
    assert(w.calibration.phase==PD_CAL_RUNNING && w.test_f1==710 && !w.correct);
    api_line(&w,idle);
    assert(w.calibration.phase==PD_CAL_CANCELED && !w.test_f1);
    w.correct=1;w.local_iq.last_valid_ns=pd_iq_monotonic_ns();
    assert(request(&w,'M',20).applied);
    model_changed(&w);assert(w.calibration.phase==PD_CAL_CANCELED);
    w.local_iq.last_valid_ns=1;assert(request(&w,'M',21).result==PD_CAL_BLOCKED);
    w.local_iq.last_valid_ns=pd_iq_monotonic_ns();assert(request(&w,'M',22).applied);
    api_line(&w,other);assert(w.calibration.phase==PD_CAL_CANCELED && !w.test_f1);
    pd_acquire_destroy(w.acquire); pd_core_destroy(w.core);
    pd_align_destroy(w.align); pd_eq_destroy(w.eq); pd_eq_destroy(w.trusted_eq);
    free(w.ref.buf); free(w.fbk.buf); free(w.acq_tx); free(w.acq_rx);
    puts("native operator calibration admission/cancel/fault tests passed; no network or RF");
    return 0;
}
