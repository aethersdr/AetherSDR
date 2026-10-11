// What an HL2 operator's transmitted audio gets from the TXA modulator (#5911):
// the passband (a multitone through the real chain, each tone's wanted bin
// against the 1 kHz one) and the onset (first audio to first IQ, at key-up and
// mid-over). The edges are written out here, not read back from the mapping,
// so a narrower shipped passband fails. Sample-counted on the IQ stream.

#include "core/backends/hl2/Hl2TxDsp.h"
#include "core/backends/hl2/Hl2TxLevelPolicy.h"
#include "TxTestAuthority.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QObject>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdio>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace AetherSDR;
using namespace AetherSDR::hl2;

namespace {

int g_failures = 0;
void check(bool ok, const char* what)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++g_failures; }
    else     { std::fprintf(stderr, "ok:   %s\n", what); }
}

constexpr double kPi = 3.14159265358979323846;
constexpr double kFsIn = 24000.0;
constexpr double kFsOut = 48000.0;
constexpr double kMsPerOut = 1000.0 / kFsOut;

// Every probe tone is a multiple of 50 Hz, so a window of whole 960-sample
// periods at 48 kHz makes all their bins (and their images) orthogonal.
constexpr std::size_t kPeriod = 960;
// Output dropped before a level is read: the key-up mute ramp and fill.
constexpr std::size_t kSettle = 12288;

// Run the event loop -- not sleep -- for `ms`, as the live I/O thread does.
void pumpFor(double ms)
{
    QElapsedTimer t;
    t.start();
    while (t.nsecsElapsed() < static_cast<qint64>(ms * 1e6)) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
}

struct Capture {
    std::vector<std::complex<float>> iq;
    unsigned long long faults = 0;
    unsigned long long stallDrops = 0;   // the part of faults that is starvation
    bool configured = false;
};

// One fresh over: `audio` at 24 kHz, fed in 10 ms deliveries at real time,
// ALC off so the chain is linear, mic gain unity.
Capture runOnce(const std::vector<float>& audio, WdspChannel::Mode mode,
                std::pair<int, int> passband)
{
    TxTestAuthority authority;
    Capture c;   // outlives tx: the iqReady slot writes into it
    Hl2TxDsp tx;
    Hl2TxDsp::Config cfg;
    cfg.mode = mode;
    cfg.filterLowHz = passband.first;
    cfg.filterHighHz = passband.second;
    cfg.alcEnabled = false;
    std::string err;
    if (!tx.configure(cfg, &err)) {
        check(false, ("the modulator configures: " + err).c_str());
        return c;
    }
    c.configured = true;
    QObject::connect(&tx, &Hl2TxDsp::iqReady, &tx,
                     [&](const std::vector<std::complex<float>>& iq,
                         const TxCoordinator::Context&) {
        c.iq.insert(c.iq.end(), iq.begin(), iq.end());
    });
    constexpr std::size_t kChunk = 240;
    for (std::size_t off = 0; off < audio.size(); off += kChunk) {
        const std::size_t n = std::min(kChunk, audio.size() - off);
        const auto first = audio.begin() + static_cast<std::ptrdiff_t>(off);
        tx.processAudioBlock(std::vector<float>(first, first + static_cast<std::ptrdiff_t>(n)),
                             TxAudioSource::Microphone, authority.context);
        pumpFor(1000.0 * static_cast<double>(n) / kFsIn);
    }
    pumpFor(50.0);
    c.faults = tx.modulatorFaultBlocks();
    c.stallDrops = tx.modulatorStallDrops();
    return c;
}

// Retried when the worker was starved (a stall drop): that capture is not
// contiguous. Still starved after three tries is machine load, reported as
// inconclusive. A configure failure or any other fault is the exchange gate
// failing (#5910): a failed check, never retried.
bool g_starved = false;
Capture run(const char* what, const std::vector<float>& audio, WdspChannel::Mode mode,
            std::pair<int, int> passband)
{
    Capture c;
    for (int attempt = 0; attempt < 3; ++attempt) {
        c = runOnce(audio, mode, passband);
        if (!c.configured || c.faults == 0) {
            return c;
        }
        if (c.faults > c.stallDrops) {
            check(false, (std::string(what) + ": no fault but a stall drop").c_str());
            return c;
        }
        std::fprintf(stderr, "starved in %s: %llu blocks missed the wire (attempt %d of 3)\n",
                     what, c.faults, attempt + 1);
    }
    g_starved = true;
    return c;
}

