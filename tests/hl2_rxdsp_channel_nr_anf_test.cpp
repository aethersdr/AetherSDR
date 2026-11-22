// HL2 receive-channel NR (WDSP ANR) and automatic notch (WDSP ANF), heard
// through the real Hl2RxDsp, offline and sample-counted (blockForOutput).
//
// Stimulus: a steady carrier 1 kHz above the tuned frequency in wire order,
// plus seeded white noise, AGC off so nothing rescales what a stage removed.
// The carrier's level is read with a Goertzel bin at 1 kHz, the noise as the
// rest of the audio power. ANF must take the carrier down and leave the noise;
// ANR (an LMS predictor) must take the uncorrelated noise down and keep the
// carrier. Off restores each, a rebuild keeps each, and the mode gate holds a
// request through a mode where the stage is out of circuit.

#include "core/backends/hl2/Hl2RxDsp.h"
#include "core/backends/hl2/MetisProtocol.h"   // kEp6BlockSamples

#include <QCoreApplication>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using namespace AetherSDR::hl2;

namespace {

int g_failures = 0;
void check(bool cond, const char* what)
{
    std::fprintf(cond ? stdout : stderr, "%s: %s\n", cond ? "ok" : "FAIL", what);
    if (!cond) {
        ++g_failures;
    }
}

constexpr double kPi = 3.14159265358979323846;
constexpr int kFs = 48000;
constexpr double kToneHz = 1000.0;
constexpr double kToneAmplitude = 0.05;
constexpr double kNoiseSigma = 0.02;
constexpr int kSettleSeconds = 3;     // filter fill plus LMS convergence
constexpr int kMeasureSeconds = 1;

Hl2RxDsp::Config config()
{
    Hl2RxDsp::Config cfg;
    cfg.inputSampleRateHz = kFs;
    cfg.audioSampleRateHz = kFs;
    cfg.dspBlockSize = 1024;
    cfg.fftSize = 256;
    cfg.mode = WdspChannel::Mode::Usb;
    cfg.agcMode = 0;               // AGC off: a fixed gain, nothing claws it back
    cfg.blockForOutput = true;
    return cfg;
}

struct Reading {
    double tone = 0.0;    // rms of the 1 kHz component
    double noise = 0.0;   // rms of everything else
};

struct Rig {
    Hl2RxDsp dsp;
    std::vector<float> left;
    bool collecting = false;
    long clock = 0;
    std::mt19937 rng{12345};
    std::normal_distribution<float> gauss{0.0f, static_cast<float>(kNoiseSigma)};
    bool withTone = true;

    Rig()
    {
        std::string err;
        const bool ok = dsp.configure(config(), &err);
        check(ok, ok ? "Hl2RxDsp configures" : err.c_str());
        QObject::connect(&dsp, &Hl2RxDsp::audioReady, &dsp,
                         [this](const std::vector<float>& pcm) {
            if (!collecting) {
                return;
            }
            for (std::size_t i = 0; i + 1 < pcm.size(); i += 2) {
                left.push_back(pcm[i]);   // interleaved stereo; L == R here
            }
        });
    }

    void feedSeconds(int seconds)
    {
        const std::size_t total = static_cast<std::size_t>(seconds) * kFs;
        for (std::size_t off = 0; off < total; off += kEp6BlockSamples) {
            const std::size_t n = std::min<std::size_t>(kEp6BlockSamples, total - off);
            std::vector<std::complex<float>> blk(n);
            for (std::size_t i = 0; i < n; ++i) {
                const double ph = 2.0 * kPi * kToneHz * static_cast<double>(clock++) / kFs;
                const double a = withTone ? kToneAmplitude : 0.0;
                blk[i] = std::complex<float>(static_cast<float>(a * std::cos(ph)) + gauss(rng),
                                             static_cast<float>(-a * std::sin(ph)) + gauss(rng));
            }
            dsp.processIqBlock(blk);
        }
    }

