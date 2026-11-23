// HL2 receive audio is mono at the WDSP output panel: L == R in every mode,
// heard through the real Hl2RxDsp, offline and sample-counted (blockForOutput).
//
// WDSP's create_rxa() leaves the panel at copy 0 (L = I, R = Q): a 90 degree
// pair in SSB/CW/data, and in AM/SAM with ANR or ANF a silent right channel
// (anr.c/anf.c zero Q). Hl2RxDsp opens with binaural off, like Thetis and
// piHPSDR. Checks per mode: R equals L, the mean (L+R)/2 equals L; with ANR or
// ANF in AM/SAM, R is not silent; L's level is the one copy 0 gave (L = I
// either way); the setting holds across a mode change and a rebuild.

#include "core/backends/hl2/Hl2RxDsp.h"
#include "core/backends/hl2/MetisProtocol.h"   // kEp6BlockSamples

#include <QCoreApplication>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <string>
#include <vector>

using namespace AetherSDR::hl2;
using Mode = WdspChannel::Mode;

namespace {

int g_failures = 0;
void check(bool cond, const std::string& what)
{
    std::fprintf(cond ? stdout : stderr, "%s: %s\n", cond ? "ok" : "FAIL", what.c_str());
    if (!cond) {
        ++g_failures;
    }
}

constexpr double kPi = 3.14159265358979323846;
constexpr double kAmplitude = 0.02;   // -34 dBFS
constexpr double kSettleSeconds = 3.0;
constexpr double kMeasureSeconds = 1.0;

// Hl2Backend.cpp defaultPassbandForMode / followModeAgc / cwBfoOffsetHz.
struct ModeRow {
    const char* name;
    Mode mode;
    double lo, hi;
    int agc;
    double bfo;
    double toneHz;
};
const ModeRow kModes[] = {
    {"USB", Mode::Usb, 100, 2900, 3, 0, 1000},    {"LSB", Mode::Lsb, -2900, -100, 3, 0, 1000},
    {"CW", Mode::Cwu, -250, 250, 3, 600, 600},    {"CWL", Mode::Cwl, -250, 250, 3, -600, 600},
    {"DIGU", Mode::Digu, 150, 3000, 0, 0, 1000},  {"DIGL", Mode::Digl, -3000, -150, 0, 0, 1000},
    {"AM", Mode::Am, -4000, 4000, 3, 0, 1000},    {"SAM", Mode::Sam, -4000, 4000, 3, 0, 1000},
    {"FM", Mode::Fm, -8000, 8000, 3, 0, 1000},    {"DSB", Mode::Dsb, -3000, 3000, 3, 0, 1000}};

const ModeRow& row(Mode mode)
{
    for (const auto& m : kModes) {
        if (m.mode == mode) {
            return m;
        }
    }
    return kModes[0];
}

Hl2RxDsp::Config config(const ModeRow& m, int inputRate = 48000)
{
    Hl2RxDsp::Config cfg;
    cfg.inputSampleRateHz = inputRate;
    cfg.audioSampleRateHz = 24000;
    cfg.mode = m.mode;
    cfg.filterLowHz = m.lo + m.bfo;
    cfg.filterHighHz = m.hi + m.bfo;
    cfg.agcMode = m.agc;
    cfg.maximumAgcGainDb = 39.0;
    cfg.blockForOutput = true;
    return cfg;
}

struct Reading {
    double l = 0, r = 0, mean = 0;   // rms
    double corr = 0;                 // sum(LR) / sqrt(sum L^2 sum R^2)
    double maxDiff = 0;              // max |L - R|
};

double db(double ratio) { return 20.0 * std::log10(std::max(ratio, 1e-12)); }

struct Rig {
    Hl2RxDsp dsp;
    ModeRow m;
    int rate;
    long n = 0;
    double fmPhase = 0;
    bool collecting = false;
    std::vector<float> pcm;

    explicit Rig(const ModeRow& row, int inputRate = 48000) : m(row), rate(inputRate)
    {
        std::string err;
        const bool ok = dsp.configure(config(m, rate), &err);
        check(ok, ok ? std::string(m.name) + ": Hl2RxDsp configures" : err);
        dsp.setShift(-m.bfo);
        QObject::connect(&dsp, &Hl2RxDsp::audioReady, &dsp,
                         [this](const std::vector<float>& p) {
            if (collecting) {
                pcm.insert(pcm.end(), p.begin(), p.end());
            }
        });
    }

