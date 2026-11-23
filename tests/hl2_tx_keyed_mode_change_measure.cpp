// Measurement, not a pass/fail test: what a mode or passband change WHILE KEYED
// does to the HL2's transmitted IQ, call by call. Offline, through the real
// Hl2TxDsp and WdspChannel; no socket, no radio, nothing keyed.
//
// Each event runs before input chunk kEvent (1.000 s) of a 2.0 s over. Some go
// through Hl2TxDsp as Hl2Backend::setSliceMode does (setMode, then setFilter);
// others call ONE WdspChannel setter on the open channel (pureSignalChannel()),
// to isolate which call does it. One JSON object per line on stdout. Metric
// definitions: hl2-lab streams/bench-runner/runs/tx-keyed-mode-change-2026-11.

#include "core/backends/hl2/Hl2TxDsp.h"
#include "TxTestAuthority.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using AetherSDR::TxAudioSource;
using AetherSDR::TxCoordinator;
using AetherSDR::hl2::Hl2TxDsp;
using Mode = WdspChannel::Mode;
using Iq = std::vector<std::complex<float>>;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kInRate = 24000.0;
constexpr double kOutRate = 48000.0;
constexpr std::size_t kChunk = 240;     // 10 ms at 24 kHz
constexpr std::size_t kChunks = 200;    // 2.0 s per over
constexpr std::size_t kEvent = 100;     // event before this chunk: 1.000 s of input

// Same stimuli as hl2_tx_release_measure (maxi-release-steps-2026-11).
std::vector<float> toneStim()
{
    std::vector<float> x(kChunk * kChunks);
    for (std::size_t n = 0; n < x.size(); ++n) {
        x[n] = static_cast<float>(0.5 * std::sin(2.0 * kPi * 1000.0 * n / kInRate));
    }
    return x;
}

std::vector<float> twoToneStim()
{
    std::vector<float> x(kChunk * kChunks);
    for (std::size_t n = 0; n < x.size(); ++n) {
        const double t = n / kInRate;
        x[n] = static_cast<float>(0.25 * std::sin(2.0 * kPi * 700.0 * t)
                                  + 0.25 * std::sin(2.0 * kPi * 1900.0 * t));
    }
    return x;
}

std::vector<float> voiceStim()
{
    const std::size_t N = kChunk * kChunks;
    std::vector<double> src(N);
    double phase = 0.0;
    double prevG = 0.0;
    std::uint32_t lcg = 0x2545F491u;
    for (std::size_t n = 0; n < N; ++n) {
        const double t = n / kInRate;
        const double f0 = 120.0 * (1.0 + 0.08 * std::sin(2.0 * kPi * 0.7 * t)
                                   + 0.03 * std::sin(2.0 * kPi * 5.3 * t));
        phase += f0 / kInRate;
        phase -= std::floor(phase);
        double g = 0.0;
        if (phase < 0.4) {
            g = 0.5 * (1.0 - std::cos(kPi * phase / 0.4));
        } else if (phase < 0.56) {
            g = std::cos(kPi * (phase - 0.4) / 0.32);
        }
        const double dg = g - prevG;
        prevG = g;
        lcg = lcg * 1664525u + 1013904223u;
        const double noise = (static_cast<double>(lcg >> 8) / 16777216.0 - 0.5) * 0.004;
        src[n] = dg + noise;
    }
    std::vector<double> y(N, 0.0);
    struct Res { double f0, f1, bw, y1 = 0, y2 = 0; };
    Res res[3] = {{550, 750, 80}, {1100, 1700, 100}, {2400, 2600, 130}};
    for (std::size_t n = 0; n < N; ++n) {
        const double t = n / kInRate;
        const double m = 0.5 + 0.5 * std::sin(2.0 * kPi * 0.6 * t);
        double acc = 0.0;
        for (auto& r : res) {
            const double fc = r.f0 + (r.f1 - r.f0) * m;
            const double rad = std::exp(-kPi * r.bw / kInRate);
            const double a1 = 2.0 * rad * std::cos(2.0 * kPi * fc / kInRate);
            const double a2 = -rad * rad;
            const double out = (1.0 - rad) * src[n] + a1 * r.y1 + a2 * r.y2;
            r.y2 = r.y1;
            r.y1 = out;
            acc += out;
        }
        const double s = std::sin(2.0 * kPi * 3.25 * t);
        const double env = 0.08 + 0.92 * std::pow(std::max(0.0, s), 0.6);
        y[n] = acc * env;
    }
    double peak = 0.0;
    for (double v : y) {
        peak = std::max(peak, std::fabs(v));
    }
    std::vector<float> x(N);
    for (std::size_t n = 0; n < N; ++n) {
        x[n] = static_cast<float>(0.6 * y[n] / peak);
    }
    return x;
}

