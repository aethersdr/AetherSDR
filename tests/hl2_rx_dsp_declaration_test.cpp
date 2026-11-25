// What receive DSP the HL2 declares, read from the descriptor.
//
// The HL2 has no firmware DSP, so each receive-audio stage either runs in host
// WDSP and is wired, or is declared absent so the UI refuses it rather than
// shows a control that reaches nothing. This pins the declaration for the whole
// family in one place, next to the behavioural tests of the stages that exist
// (hl2_noise_blanker_test, hl2_rxdsp_agc_test, hl2_rxdsp_notch_audio_test):
//
//   present:  the host noise blanker, the AGC threshold control, the CW audio
//             peaking filter (host WDSP, Hl2Backend::setSliceApf), the receive
//             channel's NR (WDSP ANR) and auto notch (WDSP ANF), and the host
//             multi-notch (maxNotchFilters, no depth choice)
//   absent:   radio-side DSP, the LMS noise filters (NRL/ANFL/ANFT), extended
//             DSP (NRS/RNN/NRF), the radio-side single manual notch
//
// receiveDspControl names the same set per mode (AGC, NB, RIT everywhere, APF
// in CW, NR outside FM and DRM, ANF in the voice modes), and so gates both the
// desktop's DSP buttons and aetherd's verbs. hl2_rxdsp_channel_nr_anf_test
// measures NR and ANF on the audio.
//
// Absent is a statement of the current wiring, not a design ruling: when one of
// them is implemented this line should change in the same commit, and the
// behavioural test for it should arrive with it.

#include "TestSettingsProfile.h"
#include "core/backends/hl2/Hl2Backend.h"
#include "core/backends/hl2/Hl2ModeVocabulary.h"
#include "core/backends/hl2/Hl2RxDsp.h"
#include "core/backends/ModelControlRequest.h"
#include "core/backends/RadioCapabilities.h"
#include "core/backends/ReceiveCommand.h"
#include "core/backends/SliceDelta.h"

#include <QCoreApplication>
#include <QObject>
#include <QString>
#include <QStringList>