double bin(const std::vector<std::complex<float>>& iq, std::size_t from,
           std::size_t count, double hz)
{
    std::complex<double> acc{0.0, 0.0};
    const double w = -2.0 * kPi * hz / kFsOut;
    for (std::size_t n = 0; n < count; ++n) {
        const double ph = w * static_cast<double>(n);
        acc += std::complex<double>(iq[from + n].real(), iq[from + n].imag())
             * std::complex<double>(std::cos(ph), std::sin(ph));
    }
    return std::abs(acc) / static_cast<double>(count);
}

double db(double ratio) { return 20.0 * std::log10(std::max(ratio, 1e-30)); }

const std::vector<int> kTones = {50, 100, 150, 200, 250, 300, 350, 400, 500, 700, 1000, 1300,
                                 1700, 2000, 2300, 2500, 2600, 2650, 2700, 2750, 2800,
                                 2900, 3000, 3100, 3500};
constexpr double kToneAmp = 0.015;

// Response of each tone's wanted bin relative to 1 kHz, in dB, in kTones order.
// Schroeder phases keep the multitone's crest factor low.
std::vector<double> passbandResponse(const char* what, WdspChannel::Mode mode,
                                     std::pair<int, int> passband, bool lower)
{
    const std::size_t total = static_cast<std::size_t>(0.75 * kFsIn);
    std::vector<float> audio(total, 0.0f);
    const double k = static_cast<double>(kTones.size());
    for (std::size_t t = 0; t < kTones.size(); ++t) {
        const double i = static_cast<double>(t);
        const double phi = -kPi * i * (i - 1.0) / k;
        for (std::size_t n = 0; n < total; ++n) {
            audio[n] += static_cast<float>(
                kToneAmp * std::sin(2.0 * kPi * kTones[t] * n / kFsIn + phi));
        }
    }
    const Capture c = run(what, audio, mode, passband);
    std::vector<double> resp(kTones.size(), -400.0);
    if (c.iq.size() < kSettle + 8 * kPeriod) {
        check(false, "the passband capture is long enough to read");
        return resp;
    }
    const std::size_t count = ((c.iq.size() - kSettle) / kPeriod) * kPeriod;
    const double sign = lower ? 1.0 : -1.0;   // wire handedness, see hl2_txdsp_test
    const double ref = bin(c.iq, kSettle, count, sign * 1000.0);
    std::fprintf(stderr, "%s, %d..%d Hz: 1 kHz gain %.3f dB (%zu samples)\n", what,
                 passband.first, passband.second, db(ref / kToneAmp), count);
    for (std::size_t t = 0; t < kTones.size(); ++t) {
        resp[t] = db(bin(c.iq, kSettle, count, sign * kTones[t]) / ref);
        std::fprintf(stderr, "  %5d Hz  %9.3f dB\n", kTones[t], resp[t]);
    }
    return resp;
}

// The shipped shape: flat between the edges, -6 dB at each edge (WDSP's
// windowed-sinc design), 50 dB down 100 Hz outside and floor-deep beyond that.
constexpr double kFlatDb = 0.1;
constexpr double kEdgeMinDb = -7.0;
constexpr double kEdgeMaxDb = -5.0;
constexpr double kStop100Db = 50.0;
constexpr double kStop150Db = 100.0;

