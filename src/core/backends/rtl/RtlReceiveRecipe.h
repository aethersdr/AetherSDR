#pragma once

#include "RtlCaptureTransaction.h"
#include "core/backends/WdspNoiseBlanker.h"
#include "core/dsp/WdspChannel.h"

#include <cmath>
#include <algorithm>
#include <utility>

namespace AetherSDR::rtl {

// The public passband stays relative to the station's carrier. Only the
// prepared extractor/demodulator recipe translates it to the detector's zero.
struct RtlReceiveRecipe {
    using Mode = RtlCaptureTransaction::Mode;
    using Receiver = RtlCaptureTransaction::Receiver;
    static bool sideband(Mode mode) noexcept
    {
        return mode == Mode::Usb || mode == Mode::Lsb || cw(mode);
    }
    static bool cw(Mode mode) noexcept { return mode == Mode::Cw || mode == Mode::Cwr; }
    static bool amplitude(Mode mode) noexcept { return mode == Mode::Am || mode == Mode::Sam; }
    static bool analogDsp(Mode mode) noexcept { return sideband(mode) || amplitude(mode); }
    static bool dcSensitive(Mode mode) noexcept
    { return amplitude(mode) || cw(mode) || mode == Mode::Fm || mode == Mode::Fmn; }
    static bool native(Mode mode, bool wfm = true) noexcept
    {
        return analogDsp(mode) || mode == Mode::Fm || mode == Mode::Fmn
            || (wfm && mode == Mode::Wfm);
    }
    static double bfo(const Receiver& receiver) noexcept
    {
        return receiver.mode == Mode::Cw ? receiver.cwPitchHz
            : receiver.mode == Mode::Cwr ? -receiver.cwPitchHz : 0.0;
    }
    static bool valid(const Receiver& receiver) noexcept
    {
        const auto& band = receiver.passband;
        if ((receiver.agcMode != 0 && receiver.agcMode != 2 && receiver.agcMode != 3 && receiver.agcMode != 4)
            || receiver.agcThreshold < 0 || receiver.agcThreshold > 100
            || receiver.agcOffLevel < 0 || receiver.agcOffLevel > 100
            || receiver.cwPitchHz < 100 || receiver.cwPitchHz > 6000
            || receiver.ritOffsetHz < -99999 || receiver.ritOffsetHz > 99999
            || !isValidNoiseBlankerKind(static_cast<int>(receiver.nbKind))
            || !isValidNoiseBlankerFill(static_cast<int>(receiver.nbFill))
            || receiver.nbLevel < 0 || receiver.nbLevel > 100
            || receiver.apfLevel < 0 || receiver.apfLevel > 100
            || receiver.nrLevel < 0 || receiver.nrLevel > 100
            || receiver.anfLevel < 0 || receiver.anfLevel > 100
            || receiver.mnLevel < 0 || receiver.mnLevel > 100) { return false; }
        if (!analogDsp(receiver.mode)) { return true; }
        if (!std::isfinite(band.filterLowHz) || !std::isfinite(band.filterHighHz)
            || band.filterLowHz < -12000 || band.filterHighHz > 12000
            || band.filterHighHz - band.filterLowHz < 2
            || band.translationHz != (receiver.ritEnabled ? receiver.ritOffsetHz : 0)) { return false; }
        if (receiver.mode == Mode::Usb && band.filterLowHz < 0) { return false; }
        if (receiver.mode == Mode::Lsb && band.filterHighHz > 0) { return false; }
        if (amplitude(receiver.mode)) {
            // Keep a finite carrier region for envelope detection and the SAM
            // PLL; either sideband may be narrowed independently.
            return band.filterLowHz <= -100 && band.filterHighHz >= 100;
        }
        return !cw(receiver.mode) || (band.filterLowHz < 0 && band.filterHighHz > 0);
    }
    static std::pair<int, int> defaultFilter(Mode mode) noexcept
    {
        if (mode == Mode::Usb) { return {100, 2900}; }
        if (mode == Mode::Lsb) { return {-2900, -100}; }
        if (amplitude(mode)) { return {-4000, 4000}; }
        return {-250, 250};
    }
    static SharedCapturePolicy::SliceDescriptor extractionBand(const Receiver& receiver) noexcept
    {
        auto band = receiver.passband;
        const double pitch = bfo(receiver);
        band.translationHz -= pitch;
        band.filterLowHz += pitch;
        band.filterHighHz += pitch;
        return band;
    }
    static void applyDsp(const Receiver& receiver, WdspChannel::Config& dsp,
                         const RtlTrackingNotches& tracking = {})
    {
        dsp.filterLowHz = receiver.passband.filterLowHz + bfo(receiver);
        dsp.filterHighHz = receiver.passband.filterHighHz + bfo(receiver);
        if (amplitude(receiver.mode)) {
            dsp.amReceive = WdspChannel::AmReceive{};
            // Resolve the 50 Hz audio high-pass transition far enough from DC
            // that the demodulated carrier cannot consume output headroom.
            dsp.filterTaps = 4096;
        }
        dsp.agcMode = receiver.agcMode;
        // Same 0..100 host AGC scale as the existing WDSP receiver UI.
        dsp.maximumAgcGainDb = 0.6 * receiver.agcThreshold;
        dsp.agcFixedGainDb = 10.0 + 0.6 * (receiver.agcOffLevel - 10);
        dsp.rxPanelGain = 1.0;
        dsp.noiseBlanker = toWdsp(receiver.nbKind);
        dsp.noiseBlankerLevel = receiver.nbLevel;
        dsp.noiseBlankerFill = toWdsp(receiver.nbFill);
        dsp.apfEnabled = cw(receiver.mode) && receiver.apfEnabled;
        dsp.apfCenterHz = receiver.cwPitchHz;
        dsp.apfBandwidthHz = 200.0 * std::pow(2.0, -receiver.apfLevel / 50.0);
        dsp.apfGain = 2.0;
        const double notch = amplitude(receiver.mode)
            ? 300.0 + receiver.mnLevel / 100.0
                * std::max(0.0, std::max(-dsp.filterLowHz, dsp.filterHighHz) - 500.0)
            : dsp.filterLowHz + receiver.mnLevel / 100.0
                * (dsp.filterHighHz - dsp.filterLowHz);
        dsp.analogReceiveDsp = WdspChannel::AnalogReceiveDsp{
            {}, receiver.nrEnabled, receiver.nrLevel, receiver.anfEnabled, receiver.anfLevel,
            receiver.mnEnabled, notch, 400.0};
        for (const auto& value : tracking.entries) {
            const double center = value.centerHz - receiver.passband.carrierHz
                - receiver.passband.translationHz + bfo(receiver);
            if (center + value.widthHz / 2 < dsp.filterLowHz
                || center - value.widthHz / 2 > dsp.filterHighHz) { continue; }
            dsp.analogReceiveDsp->trackingNotches.push_back(
                {center, value.widthHz, tracking.enabled && value.active});
        }
    }
};

} // namespace AetherSDR::rtl