    // The detector's audio in the mode's own terms, then HPSDR wire handedness.
    std::complex<double> sample()
    {
        const double t = static_cast<double>(n++) / rate;
        const double a = std::cos(2 * kPi * m.toneHz * t);
        std::complex<double> bb;
        switch (m.mode) {
        case Mode::Am: case Mode::Sam: bb = kAmplitude * (1.0 + 0.5 * a); break;
        case Mode::Fm:
            fmPhase += 2 * kPi * 2500 * a / rate;
            bb = std::polar(kAmplitude, fmPhase);
            break;
        case Mode::Dsb: bb = kAmplitude * a; break;   // suppressed carrier at phase 0
        default: {
            bb = std::polar(kAmplitude, 2 * kPi * m.toneHz * t);
            if (m.hi + m.bfo <= 0) {
                bb = std::conj(bb);   // lower sideband
            }
            bb *= std::polar(1.0, -2 * kPi * m.bfo * t);
        }
        }
        return std::conj(bb);
    }

    void feed(double seconds)
    {
        const long total = static_cast<long>(seconds * rate);
        for (long off = 0; off < total; off += static_cast<long>(kEp6BlockSamples)) {
            std::vector<std::complex<float>> blk(static_cast<std::size_t>(
                std::min<long>(static_cast<long>(kEp6BlockSamples), total - off)));
            for (auto& s : blk) {
                const auto v = sample();
                s = {static_cast<float>(v.real()), static_cast<float>(v.imag())};
            }
            dsp.processIqBlock(blk);
        }
    }

