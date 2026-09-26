// Socket-free C API injection into the production adapter and transaction.
// Removing the R82xx refusal must expose a bias-tee write on a failed request.
#define rtlsdr_STATIC
#define rtlsdr_close test_close
#define rtlsdr_get_tuner_type test_tuner_type
#define rtlsdr_get_direct_sampling test_get_ds
#define rtlsdr_set_direct_sampling test_set_ds
#define rtlsdr_get_offset_tuning test_get_offset
#define rtlsdr_set_offset_tuning test_set_offset
#define rtlsdr_get_freq_correction test_get_ppm
#define rtlsdr_set_freq_correction test_set_ppm
#define rtlsdr_get_center_freq test_get_center
#define rtlsdr_set_center_freq test_set_center
#define rtlsdr_get_sample_rate test_get_rate
#define rtlsdr_set_sample_rate test_set_rate
#define rtlsdr_get_tuner_gain test_get_gain
#define rtlsdr_set_tuner_gain test_set_gain
#define rtlsdr_set_tuner_gain_mode test_gain_mode
#define rtlsdr_set_agc_mode test_agc
#define rtlsdr_reset_buffer test_reset
#define rtlsdr_read_async test_async
#define rtlsdr_cancel_async test_cancel
#include "core/backends/rtl/RtlSdrUsbDevice.h"
#include <cstdio>

using T = AetherSDR::rtl::RtlCaptureTransaction;
struct rtlsdr_dev {
    T::Hardware hardware{100'000'000, 2'400'000, 0, 0, 0, 240};
    rtlsdr_tuner tuner = RTLSDR_TUNER_R820T;
    bool blog = true;
    bool bias = false;
    int offsetWrites = 0;
    int error = 0;
};
int test_close(rtlsdr_dev_t*) { return 0; }
rtlsdr_tuner test_tuner_type(rtlsdr_dev_t* d) { return d->tuner; }
int test_get_ds(rtlsdr_dev_t* d) { return d->hardware.directSampling; }
int test_set_ds(rtlsdr_dev_t* d, int v) { d->hardware.directSampling = v; return 0; }
int test_get_offset(rtlsdr_dev_t* d) { return d->hardware.offsetTuning; }
int test_set_offset(rtlsdr_dev_t* d, int v)
{
    ++d->offsetWrites;
    // Blog aed0ea19 librtlsdr.c:1278-1330: side effect precedes -2;
    // getter reads offs_freq, not bias-tee state. Upstream just returns -2.
    if (d->tuner == RTLSDR_TUNER_R820T || d->tuner == RTLSDR_TUNER_R828D) {
        if (d->blog) { d->bias = v != 0; }
        return -2;
    }
    if (d->hardware.directSampling) { return -3; }
    if (d->error) { return d->error; }
    d->hardware.offsetTuning = v;
    return 0;
}
int test_get_ppm(rtlsdr_dev_t* d) { return d->hardware.ppm; }
int test_set_ppm(rtlsdr_dev_t* d, int v) { d->hardware.ppm = v; return 0; }
uint32_t test_get_center(rtlsdr_dev_t* d) { return d->hardware.centerHz; }
int test_set_center(rtlsdr_dev_t* d, uint32_t v) { d->hardware.centerHz = v; return 0; }
uint32_t test_get_rate(rtlsdr_dev_t* d) { return d->hardware.sampleRateHz; }
int test_set_rate(rtlsdr_dev_t* d, uint32_t v) { d->hardware.sampleRateHz = v; return 0; }
int test_get_gain(rtlsdr_dev_t* d) { return d->hardware.gainTenths; }
int test_set_gain(rtlsdr_dev_t* d, int v) { d->hardware.gainTenths = v; return 0; }
int test_gain_mode(rtlsdr_dev_t*, int) { return 0; }
int test_agc(rtlsdr_dev_t*, int) { return 0; }
int test_reset(rtlsdr_dev_t*) { return 0; }
int test_async(rtlsdr_dev_t*, rtlsdr_read_async_cb_t, void*, uint32_t, uint32_t) { return -1; }
int test_cancel(rtlsdr_dev_t*) { return 0; }
static int failures = 0;
static void check(bool ok, const char* message)
{
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); }
}
int main()
{
    for (const auto tuner : {RTLSDR_TUNER_R820T, RTLSDR_TUNER_R828D}) {
        for (const bool blog : {false, true}) {
            rtlsdr_dev device;
            device.tuner = tuner; device.blog = blog;
            AetherSDR::rtl::RtlSdrUsbDevice usb(&device);
            T owner({8, 1}); owner.beginSession();
            T::Desired desired;
            desired.hardware = device.hardware;
            desired.receivers = {{{0, 100'000'000, -100'000, 100'000, 0, 0, 0}, T::Mode::Wfm}};
            check(bool(owner.submit(desired)), "initial request admitted");
            auto work = owner.takeWork();
            check(work && owner.complete(T::execute(*work, usb)) == T::Completion::Published,
                  "disabled unsupported offset allows initial capture");
            desired.hardware.offsetTuning = 1;
            check(bool(owner.submit(desired)), "offset request reaches adapter");
            work = owner.takeWork();
            check(work && owner.complete(T::execute(*work, usb)) == T::Completion::Failed,
                  "unsupported offset restores accepted state without disconnect");
            check(owner.confirmed() && owner.confirmed()->hardware.offsetTuning == 0,
                  "rejected offset never publishes");
            check(!device.bias, "refused offset and compensation never enable antenna DC");
            check(device.offsetWrites == 0, "unsupported offset never reaches driver setter");
            device.bias = true; // previously enabled externally; no control intent here
            check(usb.set(T::Control::OffsetTuning, 0) && device.bias && device.offsetWrites == 0,
                  "matching disabled offset does not imply or change bias-tee state");
        }
    }
    rtlsdr_dev supported; supported.tuner = RTLSDR_TUNER_E4000;
    AetherSDR::rtl::RtlSdrUsbDevice usb(&supported);
    check(usb.set(T::Control::OffsetTuning, 1) && usb.read()->offsetTuning == 1,
          "supported tuner offset still applies and reads back");
    check(usb.set(T::Control::OffsetTuning, 0) && usb.read()->offsetTuning == 0,
          "supported tuner offset can be disabled");
    supported.error = -1;
    check(!usb.set(T::Control::OffsetTuning, 1), "actual USB error remains a refusal");
    supported.error = 0; supported.hardware.directSampling = 2;
    check(!usb.set(T::Control::OffsetTuning, 1), "direct sampling conflict remains a refusal");
    std::printf("RTL USB controls: %d failures\n", failures);
    return failures ? 1 : 0;
}
