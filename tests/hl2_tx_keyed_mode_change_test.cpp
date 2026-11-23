// A mode or passband change that reaches Hl2TxDsp while an over is running is
// held until the over ends: swapping TXA's bandpass mid-stream (above all the
// minimum- to linear-phase swap of USB -> DIGU) splatters. Sample-counted on the
// emitted IQ, socket-free, paced at real time like hl2_tx_passband_onset_test.
//   1. USB -> DIGU keyed, as Hl2Backend::setSliceMode queues it: the over's
//      out-of-band energy around the change stays at the unchanged over's.
//   2. The held change is not lost: it reaches the channel at unkey, and the
//      next over transmits DIGU (a 200 Hz tone,
//      outside USB's 300 Hz edge, passes at a fresh DIGU channel's level).
//   3. Control: the same change made between overs applies to the next over.

#include "core/backends/hl2/Hl2TxDsp.h"
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
#include <functional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace AetherSDR;
using namespace AetherSDR::hl2;
using Mode = WdspChannel::Mode;
using Iq = std::vector<std::complex<float>>;

namespace {

int g_failures = 0;
void check(bool ok, const std::string& what)
{
    std::fprintf(stderr, "%s %s\n", ok ? "ok:  " : "FAIL:", what.c_str());
    if (!ok) {
        ++g_failures;
    }
}

constexpr double kPi = 3.14159265358979323846;
constexpr double kFsIn = 24000.0;
constexpr double kFsOut = 48000.0;
constexpr std::size_t kChunk = 240;            // 10 ms at 24 kHz
constexpr std::size_t kChunks = 120;           // 1.2 s per over
constexpr std::size_t kEvent = 60;             // change queued before 0.6 s of input
constexpr long kMs = 48;                       // output samples per ms

void pumpFor(double ms)
{
    QElapsedTimer t;
    t.start();
    while (t.nsecsElapsed() < static_cast<qint64>(ms * 1e6)) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
}

std::vector<float> tone(double hz, double amp)
{
    std::vector<float> x(kChunk * kChunks);
    for (std::size_t n = 0; n < x.size(); ++n) {
        x[n] = static_cast<float>(amp * std::sin(2.0 * kPi * hz * static_cast<double>(n) / kFsIn));
    }
    return x;
}

struct Over {
    Iq iq;
    unsigned long long faults = 0;
    unsigned long long stallDrops = 0;
};

// One over on `tx`, `event` called before chunk kEvent; tx.reset() after it,
// as the next over's new context does.
Over runOver(Hl2TxDsp& tx, const TxCoordinator::Context& ctx, const std::vector<float>& audio,
             const std::function<void()>& event, const std::function<void()>& beforeReset = {})
{
    Over o;
    const unsigned long long faults0 = tx.modulatorFaultBlocks();
    const unsigned long long drops0 = tx.modulatorStallDrops();
    const QMetaObject::Connection c = QObject::connect(&tx, &Hl2TxDsp::iqReady, &tx,
        [&o](const Iq& iq, const TxCoordinator::Context&) { o.iq.insert(o.iq.end(), iq.begin(), iq.end()); });
    for (std::size_t k = 0; k < kChunks; ++k) {
        if (k == kEvent && event) {
            event();
        }
        const auto first = audio.begin() + static_cast<std::ptrdiff_t>(k * kChunk);
        tx.processAudioBlock(std::vector<float>(first, first + static_cast<std::ptrdiff_t>(kChunk)),
                             TxAudioSource::Microphone, ctx);
        pumpFor(1000.0 * static_cast<double>(kChunk) / kFsIn);
    }
    pumpFor(50.0);
    QObject::disconnect(c);
    o.faults = tx.modulatorFaultBlocks() - faults0;
    o.stallDrops = tx.modulatorStallDrops() - drops0;
    if (beforeReset) {
        beforeReset();
    }
    tx.reset();
    return o;
}

bool configureTx(Hl2TxDsp& tx, Mode mode, double lo, double hi)
{
    Hl2TxDsp::Config cfg;
    cfg.mode = mode;
    cfg.filterLowHz = lo;
    cfg.filterHighHz = hi;
    cfg.alcEnabled = false;   // linear, so a level compares across channels
    std::string err;
    const bool ok = tx.configure(cfg, &err);
    check(ok, "the modulator configures " + err);
    tx.setMicGain(1.0);
    return ok;
}

void fft(std::vector<std::complex<double>>& a)
{
    const std::size_t n = a.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            std::swap(a[i], a[j]);
        }
    }
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2.0 * kPi / static_cast<double>(len);
        const std::complex<double> wl(std::cos(ang), std::sin(ang));
        for (std::size_t i = 0; i < n; i += len) {
            std::complex<double> w(1.0);
            for (std::size_t j = 0; j < len / 2; ++j) {
                const auto u = a[i + j];
                const auto v = a[i + j + len / 2] * w;
                a[i + j] = u + v;
                a[i + j + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

// Energy outside |f| in [100, 3200] Hz (either side: the union of USB's and
// DIGU's passbands with their skirts) over energy inside, in dB, 100 ms Hann
// window from `start`.
double oobDbc(const Iq& x, long start)
{
    const long len = 100 * kMs;
    std::size_t n = 1;
    while (n < static_cast<std::size_t>(len)) {
        n <<= 1;
    }
    std::vector<std::complex<double>> a(n, 0.0);
    for (long i = 0; i < len; ++i) {
        const long k = start + i;
        if (k >= 0 && k < static_cast<long>(x.size())) {
            const double w = 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / (len - 1));
            a[static_cast<std::size_t>(i)] = std::complex<double>(x[static_cast<std::size_t>(k)]) * w;
        }
    }
    fft(a);
    double in = 0.0;
    double out = 0.0;
    for (std::size_t k = 0; k < n; ++k) {
        const double f = (k <= n / 2 ? static_cast<double>(k) : static_cast<double>(k) - static_cast<double>(n))
            * kFsOut / static_cast<double>(n);
        const double af = std::fabs(f);
        ((af >= 100.0 && af <= 3200.0) ? in : out) += std::norm(a[k]);
    }
    return 10.0 * std::log10(std::max(out, 1e-300) / std::max(in, 1e-300));
}

double rms(const Iq& x, std::size_t from)
{
    double acc = 0.0;
    std::size_t n = 0;
    for (std::size_t i = from; i < x.size(); ++i, ++n) {
        acc += std::norm(x[i]);
    }
    return n ? std::sqrt(acc / static_cast<double>(n)) : 0.0;
}

double db(double r) { return 20.0 * std::log10(std::max(r, 1e-30)); }

// The output time at which the change could first show: input block kEvent
// plus the channel's latency is later than this, so the window is generous.
constexpr long kEventOut = static_cast<long>(kEvent * kChunk * 2);
constexpr std::size_t kSettle = 12288;   // key-up ramp and fill, as hl2_tx_passband_onset_test

bool starved(const Over& o) { return o.faults > 0; }

}  // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
#if !AETHER_HL2_TX_TXA
    std::fprintf(stderr, "SKIP: phasing build has no TXA channel\n");
    return 77;
#else
    TxTestAuthority authority;
    const std::vector<float> t1k = tone(1000.0, 0.5);
    const std::vector<float> t200 = tone(200.0, 0.5);
    bool inconclusive = false;

    // Reference levels for the 200 Hz tone on fresh channels.
    double ref200Digu = 0.0;
    double ref200Usb = 0.0;
    {
        Hl2TxDsp d;
        Hl2TxDsp u;
        if (configureTx(d, Mode::Digu, 150.0, 3000.0) && configureTx(u, Mode::Usb, 300.0, 2700.0)) {
            const Over od = runOver(d, authority.context, t200, {});
            const Over ou = runOver(u, authority.context, t200, {});
            inconclusive = inconclusive || starved(od) || starved(ou);
            ref200Digu = rms(od.iq, kSettle);
            ref200Usb = rms(ou.iq, kSettle);
            std::fprintf(stderr, "200 Hz tone: fresh DIGU %.3f dB, fresh USB %.3f dB\n",
                         db(ref200Digu), db(ref200Usb));
            check(db(ref200Digu) - db(ref200Usb) > 40.0,
                  "a 200 Hz tone tells DIGU from USB (USB stops it, DIGU passes it)");
        }
    }

    // 1 + 2. USB -> DIGU while keyed, then the next over.
    {
        Hl2TxDsp ctl;
        Hl2TxDsp tx;
        if (configureTx(ctl, Mode::Usb, 300.0, 2700.0) && configureTx(tx, Mode::Usb, 300.0, 2700.0)) {
            const Over c = runOver(ctl, authority.context, t1k, {});
            bool heldInOver = false;
            const Over e = runOver(tx, authority.context, t1k, [&tx] {
                // Hl2Backend::setSliceMode's two queued calls, in its order.
                tx.setMode(Mode::Digu);
                tx.setFilter(150.0, 3000.0);
            }, [&tx, &heldInOver] {
                const WdspChannel::Config* c = tx.channelConfig();
                heldInOver = c && c->mode == Mode::Usb && c->minimumPhase
                    && tx.config().mode == Mode::Digu;
            });
            inconclusive = inconclusive || starved(c) || starved(e);
            // Worst 100 ms window from the change to 200 ms after it, 10 ms steps.
            double worstEv = -400.0;
            double worstCtl = -400.0;
            for (long w = kEventOut - 50 * kMs; w <= kEventOut + 150 * kMs; w += 10 * kMs) {
                worstEv = std::max(worstEv, oobDbc(e.iq, w));
                worstCtl = std::max(worstCtl, oobDbc(c.iq, w));
            }
            std::fprintf(stderr, "keyed USB -> DIGU: out-of-band %.1f dBc, unchanged over %.1f dBc\n",
                         worstEv, worstCtl);
            check(worstEv - worstCtl <= 10.0,
                  "a keyed USB -> DIGU change does not splatter (out-of-band within 10 dB of the unchanged over)");

            check(heldInOver,
                  "the change is held while the over runs: the channel is still USB, minimum phase");
            const WdspChannel::Config* applied = tx.channelConfig();
            check(applied && applied->mode == Mode::Digu && !applied->minimumPhase,
                  "the held change reaches the channel when the over ends (reset)");

            const Over next = runOver(tx, authority.context, t200, {});
            inconclusive = inconclusive || starved(next);
            const double lvl = rms(next.iq, kSettle);
            std::fprintf(stderr, "next over, 200 Hz tone: %.3f dB (fresh DIGU %.3f dB)\n", db(lvl), db(ref200Digu));
            check(std::fabs(db(lvl) - db(ref200Digu)) < 0.5,
                  "the held change applies to the next over: it transmits DIGU");
            applied = tx.channelConfig();
            check(applied && applied->mode == Mode::Digu && !applied->minimumPhase
                      && applied->filterLowHz == 150.0 && applied->filterHighHz == 3000.0,
                  "after the over the channel runs DIGU, linear phase, 150..3000 Hz");
        }
    }

    // 3. Control: the same change between overs reaches the next over at once.
    {
        Hl2TxDsp tx;
        if (configureTx(tx, Mode::Usb, 300.0, 2700.0)) {
            const Over first = runOver(tx, authority.context, t1k, {});
            tx.setMode(Mode::Digu);
            tx.setFilter(150.0, 3000.0);
            const WdspChannel::Config* applied = tx.channelConfig();
            check(applied && applied->mode == Mode::Digu && !applied->minimumPhase,
                  "a change between overs reaches the channel immediately");
            const Over second = runOver(tx, authority.context, t200, {});
            inconclusive = inconclusive || starved(first) || starved(second);
            check(std::fabs(db(rms(second.iq, kSettle)) - db(ref200Digu)) < 0.5,
                  "a change between overs: the next over transmits DIGU");
        }
    }

    if (g_failures > 0) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    if (inconclusive) {
        std::fprintf(stderr, "SKIP: the worker was starved (machine load); inconclusive\n");
        return 77;
    }
    std::fprintf(stderr, "all checks passed\n");
    return 0;
#endif
}