    Reading read()
    {
        collecting = false;
        feed(kSettleSeconds);
        pcm.clear();
        collecting = true;
        feed(kMeasureSeconds);
        collecting = false;
        Reading r;
        double ll = 0, rr = 0, lr = 0, mm = 0;
        std::size_t frames = 0;
        for (std::size_t i = 0; i + 1 < pcm.size(); i += 2, ++frames) {
            const double l = pcm[i];
            const double q = pcm[i + 1];
            ll += l * l;
            rr += q * q;
            lr += l * q;
            mm += 0.25 * (l + q) * (l + q);
            r.maxDiff = std::max(r.maxDiff, std::abs(l - q));
        }
        if (frames == 0) {
            return r;
        }
        r.l = std::sqrt(ll / frames);
        r.r = std::sqrt(rr / frames);
        r.mean = std::sqrt(mm / frames);
        r.corr = lr / std::sqrt(std::max(ll * rr, 1e-30));
        return r;
    }
};

void print(const std::string& label, const Reading& r)
{
    std::printf("%-22s L %.5f  R-L %+8.3f dB  mean-L %+8.3f dB  corr %+.4f  max|L-R| %.2g\n",
                label.c_str(), r.l, db(r.r / std::max(r.l, 1e-12)),
                db(r.mean / std::max(r.l, 1e-12)), r.corr, r.maxDiff);
}

void checkMono(const std::string& label, const Reading& r)
{
    print(label, r);
    check(r.l > 1e-3, label + ": the signal reaches L");
    check(r.maxDiff <= 1e-5 * r.l && r.corr > 0.9999,
          label + ": R equals L sample for sample (corr > 0.9999)");
    check(std::abs(db(r.r / r.l)) < 0.01, label + ": R level equals L within 0.01 dB");
    check(std::abs(db(r.mean / r.l)) < 0.01, label + ": the mean (L+R)/2 equals L within 0.01 dB");
}

}  // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    for (const auto& m : kModes) {
        Rig rig(m);
        const auto* wc = rig.dsp.channelConfig();
        check(wc && wc->rxPanelMono, std::string(m.name) + ": the channel opens with the panel mono");
        checkMono(m.name, rig.read());
    }

    // ANR and ANF zero Q in AM/SAM; the panel copies I after them.
    for (Mode mode : {Mode::Am, Mode::Sam}) {
        for (int stage = 0; stage < 2; ++stage) {
            Rig rig(row(mode));
            if (stage == 0) {
                rig.dsp.setNoiseReduction(true, Hl2RxDsp::kDefaultNrLevel);
            } else {
                rig.dsp.setAutoNotch(true, Hl2RxDsp::kDefaultAnfLevel);
            }
            const std::string label = std::string(row(mode).name) + (stage == 0 ? "+ANR" : "+ANF");
            check(stage == 0 ? rig.dsp.noiseReductionInCircuit() : rig.dsp.autoNotchInCircuit(),
                  label + ": the stage is in circuit");
            const Reading r = rig.read();
            checkMono(label, r);
            check(r.r > 0.5 * r.l, label + ": R is not silent");
        }
    }

    // Positive control and level check: copy 0 restored on a live USB channel
    // brings back the 90 degree pair, and L (= I either way) keeps its level.
    {
        Rig mono(row(Mode::Usb));
        const Reading a = mono.read();
        Rig pair(row(Mode::Usb));
        // The control case only: puts the panel back to create_rxa()'s copy 0.
        WdspChannel::setRxPanelBinauralForTest(pair.dsp.wdspChannelId(), 1);
        const Reading b = pair.read();
        print("USB copy 0 (control)", b);
        check(std::abs(b.corr) < 0.05 && std::abs(db(b.mean / b.l) + 3.01) < 0.05,
              "control: copy 0 is a 90 degree pair, mean 3.01 dB below L");
        std::printf("USB L mono %.6f vs copy 0 %.6f (%+.4f dB)\n", a.l, b.l, db(a.l / b.l));
        check(std::abs(db(a.l / b.l)) < 0.05, "L's level is unchanged by the mono panel (0.05 dB)");
    }

    // A live mode change keeps the panel (SetRXAMode does not touch it).
    {
        Rig rig(row(Mode::Usb));
        rig.dsp.setNoiseReduction(true, Hl2RxDsp::kDefaultNrLevel);
        rig.read();
        rig.m = row(Mode::Am);
        rig.dsp.setShift(0);
        rig.dsp.setMode(Mode::Am);
        rig.dsp.setFilter(rig.m.lo, rig.m.hi);
        checkMono("USB -> AM+ANR (live)", rig.read());
    }

    // A rebuild opens a new WDSP channel: a passband rebuild and a rate change.
    {
        Rig rig(row(Mode::Am));
        rig.dsp.setAutoNotch(true, Hl2RxDsp::kDefaultAnfLevel);
        Hl2RxDsp::Config next = config(rig.m);
        next.filterHighHz = 3500.0;
        next.filterLowHz = -3500.0;
        std::string err;
        bool ok = rig.dsp.configure(next, &err);
        check(ok, ok ? "passband rebuild succeeds" : err);
        checkMono("AM+ANF after rebuild", rig.read());

        next.inputSampleRateHz = 192000;
        ok = rig.dsp.configure(next, &err);
        check(ok, ok ? "192 kHz rebuild succeeds" : err);
        rig.rate = 192000;
        rig.n = 0;
        const auto* wc = rig.dsp.channelConfig();
        check(wc && wc->inputSampleRate == 192000 && wc->rxPanelMono,
              "the 192 kHz channel opens with the panel mono");
        checkMono("AM+ANF at 192 kHz", rig.read());
    }

    // The backend's asynchronous rate change: buildChannel off-thread, then install.
    {
        Rig rig(row(Mode::Sam));
        rig.dsp.setNoiseReduction(true, Hl2RxDsp::kDefaultNrLevel);
        Hl2RxDsp::Config next = config(rig.m, 96000);
        rig.dsp.beginRebuild(next);
        auto built = Hl2RxDsp::buildChannel(next, WdspChannel::NoiseBlanker::Off, 50,
                                            WdspChannel::NoiseBlankerFill::Zero);
        check(rig.dsp.installRebuiltChannel(std::move(built)), "asynchronous rebuild installs");
        rig.rate = 96000;
        rig.n = 0;
        checkMono("SAM+ANR after async rebuild", rig.read());
    }

    return g_failures == 0 ? 0 : 1;
}
