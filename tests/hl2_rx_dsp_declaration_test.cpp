// What receive DSP the HL2 declares, read from the descriptor.
//
// The HL2 has no firmware DSP, so each receive-audio stage either runs in host
// WDSP and is wired, or is declared absent so the UI refuses it rather than
// shows a control that reaches nothing. This pins the declaration for the whole
// family in one place, next to the behavioural tests of the stages that exist
// (hl2_noise_blanker_test, hl2_rxdsp_agc_test, hl2_rxdsp_notch_audio_test):
//
//   present:  the host noise blanker, the AGC threshold control, the CW audio
//             peaking filter (host WDSP, Hl2Backend::setSliceApf), and the
//             host multi-notch (maxNotchFilters, no depth choice)
//   absent:   radio-side DSP, the LMS noise filters (NRL/ANFL/ANFT), extended
//             DSP (NRS/RNN/NRF), the radio-side single manual notch
//
// Absent is a statement of the current wiring, not a design ruling: when one of
// them is implemented this line should change in the same commit, and the
// behavioural test for it should arrive with it.

#include "TestSettingsProfile.h"
#include "core/backends/hl2/Hl2Backend.h"
#include "core/backends/hl2/Hl2RxDsp.h"

#include <QCoreApplication>

#include <cstdio>

using namespace AetherSDR;

namespace {
int failures = 0;
void check(bool condition, const char* label)
{
    std::printf("%s %s\n", condition ? "[ OK ]" : "[FAIL]", label);
    if (!condition)
        ++failures;
}
}  // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile settingsProfile(QStringLiteral("hl2-rx-dsp-declaration-test"));
    if (!settingsProfile.isValid()) {
        std::fprintf(stderr, "FAIL: could not create an isolated settings profile\n");
        return 1;
    }
    QCoreApplication app(argc, argv);

    hl2::Hl2Backend backend;
    const RadioCapabilities caps = backend.capabilities();

    check(caps.hasHostNoiseBlanker, "the impulse noise blanker runs on this host");
    check(caps.hasAgcThreshold, "the AGC threshold control is declared");
    check(!caps.hasRadioSideDsp, "no radio-side DSP");
    check(!caps.hasLmsNoiseFilters, "no NRL/ANFL/ANFT");
    check(!caps.hasExtendedDsp, "no extended NRS/RNN/NRF");
    check(caps.hasAudioPeakingFilter, "the CW peaking filter runs on this host");
    check(!caps.hasManualNotch, "no radio-side single manual notch");
    check(caps.maxNotchFilters > 0, "the host WDSP notch database is declared");
    check(!caps.notchHasDepth, "a WDSP notch is a full null, so no depth choice");
    check(caps.notchMinWidthHz == hl2::Hl2RxDsp::kMinNotchWidthHz,
          "the notch minimum width is the RX filter's");

    return failures == 0 ? 0 : 1;
}
