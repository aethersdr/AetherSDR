// aetherd HL2 — CW BFO geometry.
//
// In CW the marker on the panadapter sits on the SIGNAL, not on the audio the
// signal becomes. Every other client works that way (SmartSDR, Thetis, and
// VfoWidget::applyFilterPreset here, which builds every CW preset as
// {-width/2, +width/2} — "centred on carrier, radio's BFO handles pitch
// offset"), and the HL2 has to as well: its gateware generates a shaped CW
// carrier at the TX NCO, which is the marker, so a receiver listening anywhere
// else is listening away from where the operator transmits.
//
// WDSP gives us no BFO. SetRXAMode(CWU) does not insert one — in this chain the
// NBP edges select the sideband and the detector is direct-conversion for every
// mode — so the pitch has to come from the same place a superhet's does: the
// frequency the detector treats as zero. Hl2Backend therefore splits CW into
// two domains and this test pins the arithmetic that joins them:
//
//   operator-facing   cuts measured from the MARKER, e.g. {-250, +250}
//   demodulator       cuts measured from DC, e.g. {350, 850} at a 600 Hz pitch
//                     with the detector's zero pushed a pitch BELOW the marker
//
// What a mistake here looks like from the operator's seat: the CW passband
// skirt drawn off to one side of the marker (the bug this fixes), or — with the
// shift's sign inverted instead — a passband that draws in the right place and
// receives a full 1200 Hz away from it, on the wrong side.
//
// Offline and synthetic for the reason hl2_shift_test is: hpsdrsim carries a
// strong DC offset on I which a shift translates straight onto the offset
// frequency, where it is indistinguishable from the signal under test.

#include "core/backends/hl2/Hl2RxDsp.h"

#include <QCoreApplication>

#include <cmath>
#include <complex>
#include <cstdio>
#include <vector>

using namespace AetherSDR::hl2;

