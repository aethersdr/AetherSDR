#pragma once

#include "core/backends/rtl/RtlSdrWorker.h"
#include <rtl-sdr.h>

namespace AetherSDR::rtl {

// Private librtlsdr adapter. Kept separate so the C API can be injected in
// socket-free control tests without replacing the transaction implementation.
class RtlSdrUsbDevice final : public RtlSdrWorker::Device {
    using T = RtlCaptureTransaction;
public:
    explicit RtlSdrUsbDevice(rtlsdr_dev_t* device) : m_device(device) {}
    ~RtlSdrUsbDevice() override { if (m_device) { rtlsdr_close(m_device); } }
    bool set(T::Control control, std::int64_t value) override
    {
        if (!m_device) { return false; }
        // Several librtlsdr versions return -2 for unchanged PPM or unsupported
        // offset tuning, even when disabled. Skip only a matching readable
        // value; the transaction still verifies the complete final readback.
        switch (control) {
        case T::Control::DirectSampling:
            return rtlsdr_get_direct_sampling(m_device) == value
                || rtlsdr_set_direct_sampling(m_device, static_cast<int>(value)) == 0;
        case T::Control::SampleRate:
            return rtlsdr_set_sample_rate(m_device, static_cast<std::uint32_t>(value)) == 0;
        case T::Control::Ppm:
            return rtlsdr_get_freq_correction(m_device) == value
                || rtlsdr_set_freq_correction(m_device, static_cast<int>(value)) == 0;
        case T::Control::OffsetTuning: {
            if (rtlsdr_get_offset_tuning(m_device) == value) { return true; }
            const rtlsdr_tuner tuner = rtlsdr_get_tuner_type(m_device);
            // Both upstream and Blog reject offset tuning for R82xx. Blog
            // aed0ea19 additionally toggles bias-tee GPIO *before* returning -2,
            // while its getter still reads tuning offset. Never probe support
            // with a write: rollback cannot observe/restore that antenna DC.
            if (tuner == RTLSDR_TUNER_R820T || tuner == RTLSDR_TUNER_R828D) { return false; }
            return rtlsdr_set_offset_tuning(m_device, static_cast<int>(value)) == 0;
        }
        case T::Control::Center:
            return rtlsdr_set_center_freq(m_device, static_cast<std::uint32_t>(value)) == 0;
        case T::Control::Gain:
            return rtlsdr_set_tuner_gain_mode(m_device, 1) == 0
                && rtlsdr_set_tuner_gain(m_device, static_cast<int>(value)) == 0
                && rtlsdr_set_agc_mode(m_device, 0) == 0;
        }
        return false;
    }
    std::optional<T::Hardware> read() override
    {
        if (!m_device) { return {}; }
        return T::Hardware{rtlsdr_get_center_freq(m_device), rtlsdr_get_sample_rate(m_device),
            rtlsdr_get_direct_sampling(m_device), rtlsdr_get_offset_tuning(m_device),
            rtlsdr_get_freq_correction(m_device), rtlsdr_get_tuner_gain(m_device)};
    }
    bool resetBuffer() override { return m_device && rtlsdr_reset_buffer(m_device) == 0; }
    int readAsync(Callback callback, void* context) override
    {
        return rtlsdr_read_async(m_device, callback, context, 15, 16384);
    }
    void cancelAsync() override { if (m_device) { rtlsdr_cancel_async(m_device); } }
private:
    rtlsdr_dev_t* m_device;
};
} // namespace AetherSDR::rtl