void checkShape(const char* what, const std::vector<double>& resp, int lo, int hi)
{
    double worstFlat = 0.0;
    double worstStop100 = -400.0;
    double worstStop150 = -400.0;
    int edges = 0;
    int stop100 = 0;
    bool edgesOk = true;
    for (std::size_t t = 0; t < kTones.size(); ++t) {
        const int f = kTones[t];
        if (f >= lo + 100 && f <= hi - 100) {
            worstFlat = std::max(worstFlat, std::abs(resp[t]));
        }
        if (f == lo - 100 || f == hi + 100) {
            ++stop100;
            worstStop100 = std::max(worstStop100, resp[t]);
        }
        if (f <= lo - 150 || f >= hi + 150) {
            worstStop150 = std::max(worstStop150, resp[t]);
        }
        if (f == lo || f == hi) {
            ++edges;
            edgesOk = edgesOk && resp[t] >= kEdgeMinDb && resp[t] <= kEdgeMaxDb;
        }
    }
    std::fprintf(stderr, "%s: worst in-band %.3f dB; 100 Hz outside %.2f dB; "
                 "150+ Hz outside %.1f dB\n", what, worstFlat, worstStop100, worstStop150);
    const std::string s(what);
    check(edges == 2, (s + ": both edges are probed").c_str());
    check(worstFlat <= kFlatDb, (s + ": flat within 0.1 dB from edge+100 to edge-100 Hz").c_str());
    check(edgesOk, (s + ": each edge tone is -6 +/- 1 dB").c_str());
    check(stop100 == 2, (s + ": both edges are probed 100 Hz outside").c_str());
    check(worstStop100 <= -kStop100Db, (s + ": 100 Hz outside an edge is 50 dB down").c_str());
    check(worstStop150 <= -kStop150Db, (s + ": 150 Hz and more outside is 100 dB down").c_str());
}

struct Onset {
    double firstMs = 0.0;    // |iq| first at -60 dB of the settled level
    double halfMs = 0.0;     // first at -6 dB: the onset's 50 % point
    double rise1090Ms = 0.0;
    double leadMs = 0.0;     // from -60 dB to the 50 % point
};

// A 1 kHz tone onset at input sample `startIn`. Times are on the IQ stream's
// own clock (output sample n is n / 48 kHz), from the onset's first audio
// sample. `warm` puts a 200 ms burst and 300 ms of silence before it, so the
// channel's mute envelope has already run (WDSP starts it at the over's
// first non-zero input sample) and the onset is the chain's own.
Onset onset(const char* what, bool warm)
{
    const std::size_t startIn = warm ? static_cast<std::size_t>(0.5 * kFsIn) : 0;
    const std::size_t total = startIn + static_cast<std::size_t>(0.4 * kFsIn);
    const std::size_t burstEnd = static_cast<std::size_t>(0.2 * kFsIn);
    std::vector<float> audio(total, 0.0f);
    for (std::size_t n = 0; n < total; ++n) {
        if (n >= startIn || (warm && n < burstEnd)) {
            audio[n] = static_cast<float>(0.1 * std::sin(2.0 * kPi * 1000.0 * n / kFsIn));
        }
    }
    const Capture c = run(what, audio, WdspChannel::Mode::Usb, defaultTxPassbandForModeName("USB"));
    Onset o;
    const std::size_t t0 = 2 * startIn;
    const std::size_t settledFrom = t0 + 8192;
    if (c.iq.size() < settledFrom + 4 * kPeriod) {
        check(false, "the onset capture is long enough to read");
        o.halfMs = 1e9;
        return o;
    }
    double steady = 0.0;
    for (std::size_t n = settledFrom; n < c.iq.size(); ++n) {
        steady += std::abs(c.iq[n]);
    }
    steady /= static_cast<double>(c.iq.size() - settledFrom);
    const auto firstAbove = [&](double dbRel) {
        const double level = steady * std::pow(10.0, dbRel / 20.0);
        for (std::size_t n = t0; n < c.iq.size(); ++n) {
            if (std::abs(c.iq[n]) >= level) {
                return static_cast<double>(n - t0) * kMsPerOut;
            }
        }
        return 1e9;
    };
    o.firstMs = firstAbove(-60.0);
    o.halfMs = firstAbove(-6.0206);
    o.rise1090Ms = firstAbove(-0.9151) - firstAbove(-20.0);
    o.leadMs = o.halfMs - o.firstMs;
    const double halfAt = static_cast<double>(t0) + o.halfMs / kMsPerOut;
    const auto envDb = [&](double ms) {
        const double at = halfAt - ms / kMsPerOut;
        return at < static_cast<double>(t0)
                   ? -400.0
                   : db(std::abs(c.iq[static_cast<std::size_t>(at)]) / steady);
    };
    std::fprintf(stderr, "%s: -60 dB at %.2f ms, 50 %% at %.2f ms; rise 10-90 %% %.2f ms, "
                 "-40..-3 dB %.2f ms; envelope before the 50 %% point: -1 ms %.1f dB, "
                 "-2 ms %.1f dB, -4 ms %.1f dB, -6 ms %.1f dB, -10 ms %.1f dB, -12 ms %.1f dB\n",
                 what, o.firstMs, o.halfMs, o.rise1090Ms, firstAbove(-3.0) - firstAbove(-40.0),
                 envDb(1.0), envDb(2.0), envDb(4.0), envDb(6.0), envDb(10.0), envDb(12.0));
    return o;
}