static int g_failures = 0;
static void check(bool cond, const char* what)
{
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

static constexpr double kPi = 3.14159265358979323846;
static constexpr int kInputRate = 48000;
static constexpr int kAudioRate = 48000;   // audio == input so Hz map 1:1
static constexpr int kBlock = 1024;

// The geometry under test, in one place so a reader can check the numbers
// against Hl2Backend rather than against a comment.
static constexpr double kPitchHz = 600.0;    // TransmitModel's default
static constexpr double kMarkerOffsetHz = 2000.0;   // marker above the NCO
static constexpr double kCutLoHz = -250.0;   // operator-facing 500 Hz filter,
static constexpr double kCutHiHz =  250.0;   // centred on the marker

struct ToneResult {
    double dominantHz = -1.0;
    double rms = 0.0;
};

// Dominant frequency of a real audio buffer, by naive DFT over a coarse grid.
// Small buffers, so clarity beats speed.
static double dominantHz(const std::vector<float>& mono, int rate)
{
    if (mono.size() < 64) return -1.0;
    double bestF = -1.0, bestMag = -1.0;
    for (double f = 100.0; f <= rate / 2.0 - 100.0; f += 25.0) {
        double re = 0.0, im = 0.0;
        const double w = 2.0 * kPi * f / rate;
        for (std::size_t k = 0; k < mono.size(); ++k) {
            re += mono[k] * std::cos(w * static_cast<double>(k));
            im += mono[k] * std::sin(w * static_cast<double>(k));
        }
        const double mag = re * re + im * im;
        if (mag > bestMag) { bestMag = mag; bestF = f; }
    }
    return bestF;
}

// Feed a complex tone and report what came out.
//
// `wireOffsetHz` is in WIRE sign, as in hl2_shift_test: the HPSDR wire is the
// conjugate of the analytic convention, so a POSITIVE value is a tone BELOW the
// NCO. Every call below wants tones ABOVE the NCO and therefore passes negative
// values. Reading this argument as "offset from the NCO" is the exact trap that
// test exists to catch, and it is no less of one here.
static ToneResult runTone(Hl2RxDsp& dsp, double wireOffsetHz, double shiftHz)
{
    dsp.setShift(shiftHz);

    std::vector<float> audio;
    QObject::connect(&dsp, &Hl2RxDsp::audioReady,
                     [&audio](const std::vector<float>& stereo) {
        for (std::size_t k = 0; k + 1 < stereo.size(); k += 2)
            audio.push_back(stereo[k]);            // left channel
    });

    // Discard the first blocks: WDSP's pipeline fills, and a shift change slews.
    constexpr int kWarmBlocks = 24;
    constexpr int kMeasureBlocks = 24;
    double phase = 0.0;
    const double dp = 2.0 * kPi * wireOffsetHz / kInputRate;
    for (int b = 0; b < kWarmBlocks + kMeasureBlocks; ++b) {
        std::vector<std::complex<float>> iq(kBlock);
        for (int k = 0; k < kBlock; ++k) {
            iq[static_cast<std::size_t>(k)] = {
                static_cast<float>(0.25 * std::cos(phase)),
                static_cast<float>(0.25 * std::sin(phase))
            };
            phase += dp;
            if (phase > 2.0 * kPi) phase -= 2.0 * kPi;
        }
        if (b == kWarmBlocks) audio.clear();
        dsp.processIqBlock(iq);
    }
    QObject::disconnect(&dsp, &Hl2RxDsp::audioReady, nullptr, nullptr);

    ToneResult out;
    out.dominantHz = dominantHz(audio, kAudioRate);
    double sum = 0.0;
    for (float s : audio) sum += static_cast<double>(s) * s;
    out.rms = audio.empty() ? 0.0 : std::sqrt(sum / static_cast<double>(audio.size()));
    return out;
}

// Reconfigure for a CW mode and its demodulator-domain passband. The filter has
// to be reapplied after the mode for the reason Hl2Backend documents at its own
// push site: SetRXAMode rebuilds the NBP stage from WDSP's per-mode idea of the
// passband and discards whatever was set before it.
static bool configureCw(Hl2RxDsp& dsp, WdspChannel::Mode mode,
                        double dspLoHz, double dspHiHz)
{
    Hl2RxDsp::Config cfg;
    cfg.inputSampleRateHz = kInputRate;
    cfg.audioSampleRateHz = kAudioRate;
    cfg.dspBlockSize = kBlock;
    cfg.fftSize = 256;
    cfg.mode = mode;
    cfg.filterLowHz = dspLoHz;
    cfg.filterHighHz = dspHiHz;
    cfg.agcMode = 0;                    // AGC off: linear, so nothing rescales
    cfg.maximumAgcGainDb = 40.0;
    cfg.blockForOutput = true;          // deterministic for an offline burst feed
    std::string err;
    const bool ok = dsp.configure(cfg, &err);
    if (!ok) std::fprintf(stderr, "FAIL: configure — %s\n", err.c_str());
    return ok;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    Hl2RxDsp dsp;

    // ---- CWU: the marker must produce the pitch ----------------------------
    //
    // Detector zero a pitch BELOW the marker, passband slid the same way, so
    // the operator's {-250, +250} about the marker becomes {350, 850} of audio.
    const double cwuShift = kMarkerOffsetHz - kPitchHz;
    check(configureCw(dsp, WdspChannel::Mode::Cwu,
                      kCutLoHz + kPitchHz, kCutHiHz + kPitchHz),
          "Hl2RxDsp configures for CWU");
    if (g_failures) return 1;

    const ToneResult onMarker = runTone(dsp, -kMarkerOffsetHz, cwuShift);
    std::fprintf(stderr, "CWU  tone ON the marker -> audio %.0f Hz (expect ~%.0f)\n",
                 onMarker.dominantHz, kPitchHz);
    check(std::fabs(onMarker.dominantHz - kPitchHz) < 120.0,
          "CWU: a signal on the marker comes out at the CW pitch");

    // The edges of the operator's filter, which are what the panadapter draws.
    // 250 Hz either side of the marker must both be inside the passband and
    // must land symmetrically about the pitch — that symmetry IS the centred
    // skirt, measured rather than drawn.
    const ToneResult lowEdge =
        runTone(dsp, -(kMarkerOffsetHz + kCutLoHz + 40.0), cwuShift);
    const ToneResult highEdge =
        runTone(dsp, -(kMarkerOffsetHz + kCutHiHz - 40.0), cwuShift);
    std::fprintf(stderr, "CWU  low edge -> %.0f Hz, high edge -> %.0f Hz\n",
                 lowEdge.dominantHz, highEdge.dominantHz);
    check(std::fabs(lowEdge.dominantHz - (kPitchHz + kCutLoHz + 40.0)) < 120.0,
          "CWU: the filter's low edge is 250 Hz BELOW the marker");
    check(std::fabs(highEdge.dominantHz - (kPitchHz + kCutHiHz - 40.0)) < 120.0,
          "CWU: the filter's high edge is 250 Hz ABOVE the marker");
    check(lowEdge.rms > 0.25 * onMarker.rms && highEdge.rms > 0.25 * onMarker.rms,
          "CWU: both filter edges are inside the passband");

    // And the regression this whole change is about: a signal a full pitch
    // ABOVE the marker — where the old {350, 850}-about-the-marker geometry put
    // the passband — must now be OUT of the filter. Without this the fix could
    // be "the passband is centred AND 600 Hz wider", which draws correctly and
    // still hears the wrong thing.
    const ToneResult pitchAbove =
        runTone(dsp, -(kMarkerOffsetHz + kPitchHz), cwuShift);
    std::fprintf(stderr, "CWU  tone a pitch above the marker -> rms %.5f "
                         "(on-marker rms %.5f)\n", pitchAbove.rms, onMarker.rms);
    check(pitchAbove.rms < 0.1 * onMarker.rms,
          "CWU: a signal a pitch above the marker is rejected");

    // ---- CWL: same pitch, opposite lean ------------------------------------
    //
    // Lower-sideband CW leans the BFO the other way, so the detector's zero
    // goes a pitch ABOVE the marker and the passband becomes negative. The
    // operator's cuts do not change sign — they are still centred on the
    // marker, which is why defaultPassbandForMode returns one entry for both.
    const double cwlShift = kMarkerOffsetHz + kPitchHz;
    check(configureCw(dsp, WdspChannel::Mode::Cwl,
                      kCutLoHz - kPitchHz, kCutHiHz - kPitchHz),
          "Hl2RxDsp configures for CWL");
    const ToneResult cwlOnMarker = runTone(dsp, -kMarkerOffsetHz, cwlShift);
    std::fprintf(stderr, "CWL  tone ON the marker -> audio %.0f Hz (expect ~%.0f)\n",
                 cwlOnMarker.dominantHz, kPitchHz);
    check(std::fabs(cwlOnMarker.dominantHz - kPitchHz) < 120.0,
          "CWL: a signal on the marker comes out at the CW pitch");

    const ToneResult cwlPitchBelow =
        runTone(dsp, -(kMarkerOffsetHz - kPitchHz), cwlShift);
    std::fprintf(stderr, "CWL  tone a pitch below the marker -> rms %.5f "
                         "(on-marker rms %.5f)\n",
                 cwlPitchBelow.rms, cwlOnMarker.rms);
    check(cwlPitchBelow.rms < 0.1 * cwlOnMarker.rms,
          "CWL: a signal a pitch below the marker is rejected");

    // ---- a non-default pitch -----------------------------------------------
    //
    // The whole point of plumbing the pitch to the backend rather than baking
    // 600 into a table: an operator running 700 Hz must still find the signal
    // under the marker. Both the shift and the passband move together.
    constexpr double kAltPitchHz = 700.0;
    const double altShift = kMarkerOffsetHz - kAltPitchHz;
    check(configureCw(dsp, WdspChannel::Mode::Cwu,
                      kCutLoHz + kAltPitchHz, kCutHiHz + kAltPitchHz),
          "Hl2RxDsp configures for CWU at a 700 Hz pitch");
    const ToneResult alt = runTone(dsp, -kMarkerOffsetHz, altShift);
    std::fprintf(stderr, "CWU@700  tone ON the marker -> audio %.0f Hz (expect ~700)\n",
                 alt.dominantHz);
    check(std::fabs(alt.dominantHz - kAltPitchHz) < 120.0,
          "CWU at a 700 Hz pitch: the marker still produces the pitch");

    if (g_failures == 0)
        std::fprintf(stderr, "hl2_cw_bfo_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