void pumpFor(double ms)
{
    QElapsedTimer t;
    t.start();
    while (t.nsecsElapsed() < static_cast<qint64>(ms * 1e6)) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
}

struct Start {
    Mode mode;
    double lo;
    double hi;
};

using Action = std::function<void(Hl2TxDsp&)>;

struct Out {
    Iq iq;
    bool ok = false;
    int attempts = 0;
};

Out runOnce(const std::vector<float>& audio, Start st, const Action& event, unsigned long long* faults)
{
    TxTestAuthority authority;
    Out out;
    Hl2TxDsp tx;
    Hl2TxDsp::Config cfg;
    cfg.mode = st.mode;
    cfg.filterLowHz = st.lo;
    cfg.filterHighHz = st.hi;
    std::string err;
    if (!tx.configure(cfg, &err)) {
        std::fprintf(stderr, "configure: %s\n", err.c_str());
        return out;
    }
    tx.setMicGain(1.0);
    QObject::connect(&tx, &Hl2TxDsp::iqReady, &tx,
                     [&](const Iq& iq, const TxCoordinator::Context&) {
        out.iq.insert(out.iq.end(), iq.begin(), iq.end());
    });
    for (std::size_t k = 0; k < kChunks; ++k) {
        if (k == kEvent && event) {
            event(tx);
        }
        std::vector<float> chunk(audio.begin() + k * kChunk, audio.begin() + (k + 1) * kChunk);
        tx.processAudioBlock(chunk, TxAudioSource::Microphone, authority.context);
        pumpFor(1000.0 * kChunk / kInRate);
    }
    pumpFor(50.0);
    *faults = tx.modulatorFaultBlocks();
    out.ok = true;
    return out;
}

Out run(const std::vector<float>& audio, Start st, const Action& event)
{
    for (int attempt = 1; attempt <= 4; ++attempt) {
        unsigned long long faults = 0;
        Out o = runOnce(audio, st, event, &faults);
        o.attempts = attempt;
        if (o.ok && faults == 0) {
            return o;
        }
        std::fprintf(stderr, "RETRY attempt %d: %llu fault(s)\n", attempt, faults);
    }
    return {};
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

// In-band mask: the union of the passbands on either side of the event, each
// with 200 Hz of skirt, as signed wire frequencies [lo, hi].
using Mask = std::vector<std::pair<double, double>>;

struct Spec { double inband = 0, oob = 0, plus = 0, minus = 0; };

Spec spectrum(const Iq& x, long start, long len, const Mask& mask)
{
    std::size_t n = 1;
    while (n < static_cast<std::size_t>(len)) {
        n <<= 1;
    }
    std::vector<std::complex<double>> a(n, 0.0);
    for (long i = 0; i < len; ++i) {
        const long k = start + i;
        if (k < 0 || k >= static_cast<long>(x.size())) {
            continue;
        }
        const double w = 0.5 - 0.5 * std::cos(2.0 * kPi * i / (len - 1));
        a[static_cast<std::size_t>(i)] = std::complex<double>(x[k]) * w;
    }
    fft(a);
    Spec s;
    for (std::size_t k = 0; k < n; ++k) {
        const double f = (k <= n / 2 ? static_cast<double>(k) : static_cast<double>(k) - n) * kOutRate / n;
        const double e = std::norm(a[k]);
        if (f >= 100.0 && f <= 2900.0) {
            s.plus += e;
        }
        if (f <= -100.0 && f >= -2900.0) {
            s.minus += e;
        }
        bool in = false;
        for (const auto& [lo, hi] : mask) {
            in = in || (f >= lo && f <= hi);
        }
        (in ? s.inband : s.oob) += e;
    }
    return s;
}

double rms(const Iq& x, long a, long b)
{
    double acc = 0;
    long n = 0;
    for (long i = std::max(0L, a); i < std::min<long>(b, x.size()); ++i, ++n) {
        acc += std::norm(x[i]);
    }
    return n ? std::sqrt(acc / n) : 0.0;
}

double maxDelta(const Iq& x, long a, long b)
{
    double p = 0;
    for (long i = std::max(1L, a); i < std::min<long>(b, x.size()); ++i) {
        p = std::max(p, static_cast<double>(std::abs(x[i] - x[i - 1])));
    }
    return p;
}

double db20(double r) { return 20.0 * std::log10(std::max(r, 1e-300)); }
double db10(double r) { return 10.0 * std::log10(std::max(r, 1e-300)); }

long firstDiff(const Iq& a, const Iq& b)
{
    const std::size_t n = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i) {
        if (std::abs(a[i] - b[i]) > 1e-6) {
            return static_cast<long>(i);
        }
    }
    return -1;
}