// Key-up: WdspChannel's mute envelope (10 ms held at zero, then a 25 ms
// raised-cosine slew) sets the shape. It is the anti-click ramp, so its rise
// is bounded from both sides. Mid-over: the 2048-tap bandpass sets the shape,
// so its pre-ringing lead is bounded. Each 50 % point is held within half a
// 1024-sample DSP block of today's figure: another buffer or more taps fails.
constexpr double kMaxKeyUpHalfMs = 106.0;
constexpr double kMinKeyUpRiseMs = 10.0;
constexpr double kMaxKeyUpRiseMs = 20.0;
constexpr double kMaxMidHalfMs = 84.0;
constexpr double kMaxMidLeadMs = 12.0;

void checkKeyUp(const Onset& o)
{
    check(o.halfMs <= kMaxKeyUpHalfMs, "key-up: the 50 % point is at most 106 ms after the first audio");
    check(o.rise1090Ms >= kMinKeyUpRiseMs && o.rise1090Ms <= kMaxKeyUpRiseMs,
          "key-up: the 10-90 % rise is the 10..20 ms anti-click ramp");
}

void checkMidOver(const Onset& o)
{
    check(o.halfMs <= kMaxMidHalfMs, "mid-over: the 50 % point is at most 84 ms after the audio");
    check(o.leadMs <= kMaxMidLeadMs, "mid-over: -60 dB to the 50 % point is at most 12 ms");
}

}  // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    std::fprintf(stderr, "modulator: %s\n", Hl2TxDsp::modulatorName());
    if (std::string(Hl2TxDsp::modulatorName()) != "wdsp-txa") {
        std::fprintf(stderr, "SKIP: these figures are the TXA modulator's\n");
        return 77;
    }

    // The passband per mode, as Hl2Backend pushes it.
    const std::pair<int, int> usb = defaultTxPassbandForModeName("USB");
    const std::pair<int, int> lsb = defaultTxPassbandForModeName("LSB");
    const std::pair<int, int> digu = defaultTxPassbandForModeName("DIGU");
    checkShape("USB voice 300..2700",
               passbandResponse("USB", WdspChannel::Mode::Usb, usb, false), 300, 2700);
    checkShape("LSB voice 300..2700",
               passbandResponse("LSB", WdspChannel::Mode::Lsb, lsb, true), 300, 2700);
    checkShape("DIGU 150..3000",
               passbandResponse("DIGU", WdspChannel::Mode::Digu, digu, false), 150, 3000);

    // The onset: the over's first audio, and an onset after the envelope ran.
    for (int i = 0; i < 2; ++i) {
        checkKeyUp(onset("key-up", false));
        checkMidOver(onset("mid-over", true));
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    if (g_starved) {
        std::fprintf(stderr, "INCONCLUSIVE: a capture stayed starved (machine load)\n");
        return 77;
    }
    std::fprintf(stderr, "all checks passed\n");
    return 0;
}
