// HL2 receive AGC, measured at the audio the operator hears.
//
// Hl2RxDsp hands the AGC two numbers: a mode and a ceiling (the "top", the most
// gain WDSP may apply; Hl2Backend maps the slice's AGC-T onto it). Existing
// tests pin that the numbers arrive (hl2_rxdsp_async_rebuild_test) and map
// (hl2_dbref_test), and every other fixture switches the AGC OFF so it can
// measure something else. Nothing measured what the stage does to the audio.
// Three properties, all differential on the real Hl2RxDsp, offline and
// sample-counted (blockForOutput, no audio device, no wall clock):
//
//   1. LEVELLING. With the AGC on, a 20 dB step in input level comes out as a
//      much smaller step in audio; with it off the step passes through whole.
//   2. THE CEILING IS A CEILING. A weak tone's audio falls as the ceiling is
//      lowered (a threshold that moved nothing would read as a dead control);
//      a strong tone, which needs little gain, does not move with it.
//   3. OFF IS A FIXED GAIN. With the AGC off the audio follows the input
//      linearly and does not depend on the ceiling, whatever the operator's
//      AGC-T slider says.
//
// The AGC-off LEVEL control (fixed gain per slider unit) is pinned in
// hl2_apf_agc_off_test; this file does not repeat it. The absolute AGC-off
// gain is not pinned here: it is Hl2RxDsp::kDefaultAgcOffLevel mapped by
// agcFixedGainDbForOffLevel (pinned in hl2_apf_agc_off_test), and this file
// measures ratios.

#include "core/backends/hl2/Hl2RxDsp.h"
#include "core/backends/hl2/MetisProtocol.h"   // kEp6BlockSamples

#include <QCoreApplication>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <vector>

using namespace AetherSDR::hl2;

namespace {

int g_failures = 0;
void check(bool cond, const char* what)
{
    std::fprintf(cond ? stdout : stderr, "%s: %s\n", cond ? "ok" : "FAIL", what);
    if (!cond)
        ++g_failures;
}

constexpr double kPi = 3.14159265358979323846;
constexpr int kFs = 48000;
constexpr double kToneHz = 1000.0;
constexpr int kSettleSeconds = 4;     // AGC attack/decay and filter fill
constexpr int kMeasureSeconds = 1;

// A tone 1 kHz above centre IN WIRE ORDER (the HPSDR wire is the conjugate of
// the analytic convention; see Hl2RxDsp::processIqBlock).
std::vector<std::complex<float>> tone(double amp, int seconds, long phaseOffset)
{
    std::vector<std::complex<float>> out(static_cast<std::size_t>(seconds) * kFs);
    for (std::size_t n = 0; n < out.size(); ++n) {
        const double ph = 2.0 * kPi * kToneHz
                          * static_cast<double>(static_cast<long>(n) + phaseOffset) / kFs;
        out[n] = std::complex<float>(static_cast<float>(amp * std::cos(ph)),
                                     static_cast<float>(-amp * std::sin(ph)));
    }
    return out;
}

void feed(Hl2RxDsp& dsp, const std::vector<std::complex<float>>& s)
{
    for (std::size_t off = 0; off < s.size(); off += kEp6BlockSamples) {
        const std::size_t n = std::min<std::size_t>(kEp6BlockSamples, s.size() - off);
        dsp.processIqBlock(std::vector<std::complex<float>>(
            s.begin() + static_cast<std::ptrdiff_t>(off),
            s.begin() + static_cast<std::ptrdiff_t>(off + n)));
    }
}

struct Rig {
    Hl2RxDsp dsp;
    double sumSquares = 0.0;
    std::size_t count = 0;
    bool collecting = false;
    long clock = 0;

    explicit Rig(int agcMode, double ceilingDb)
    {
        Hl2RxDsp::Config cfg;
        cfg.inputSampleRateHz = kFs;
        cfg.audioSampleRateHz = kFs;
        cfg.dspBlockSize = 1024;
        cfg.fftSize = 256;
        cfg.mode = WdspChannel::Mode::Usb;
        cfg.agcMode = agcMode;
        cfg.maximumAgcGainDb = ceilingDb;
        cfg.blockForOutput = true;
        std::string err;
        const bool ok = dsp.configure(cfg, &err);
        check(ok, ok ? "Hl2RxDsp configures" : err.c_str());
        QObject::connect(&dsp, &Hl2RxDsp::audioReady, &dsp,
                         [this](const std::vector<float>& pcm) {
            if (!collecting)
                return;
            for (float s : pcm) {
                sumSquares += static_cast<double>(s) * s;
                ++count;
            }
        });
    }