// Which wire side a single-sideband control carries, from its energy.
int sideOf(const Iq& x, long w0, long len)
{
    const Spec s = spectrum(x, w0, len, {});
    return s.plus >= s.minus ? 1 : -1;
}

void addBand(Mask& m, Mode mode, double lo, double hi, int side)
{
    const double a = std::min(std::fabs(lo), std::fabs(hi));
    const double b = std::max(std::fabs(lo), std::fabs(hi));
    if (mode == Mode::Am || mode == Mode::Sam || mode == Mode::Dsb || mode == Mode::Fm) {
        m.push_back({-(b + 200.0), b + 200.0});
    } else if (side > 0) {
        m.push_back({a - 200.0, b + 200.0});
    } else {
        m.push_back({-(b + 200.0), -(a - 200.0)});
    }
}

struct Event {
    std::string name;
    Start from;
    Start to;        // the steady state after the change, for the second control
    Action action;
};

// ev = event run; before = control in the start state, no event; after =
// control started in the target state, no event.
void report(const Event& e, const std::string& stim, const Out& ev, const Out& before, const Out& after)
{
    const long ms = 48;
    std::printf("{\"event\":\"%s\",\"stim\":\"%s\"", e.name.c_str(), stim.c_str());
    const long tE = (ev.ok && before.ok) ? firstDiff(ev.iq, before.iq) : -2;
    if (tE < 0) {
        std::printf(",\"tE_ms\":null,\"diverges\":false,\"error\":\"%s\"}\n",
                    tE == -1 ? "identical to the control (no divergence)" : "run failed");
        std::fflush(stdout);
        return;
    }
    const long w0 = tE - 50 * ms;
    const long len = 100 * ms;
    const int sideFrom = sideOf(before.iq, w0, len);
    const int sideTo = sideOf(after.iq, w0, len);
    Mask mask;
    addBand(mask, e.from.mode, e.from.lo, e.from.hi, sideFrom);
    addBand(mask, e.to.mode, e.to.lo, e.to.hi, sideTo);
    const Spec se = spectrum(ev.iq, w0, len, mask);
    const Spec sb = spectrum(before.iq, w0, len, mask);
    const Spec sa = spectrum(after.iq, w0, len, mask);
    const double oobEv = db10(se.oob / se.inband);
    const double oobB = db10(sb.oob / sb.inband);
    const double oobA = db10(sa.oob / sa.inband);
    const double ref = std::max(oobB, oobA);
    // Level after the event against the target state's own control.
    const double step = db20(rms(ev.iq, tE + 10 * ms, tE + 60 * ms) / rms(after.iq, tE + 10 * ms, tE + 60 * ms));
    const double late = db20(rms(ev.iq, tE + 200 * ms, tE + 400 * ms) / rms(after.iq, tE + 200 * ms, tE + 400 * ms));
    const double dEv = maxDelta(ev.iq, w0, w0 + len);
    const double dB = maxDelta(before.iq, w0, w0 + len);
    const double dA = maxDelta(after.iq, w0, w0 + len);
    // The splatter's own spectrum: event minus the better-matching control,
    // integrated outside the mask. Shown as dBc against the event's in-band.
    std::printf(",\"tE_ms\":%.3f,\"diverges\":true,\"side_from\":%d,\"side_to\":%d,"
                "\"oob_dBc_event\":%.2f,\"oob_dBc_before\":%.2f,\"oob_dBc_after\":%.2f,\"oob_rise_dB\":%.2f,"
                "\"level_step_vs_after_dB\":%.3f,\"late_vs_after_dB\":%.3f,"
                "\"max_dIQ_event\":%.5f,\"max_dIQ_before\":%.5f,\"max_dIQ_after\":%.5f,"
                "\"dIQ_rise_dB\":%.2f,\"attempts\":%d}\n",
                tE / 48.0, sideFrom, sideTo, oobEv, oobB, oobA, oobEv - ref, step, late,
                dEv, dB, dA, db20(dEv / std::max(dB, dA)), ev.attempts);
    std::fflush(stdout);
}

}  // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const std::map<std::string, std::vector<float>> stims = {
        {"tone", toneStim()}, {"twotone", twoToneStim()}, {"voice", voiceStim()}};
    const Start usb{Mode::Usb, 300.0, 2700.0};
    const Start lsb{Mode::Lsb, 300.0, 2700.0};
    const Start digu{Mode::Digu, 150.0, 3000.0};
    const Start am{Mode::Am, 100.0, 3000.0};
    const Start usbWide{Mode::Usb, 100.0, 2900.0};
    // Intermediate states reached by one isolated call from USB 300..2700.
    const Start usbLin{Mode::Usb, 300.0, 2700.0};      // USB, linear phase (control below)
    const Start usbMpFilt{Mode::Usb, 150.0, 3000.0};   // USB filter widened, still min phase

    auto ch = [](Hl2TxDsp& tx) { return tx.pureSignalChannel(); };
    const std::vector<Event> events = {
        // Negative control: the same mode re-pushed, as every setSliceMode does.
        {"noop-usb-repush", usb, usb, [](Hl2TxDsp& tx) { tx.setMode(Mode::Usb); tx.setFilter(300.0, 2700.0); }},
        // The operator path, as Hl2Backend::setSliceMode queues it.
        {"usb-digu-hl2path", usb, digu, [](Hl2TxDsp& tx) { tx.setMode(Mode::Digu); tx.setFilter(150.0, 3000.0); }},
        // The previous harness's event: Hl2TxDsp::setMode only (filter unchanged).
        {"usb-digu-setmode-only", usb, {Mode::Digu, 300.0, 2700.0}, [](Hl2TxDsp& tx) { tx.setMode(Mode::Digu); }},
        // One WDSP call at a time on the open channel.
        {"wdsp-SetTXAMode-only", usb, usb, [ch](Hl2TxDsp& tx) { ch(tx)->setMode(Mode::Digu); }},
        {"wdsp-TXASetMP-off-only", usb, usbLin, [ch](Hl2TxDsp& tx) { ch(tx)->setMinimumPhase(false); }},
        {"wdsp-SetTXABandpassFreqs-only-mp", usb, usbMpFilt, [ch](Hl2TxDsp& tx) { ch(tx)->setFilter(150.0, 3000.0); }},
        {"wdsp-mode+bandpass-no-mp", usb, digu, [ch](Hl2TxDsp& tx) { ch(tx)->setMode(Mode::Digu); ch(tx)->setFilter(150.0, 3000.0); }},
        {"wdsp-mp+mode-no-bandpass", usb, {Mode::Digu, 300.0, 2700.0}, [ch](Hl2TxDsp& tx) { ch(tx)->setMode(Mode::Digu); ch(tx)->setMinimumPhase(false); }},
        // Wider: other mode pairs and a passband change in the same mode.
        {"digu-usb-hl2path", digu, usb, [](Hl2TxDsp& tx) { tx.setMode(Mode::Usb); tx.setFilter(300.0, 2700.0); }},
        {"lsb-usb-hl2path", lsb, usb, [](Hl2TxDsp& tx) { tx.setMode(Mode::Usb); tx.setFilter(300.0, 2700.0); }},
        {"usb-am-hl2path", usb, am, [](Hl2TxDsp& tx) { tx.setMode(Mode::Am); tx.setFilter(100.0, 3000.0); }},
        {"usb-passband-100-2900", usb, usbWide, [](Hl2TxDsp& tx) { tx.setFilter(100.0, 2900.0); }},
        {"digu-passband-100-2900", digu, {Mode::Digu, 100.0, 2900.0}, [](Hl2TxDsp& tx) { tx.setFilter(100.0, 2900.0); }},
    };

    // The second control of an isolated call is a channel started in that call's
    // end state; the WDSP-only states are reached on a fresh channel the same way.
    std::map<std::string, Action> afterSetup = {
        {"wdsp-SetTXAMode-only", [ch](Hl2TxDsp& tx) { ch(tx)->setMode(Mode::Digu); }},
        {"wdsp-TXASetMP-off-only", [ch](Hl2TxDsp& tx) { ch(tx)->setMinimumPhase(false); }},
        {"wdsp-SetTXABandpassFreqs-only-mp", [ch](Hl2TxDsp& tx) { ch(tx)->setFilter(150.0, 3000.0); }},
        {"wdsp-mode+bandpass-no-mp", [ch](Hl2TxDsp& tx) { ch(tx)->setMode(Mode::Digu); ch(tx)->setFilter(150.0, 3000.0); }},
        {"wdsp-mp+mode-no-bandpass", [ch](Hl2TxDsp& tx) { ch(tx)->setMode(Mode::Digu); ch(tx)->setMinimumPhase(false); }},
    };

    for (const auto& [stimName, audio] : stims) {
        std::map<std::string, Out> controls;
        auto control = [&](const Start& s) -> const Out& {
            const std::string key = std::to_string(static_cast<int>(s.mode)) + "/"
                + std::to_string(s.lo) + "/" + std::to_string(s.hi);
            auto it = controls.find(key);
            if (it == controls.end()) {
                it = controls.emplace(key, run(audio, s, {})).first;
            }
            return it->second;
        };
        for (const Event& e : events) {
            const Out ev = run(audio, e.from, e.action);
            const Out& before = control(e.from);
            Out afterOwn;
            const Out* after = nullptr;
            const auto setup = afterSetup.find(e.name);
            if (setup != afterSetup.end()) {
                // The end state applied before the first block (channel not yet running).
                Action pre = setup->second;
                for (int attempt = 1; attempt <= 4; ++attempt) {
                    Hl2TxDsp tx;
                    Hl2TxDsp::Config cfg;
                    cfg.mode = e.from.mode;
                    cfg.filterLowHz = e.from.lo;
                    cfg.filterHighHz = e.from.hi;
                    std::string err;
                    if (!tx.configure(cfg, &err)) {
                        break;
                    }
                    tx.setMicGain(1.0);
                    pre(tx);
                    Out o;
                    QObject::connect(&tx, &Hl2TxDsp::iqReady, &tx,
                                     [&](const Iq& iq, const TxCoordinator::Context&) {
                        o.iq.insert(o.iq.end(), iq.begin(), iq.end());
                    });
                    TxTestAuthority a2;
                    for (std::size_t k = 0; k < kChunks; ++k) {
                        std::vector<float> chunk(audio.begin() + k * kChunk, audio.begin() + (k + 1) * kChunk);
                        tx.processAudioBlock(chunk, TxAudioSource::Microphone, a2.context);
                        pumpFor(1000.0 * kChunk / kInRate);
                    }
                    pumpFor(50.0);
                    if (tx.modulatorFaultBlocks() == 0) {
                        o.ok = true;
                        o.attempts = attempt;
                        afterOwn = std::move(o);
                        break;
                    }
                }
                after = &afterOwn;
            } else {
                after = &control(e.to);
            }
            report(e, stimName, ev, before, *after);
        }
    }
    return 0;
}