    Reading read()
    {
        collecting = false;
        feedSeconds(kSettleSeconds);
        left.clear();
        collecting = true;
        feedSeconds(kMeasureSeconds);
        collecting = false;
        Reading r;
        if (left.empty()) {
            return r;
        }
        // Single-bin DFT at the tone over the whole capture (an integer number
        // of cycles at 1 kHz / 48 kHz), so leakage from the noise is one bin's.
        double re = 0.0;
        double im = 0.0;
        double power = 0.0;
        for (std::size_t i = 0; i < left.size(); ++i) {
            const double ph = 2.0 * kPi * kToneHz * static_cast<double>(i) / kFs;
            re += left[i] * std::cos(ph);
            im += left[i] * std::sin(ph);
            power += static_cast<double>(left[i]) * left[i];
        }
        const double n = static_cast<double>(left.size());
        const double toneAmp = 2.0 * std::sqrt(re * re + im * im) / n;
        const double tonePower = toneAmp * toneAmp / 2.0;
        r.tone = std::sqrt(tonePower);
        r.noise = std::sqrt(std::max(0.0, power / n - tonePower));
        return r;
    }
};

double db(double ratio)
{
    return 20.0 * std::log10(std::max(ratio, 1e-12));
}

}  // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // The reference: neither stage requested.
    Reading base;
    {
        Rig rig;
        base = rig.read();
        std::printf("baseline: tone %.5f noise %.5f\n", base.tone, base.noise);
        check(base.tone > 1e-4 && base.noise > 1e-5, "the carrier and the noise both reach the audio");
        check(!rig.dsp.noiseReductionRequested() && !rig.dsp.autoNotchRequested(),
              "a fresh chain runs neither stage");
    }

    // ANF: the carrier goes, the noise stays; off brings the carrier back.
    {
        Rig rig;
        rig.dsp.setAutoNotch(true, Hl2RxDsp::kDefaultAnfLevel);
        check(rig.dsp.autoNotchInCircuit(), "ANF is in circuit in USB");
        const Reading on = rig.read();
        std::printf("ANF on: tone %+.1f dB, noise %+.1f dB\n", db(on.tone / base.tone),
                    db(on.noise / base.noise));
        check(db(on.tone / base.tone) < -15.0, "ANF takes the carrier down by more than 15 dB");
        check(db(on.noise / base.noise) > -6.0, "and leaves the noise within 6 dB");

        rig.dsp.setAutoNotch(false, Hl2RxDsp::kDefaultAnfLevel);
        const Reading off = rig.read();
        std::printf("ANF off again: tone %+.1f dB\n", db(off.tone / base.tone));
        check(std::abs(db(off.tone / base.tone)) < 1.0, "ANF off restores the carrier within 1 dB");
    }

    // ANR: the noise goes down, the carrier survives; off restores the noise.
    // Measured at the default level: noise -6.9 dB, SNR +6.6 dB; the 3 dB bars
    // leave room for WDSP's per-open buffer phase (#5629).
    {
        Rig rig;
        rig.dsp.setNoiseReduction(true, Hl2RxDsp::kDefaultNrLevel);
        check(rig.dsp.noiseReductionInCircuit(), "NR is in circuit in USB");
        const Reading on = rig.read();
        const double snrGain = db((on.tone / on.noise) / (base.tone / base.noise));
        std::printf("NR on: tone %+.1f dB, noise %+.1f dB, SNR %+.1f dB\n",
                    db(on.tone / base.tone), db(on.noise / base.noise), snrGain);
        check(db(on.noise / base.noise) < -3.0, "NR lowers the noise floor by more than 3 dB");
        check(snrGain > 3.0, "and improves the carrier-to-noise ratio by more than 3 dB");

        rig.dsp.setNoiseReduction(false, Hl2RxDsp::kDefaultNrLevel);
        const Reading off = rig.read();
        std::printf("NR off again: noise %+.1f dB\n", db(off.noise / base.noise));
        check(std::abs(db(off.noise / base.noise)) < 1.0, "NR off restores the noise within 1 dB");
    }

    // A rebuild opens a new WDSP channel; both requests have to land in it.
    {
        Rig rig;
        rig.dsp.setAutoNotch(true, Hl2RxDsp::kDefaultAnfLevel);
        rig.dsp.setNoiseReduction(true, Hl2RxDsp::kDefaultNrLevel);
        Hl2RxDsp::Config next = config();
        next.filterHighHz = 2800.0;   // a real change, so the channel is rebuilt
        std::string err;
        const bool ok = rig.dsp.configure(next, &err);
        check(ok, ok ? "reconfigure succeeds" : err.c_str());
        const Reading after = rig.read();
        std::printf("after rebuild: tone %+.1f dB, noise %+.1f dB\n", db(after.tone / base.tone),
                    db(after.noise / base.noise));
        check(db(after.tone / base.tone) < -15.0, "ANF still removes the carrier after a rebuild");
        check(db(after.noise / base.noise) < -3.0, "and NR still lowers the noise");
    }

    // The mode gate: a CW signal IS a steady tone, so ANF is out of circuit in
    // CW and the request waits; back in USB it bites again.
    {
        Rig rig;
        rig.dsp.setAutoNotch(true, Hl2RxDsp::kDefaultAnfLevel);
        rig.dsp.setMode(WdspChannel::Mode::Cwu);
        check(rig.dsp.autoNotchRequested() && !rig.dsp.autoNotchInCircuit(),
              "in CW the ANF request is held, out of circuit");
        const Reading cw = rig.read();
        std::printf("ANF requested in CW: tone %+.1f dB\n", db(cw.tone / base.tone));
        check(db(cw.tone / base.tone) > -3.0, "so the CW tone is not notched");
        rig.dsp.setMode(WdspChannel::Mode::Usb);
        const Reading usb = rig.read();
        check(db(usb.tone / base.tone) < -15.0, "and back in USB the held request notches it");
    }
    check(!Hl2RxDsp::noiseReductionRunsIn(WdspChannel::Mode::Fm)
              && Hl2RxDsp::noiseReductionRunsIn(WdspChannel::Mode::Am)
              && !Hl2RxDsp::autoNotchRunsIn(WdspChannel::Mode::Digu)
              && Hl2RxDsp::autoNotchRunsIn(WdspChannel::Mode::Lsb),
          "NR runs outside FM, ANF in the voice modes only");

    return g_failures == 0 ? 0 : 1;
}