    // Settle on `amp`, then return the audio level in dB over one second.
    double levelDb(double amp)
    {
        collecting = false;
        auto settle = tone(amp, kSettleSeconds, clock);
        clock += static_cast<long>(settle.size());
        feed(dsp, settle);
        sumSquares = 0.0;
        count = 0;
        collecting = true;
        auto meas = tone(amp, kMeasureSeconds, clock);
        clock += static_cast<long>(meas.size());
        feed(dsp, meas);
        collecting = false;
        if (count == 0 || sumSquares <= 0.0)
            return -300.0;
        return 10.0 * std::log10(sumSquares / static_cast<double>(count));
    }
};

}  // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    constexpr double kWeak = 0.0005;     // -66 dBFS: needs most of the gain
    constexpr double kMid = 0.005;       // 20 dB above kWeak
    constexpr double kStrong = 0.3;      // needs little gain

    // ---- 1. levelling -------------------------------------------------
    {
        Rig on1(3, 60.0), on2(3, 60.0), off1(0, 60.0), off2(0, 60.0);
        const double onStep = on2.levelDb(kMid) - on1.levelDb(kWeak);
        const double offStep = off2.levelDb(kMid) - off1.levelDb(kWeak);
        std::printf("levelling: 20 dB input step -> AGC medium %.1f dB, AGC off %.1f dB\n",
                    onStep, offStep);
        check(offStep > 18.0 && offStep < 22.0,
              "AGC off: a 20 dB input step is a 20 dB audio step");
        check(onStep < 12.0,
              "AGC medium: the same 20 dB step is compressed to well under 12 dB");
        check(onStep < offStep - 8.0,
              "the AGC takes at least 8 dB out of the step that AGC off passes");
    }

    // ---- 2. the ceiling ----------------------------------------------
    {
        // One instance, the ceiling moved at run time through setAgc(): that is
        // the path the slice's AGC-T takes, and it must not need a rebuild.
        Rig rig(3, 60.0);
        const double weak60 = rig.levelDb(kWeak);
        rig.dsp.setAgc(3, 20.0);
        const double weak20 = rig.levelDb(kWeak);
        rig.dsp.setAgc(3, 0.0);
        const double weak0 = rig.levelDb(kWeak);
        std::printf("ceiling, weak tone: 60 dB -> %.1f, 20 dB -> %.1f, 0 dB -> %.1f dBFS\n",
                    weak60, weak20, weak0);
        check(weak20 < weak60 - 10.0,
              "lowering the ceiling from 60 to 20 dB lowers a weak tone's audio by over 10 dB");
        check(weak0 < weak20 - 10.0,
              "lowering it further to 0 dB lowers it by over 10 dB more");

        rig.dsp.setAgc(3, 60.0);
        const double restored = rig.levelDb(kWeak);
        check(std::fabs(restored - weak60) < 3.0,
              "raising the ceiling back restores the level (the setter works both ways)");

        // A strong tone needs far less than 20 dB of gain, so the ceiling is
        // not what sets its level.
        Rig a(3, 60.0), b(3, 30.0);
        const double strong60 = a.levelDb(kStrong);
        const double strong30 = b.levelDb(kStrong);
        std::printf("ceiling, strong tone: 60 dB -> %.1f, 30 dB -> %.1f dBFS\n",
                    strong60, strong30);
        check(std::fabs(strong60 - strong30) < 3.0,
              "a strong tone sits at the same level under a 60 dB and a 30 dB ceiling");
    }

    // ---- 3. off is a fixed gain ---------------------------------------
    {
        Rig rig(0, 0.0);
        const double at0 = rig.levelDb(kMid);
        rig.dsp.setAgc(0, 60.0);
        const double at60 = rig.levelDb(kMid);
        std::printf("AGC off: ceiling 0 dB -> %.1f, 60 dB -> %.1f dBFS\n", at0, at60);
        check(at0 > -300.0, "AGC off produces audio");
        check(std::fabs(at0 - at60) < 0.5,
              "AGC off: the ceiling does not change the level");
    }

    if (g_failures == 0)
        std::fprintf(stdout, "hl2_rxdsp_agc_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