#include <cstdio>
#include <limits>

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

    using Feature = ReceiveDspControl::Feature;
    const auto& dsp = caps.receiveDspControl;
    check(dsp.has_value(), "receive DSP is declared per feature and mode");
    if (dsp) {
        check(dsp->authority == SliceFrequencyControl::Authority::Engine,
              "with engine authority, so aetherd may steer it");
        check(dsp->features.contains(Feature::Agc) && dsp->features.contains(Feature::Nb)
                  && dsp->features.contains(Feature::Apf) && dsp->features.contains(Feature::Rit)
                  && dsp->features.contains(Feature::Nr) && dsp->features.contains(Feature::Anf)
                  && dsp->features.size() == 6,
              "exactly AGC, the blanker, the APF, RIT, NR and ANF are declared");
        check(!dsp->features.contains(Feature::ManualNotch),
              "the single manual notch is not: nothing runs it here");
        // Every mode setSliceMode() accepts, each once and canonical: a slice in
        // a mode the record lacks would lose its AGC, NB and RIT controls.
        bool everyAccepted = true;
        for (const QString& mode : hl2::knownModeStrings()) {
            everyAccepted &= dsp->modes.contains(hl2::canonicalOfferedMode(mode));
        }
        bool onlyCanonical = true;
        for (const QString& mode : dsp->modes) {
            onlyCanonical &= hl2::isKnownModeString(mode) && hl2::canonicalOfferedMode(mode) == mode
                && dsp->modes.count(mode) == 1;
        }
        check(everyAccepted && onlyCanonical,
              "the DSP modes are every accepted mode, in its canonical spelling, once");
        bool everywhere = true;
        QStringList apfModes;
        QStringList nrModes;
        QStringList anfModes;
        for (const QString& mode : dsp->modes) {
            everywhere &= dsp->supports(Feature::Agc, mode) && dsp->supports(Feature::Nb, mode)
                && dsp->supports(Feature::Rit, mode);
            if (dsp->supports(Feature::Apf, mode)) {
                apfModes.append(mode);
            }
            if (dsp->supports(Feature::Nr, mode)) {
                nrModes.append(mode);
            }
            if (dsp->supports(Feature::Anf, mode)) {
                anfModes.append(mode);
            }
        }
        apfModes.sort();
        anfModes.sort();
        check(everywhere, "AGC, the blanker and RIT apply in every one of them");
        check(apfModes == QStringList({QStringLiteral("CW"), QStringLiteral("CWL")}),
              "the APF applies only where Hl2RxDsp puts it in circuit, CW and CWL");
        check(!nrModes.contains(QStringLiteral("FM")) && !nrModes.contains(QStringLiteral("WBFM"))
                  && !nrModes.contains(QStringLiteral("DRM"))
                  && dsp->modes.contains(QStringLiteral("DRM"))
                  && nrModes.size() == dsp->modes.size() - 3,
              "NR applies in every mode but FM, WBFM and DRM (a digital waveform ANR would corrupt)");
        check(anfModes == QStringList({QStringLiteral("AM"), QStringLiteral("DSB"), QStringLiteral("LSB"),
                                       QStringLiteral("SAM"), QStringLiteral("USB")}),
              "ANF applies only in the voice modes, where a steady tone is interference");
        // The manual notch stays hidden by the legacy flags as before the record.
        check(!caps.hasRadioSideDsp && !caps.hasManualNotch,
              "no radio-side DSP and no manual notch: the radio still runs none");

        // NR and ANF reach the receiver and come back in its echo, per field.
        SliceDelta echo;
        const QMetaObject::Connection echoWatch = QObject::connect(&backend, &IRadioBackend::sliceChanged,
            &backend, [&](int id, const SliceDelta& d) { if (id == 0) { echo = d; } });
        const auto dspRequest = [&](SliceDspRequest::Feature feature, SliceDspRequest::Field field,
                                    bool enabled, int level) {
            SliceDspRequest request;
            request.feature = feature;
            request.field = field;
            request.enabled = enabled;
            request.level = level;
            return backend.requestSliceDsp(0, request);
        };
        using DspFeature = SliceDspRequest::Feature;
        using DspField = SliceDspRequest::Field;
        check(dspRequest(DspFeature::Nr, DspField::Enabled, true, 0) == ReceiveDispatch::Dispatched
                  && echo.nr == true && echo.nrLevel == 50 && !echo.anf && !echo.anfLevel,
              "an NR enable is dispatched and echoed on, at the held level, ANF untouched");
        echo = SliceDelta{};
        backend.setSliceAudioGain(0, 60);
        check(echo.audioGain.has_value() && !echo.nr && !echo.nrLevel && !echo.anf && !echo.anfLevel,
              "an unrelated echo carries no NR/ANF: SliceModel re-emits them for each one");
        check(dspRequest(DspFeature::Nr, DspField::Level, false, 70) == ReceiveDispatch::Dispatched
                  && echo.nr == true && echo.nrLevel == 70 && !echo.anf,
              "an NR level moves the level only");
        check(dspRequest(DspFeature::Anf, DspField::Enabled, true, 0) == ReceiveDispatch::Dispatched
                  && echo.anf == true && echo.anfLevel == 50 && !echo.nr && !echo.nrLevel,
              "an ANF enable is dispatched and echoed on, NR untouched");
        check(dspRequest(DspFeature::Anf, DspField::Level, false, 20) == ReceiveDispatch::Dispatched
                  && echo.anf == true && echo.anfLevel == 20 && !echo.nr,
              "an ANF level moves the level only");
        check(dspRequest(DspFeature::Nr, DspField::Enabled, false, 0) == ReceiveDispatch::Dispatched
                  && echo.nr == false && !echo.anf
                  && dspRequest(DspFeature::Anf, DspField::Enabled, false, 0) == ReceiveDispatch::Dispatched
                  && echo.anf == false && !echo.nr,
              "and both switch off again");
        check(dspRequest(DspFeature::Nrl, DspField::Enabled, true, 0) == ReceiveDispatch::Unsupported,
              "negative control: the Flex LMS NRL is still refused");
        QObject::disconnect(echoWatch);

        // The declared RIT limit is the backend's own clamp, read back.
        SliceDelta last;
        const QMetaObject::Connection watch = QObject::connect(&backend, &IRadioBackend::sliceChanged,
            &backend, [&](int id, const SliceDelta& d) { if (id == 0) { last = d; } });
        const auto rit = [&](int hz) {
            return backend.requestSliceControl(0, SliceControlRequest{SliceControlRequest::Field::Rit,
                                                                      IncrementalTuning{true, hz}});
        };
        check(dsp->maximumRitOffsetHz > 0, "RIT has a headless limit");
        check(rit(dsp->maximumRitOffsetHz + 5000) == ReceiveDispatch::Dispatched
                  && last.ritFreq == dsp->maximumRitOffsetHz,
              "an offset past the limit is applied and published at the limit");
        check(rit(-dsp->maximumRitOffsetHz) == ReceiveDispatch::Dispatched
                  && last.ritFreq == -dsp->maximumRitOffsetHz && last.ritOn == true,
              "and the negative limit is taken as asked");
        QObject::disconnect(watch);
    }

    // panadapter.setRfGain: the LNA range, read back through panRfGainChanged.
    const auto& gain = caps.receivePanRfGainControl;
    check(gain && gain->authority == SliceFrequencyControl::Authority::Engine && gain->stepDb == 1,
          "pan RF gain is declared with engine authority, in 1 dB steps");
    if (gain) {
        QString panId;
        QObject::connect(&backend, &IRadioBackend::sliceChanged, &backend,
                         [&](int id, const SliceDelta& d) { if (id == 0 && d.panId) { panId = *d.panId; } });
        int reported = std::numeric_limits<int>::min();
        QObject::connect(&backend, &IRadioBackend::panRfGainChanged, &backend,
                         [&](const QString&, int db) { reported = db; });
        backend.setSliceAudioGain(0, 40);
        backend.setPanRfGain(panId, gain->maximumDb + 50);
        check(reported == gain->maximumDb, "the declared maximum is what the LNA takes and reports");
        backend.setPanRfGain(panId, gain->minimumDb - 50);
        check(reported == gain->minimumDb, "and so is the declared minimum");
    }

    return failures == 0 ? 0 : 1;
}
