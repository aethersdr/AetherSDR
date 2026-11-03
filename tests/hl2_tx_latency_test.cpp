// Transmit latency of the Hermes-Lite 2's WDSP TXA channel, measured offline.
//
// #6052 measured ~117 ms of keyed silence at the start of every HL2 over on
// hardware, and its triage placed most of it UPSTREAM of the EP2 queue, inside
// the transmit chain. This file measures the part that lives in the TXA
// channel, on emitted IQ, with no radio and no network:
//
//   ONSET   a 1 kHz tone that starts at input sample 0 of a freshly started
//           channel. How many 48 kHz output samples pass before the first
//           sample at or above one EP2 LSB (-84 dB re the settled level, the
//           wire's own "first non-zero"), and before 10 / 50 / 90 % of the
//           settled envelope. This is the key-down edge as the wire sees it.
//
//   IMPULSE the channel settled past its up-ramp, then one impulse. Where its
//           response peaks, and the group delay across the passband (the
//           phase slope, read with the bulk delay removed so it cannot wrap).
//           This is the latency every syllable pays, not only the first.
//
//   SIDEBAND the opposite-sideband suppression and the in-band level of a
//           steady tone, so a latency change that cost selectivity or
//           flatness is caught by the same file that claims the latency.
//
// Every channel here is opened from Hl2TxDsp::modulatorChannelConfig(), which
// is the function buildModulator() opens the product's channel with, so what
// is measured is the product's channel and not a copy of its arithmetic.
//
// ONE DEVIATION, and it moves no sample: blockForOutput is set true. The
// product runs it false and is paced by the sound card; an offline feed has no
// pacing, and unpaced it would starve the channel (see the note above feed()
// in hl2_txdsp_test.cpp). blockForOutput only decides whether fexchange2 WAITS
// for Sem_OutReady; the output ring's pre-load and every index into it are the
// same either way (create_iobuffs / fexchange2, iobuffs.c), so sample
// positions -- which is all this file reads -- do not depend on it.
//
// No radio was keyed for any of this. The figures are IQ above the wire.

#include "core/backends/hl2/Hl2TxDsp.h"
#include "core/dsp/WdspChannel.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdio>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

using AetherSDR::hl2::Hl2TxDsp;

namespace {

int g_failures = 0;

void check(bool ok, const char* what)
{
    std::fprintf(stderr, "%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) {
        ++g_failures;
    }
}

constexpr double kPi = 3.14159265358979323846;

// Run `audio` (mono, at the channel's input rate) through a freshly created and
// started TX channel, in whole input blocks, and return the wire-order IQ.
// Empty on any refusal or non-Ok block, which the caller reports.
// `beforeFeed`, when given, runs on the open channel before the first block --
// the seam for a runtime setter (setMinimumPhase) that the product calls on a
// live channel.
std::vector<std::complex<double>> runChannel(
    WdspChannel::Config config, const std::vector<float>& audio,
    const std::function<bool(WdspChannel&)>& beforeFeed = {})
{
    config.blockForOutput = true;   // see the file header: moves no sample
    std::string error;
    std::unique_ptr<WdspChannel> channel = WdspChannel::create(config, &error);
    if (!channel) {
        std::fprintf(stderr, "channel refused: %s\n", error.c_str());
        return {};
    }
    if (beforeFeed && !beforeFeed(*channel)) {
        std::fprintf(stderr, "the pre-feed control call was refused\n");
        return {};
    }
    const std::size_t in = config.inputBlockSize;
    const std::size_t out = channel->outputBlockSize();
    std::vector<float> zeroQ(in, 0.0f);
    std::vector<float> outI(out, 0.0f);
    std::vector<float> outQ(out, 0.0f);
    std::vector<std::complex<double>> iq;
    iq.reserve(audio.size() / in * out);
    for (std::size_t off = 0; off + in <= audio.size(); off += in) {
        // Mono audio in I, zeros in Q -- the arrangement Hl2TxDsp::modulate()
        // uses, and the only one xpanel (inselect = 2) passes.
        const WdspChannel::ProcessResult r = channel->processIq(
            std::span<const float>(audio.data() + off, in), zeroQ, outI, outQ);
        if (r != WdspChannel::ProcessResult::Ok) {
            std::fprintf(stderr, "block at input sample %zu did not process\n", off);
            return {};
        }
        for (std::size_t k = 0; k < out; ++k) {
            iq.emplace_back(outI[k], outQ[k]);
        }
    }
    return iq;
}

struct Onset {
    bool ok = false;
    long firstLsb = -1;   // first output sample >= settled / 16384 (one EP2 LSB at -6 dBFS)
    long rise10 = -1;
    long rise50 = -1;
    long rise90 = -1;
};

// Tone onset at key-down: the channel is started and the tone begins at input
// sample 0, which is what Hl2TxDsp::modulate() does on the first block of an
// over. Positions are output samples at the channel's output rate, counted from
// the first input sample.
Onset measureOnset(const WdspChannel::Config& config)
{
    constexpr double kToneHz = 1000.0;
    constexpr float kAmplitude = 0.5f;          // -6 dBFS, the #6052 hardware tone
    constexpr std::size_t kBlocks = 40;         // 0.85 s at 24 kHz / 512
    std::vector<float> audio(config.inputBlockSize * kBlocks);
    for (std::size_t n = 0; n < audio.size(); ++n) {
        audio[n] = kAmplitude * static_cast<float>(
            std::sin(2.0 * kPi * kToneHz * static_cast<double>(n)
                     / static_cast<double>(config.inputSampleRate)));
    }
    const std::vector<std::complex<double>> iq = runChannel(config, audio);
    Onset o;
    if (iq.size() < 4 * 1024) {
        return o;
    }
    // Settled level from the last quarter, far past any ramp or filter fill.
    double settled = 0.0;
    for (std::size_t i = iq.size() - iq.size() / 4; i < iq.size(); ++i) {
        settled = std::max(settled, std::abs(iq[i]));
    }
    if (settled <= 0.0) {
        return o;
    }
    for (std::size_t i = 0; i < iq.size(); ++i) {
        const double m = std::abs(iq[i]);
        const long at = static_cast<long>(i);
        if (o.firstLsb < 0 && m >= settled / 16384.0) o.firstLsb = at;
        if (o.rise10 < 0 && m >= 0.1 * settled) o.rise10 = at;
        if (o.rise50 < 0 && m >= 0.5 * settled) o.rise50 = at;
        if (o.rise90 < 0 && m >= 0.9 * settled) o.rise90 = at;
    }
    o.ok = o.firstLsb >= 0 && o.rise90 >= 0;
    return o;
}

double ms(long samples, int rate)
{
    return 1000.0 * static_cast<double>(samples) / static_cast<double>(rate);
}

void printOnset(const char* label, const Onset& o, int rate)
{
    std::fprintf(stderr,
        "PROBE  onset %-34s first>=LSB %5ld (%6.2f ms)  10%% %5ld (%6.2f ms)"
        "  50%% %5ld (%6.2f ms)  90%% %5ld (%6.2f ms)\n",
        label, o.firstLsb, ms(o.firstLsb, rate), o.rise10, ms(o.rise10, rate),
        o.rise50, ms(o.rise50, rate), o.rise90, ms(o.rise90, rate));
}

// ── (a) The mute DELAY is gone from transmit; the mute SLEW is not ─────────
void runMuteDelayCase()
{
    const Hl2TxDsp::Config product{};
    const WdspChannel::Config c = Hl2TxDsp::modulatorChannelConfig(product);
    const int outRate = c.outputSampleRate;

    // The reference is the SAME channel with the delay WdspChannel::Config
    // opens every channel with by default -- read off the struct, not retyped,
    // so it is whatever receive still uses.
    WdspChannel::Config withDelay = c;
    withDelay.muteDelayUpSec = WdspChannel::Config{}.muteDelayUpSec;

    const Onset now = measureOnset(c);
    const Onset ref = measureOnset(withDelay);
    check(now.ok && ref.ok, "(a) both onset legs measured");
    printOnset("HL2 TX channel (product)", now, outRate);
    printOnset("same, with the shared 10 ms delay", ref, outRate);
    if (!now.ok || !ref.ok) {
        return;
    }

    // What the delay costs, derived: upslew0 zeroes ndelup input samples after
    // the first non-zero one, which is ndelup x (out/in) output samples.
    const double expectedDelta = WdspChannel::Config{}.muteDelayUpSec
        * static_cast<double>(outRate);
    const long delta = ref.firstLsb - now.firstLsb;
    std::fprintf(stderr,
        "PROBE  (a) the transmit channel's first sample above one EP2 LSB is"
        " %ld samples (%.2f ms) earlier than with the 10 ms delay; the delay"
        " is %.0f samples\n", delta, ms(delta, outRate), expectedDelta);
    check(std::abs(static_cast<double>(delta) - expectedDelta) <= 8.0,
          "(a) the HL2 transmit channel opens without the 10 ms mute delay: "
          "its first audio reaches the wire one delay (480 samples at 48 kHz) "
          "earlier, and the samples it used to zero are audio");
    check(std::abs(static_cast<double>(ref.rise50 - now.rise50) - expectedDelta) <= 8.0,
          "(a) and the whole envelope moves with it, not only its first sample");

    // THE DE-CLICK IS KEPT. A raised-cosine up-slew of T seconds rises 10 -> 90 %
    // in 0.59 T; with no slew at all the rise is the filter's own, a few ms.
    // Asserted as at least half the slew, so it fails if the ramp is removed
    // and does not re-type the ramp's exact shape.
    const long rise = now.rise90 - now.rise10;
    std::fprintf(stderr, "PROBE  (a) 10-90 %% rise %ld samples (%.2f ms), up-slew %.0f ms\n",
                 rise, ms(rise, outRate), 1000.0 * c.muteSlewUpSec);
    check(c.muteSlewUpSec > 0.0
              && static_cast<double>(rise) >= 0.5 * c.muteSlewUpSec * outRate,
          "(a) the 25 ms up-slew is still there: the over still ramps in, "
          "it just starts on the first sample of audio");

    // RECEIVE IS NOT TOUCHED. The override lives in the transmit channel's
    // config only; the shared default every receiver opens with keeps its delay.
    check(WdspChannel::Config{}.muteDelayUpSec > 0.0,
          "(a) receive keeps WdspChannel::Config's mute delay");
}

// ── (b) the transmit bandpass: minimum phase in the voice modes ──────────

// Which wire bin a tone lands on. The HPSDR wire has the opposite handedness to
// the textbook analytic signal (hl2_txdsp_test's header): an upper-sideband
// mode puts +f audio at -f on the wire.
double wireSign(WdspChannel::Mode mode)
{
    switch (mode) {
    case WdspChannel::Mode::Lsb:
    case WdspChannel::Mode::Cwl:
    case WdspChannel::Mode::Digl:
        return 1.0;
    default:
        return -1.0;
    }
}

WdspChannel::Config productChannel(WdspChannel::Mode mode, double lowHz, double highHz)
{
    Hl2TxDsp::Config cfg;
    cfg.mode = mode;
    cfg.filterLowHz = lowHz;
    cfg.filterHighHz = highHz;
    return Hl2TxDsp::modulatorChannelConfig(cfg);
}

struct Impulse {
    bool ok = false;
    std::vector<std::complex<double>> h;   // from the impulse's own output position
    long peak = -1;                        // output samples after the impulse
};

// The channel is first walked past its up-ramp with a -80 dBFS tone (the ramp
// only starts on non-zero input), then left silent, then handed one impulse at
// a -40 dBFS level that keeps the always-on TXA ALC (max_gain 1.0) linear.
Impulse measureImpulse(const WdspChannel::Config& config,
                       const std::function<bool(WdspChannel&)>& beforeFeed = {})
{
    const std::size_t in = config.inputBlockSize;
    const std::size_t upsample =
        static_cast<std::size_t>(config.outputSampleRate / config.inputSampleRate);
    constexpr std::size_t kBlocks = 60;
    constexpr std::size_t kImpulseBlock = 30;
    std::vector<float> audio(in * kBlocks, 0.0f);
    const std::size_t primer = static_cast<std::size_t>(config.inputSampleRate / 10);
    for (std::size_t n = 0; n < primer; ++n) {
        audio[n] = 1.0e-4f * static_cast<float>(
            std::sin(2.0 * kPi * 1000.0 * static_cast<double>(n)
                     / static_cast<double>(config.inputSampleRate)));
    }
    const std::size_t at = in * kImpulseBlock;
    audio[at] = 0.01f;
    const std::vector<std::complex<double>> iq = runChannel(config, audio, beforeFeed);
    Impulse r;
    const std::size_t from = at * upsample;
    constexpr std::size_t kSpan = 8192;
    if (iq.size() < from + kSpan) {
        return r;
    }
    r.h.assign(iq.begin() + static_cast<std::ptrdiff_t>(from),
               iq.begin() + static_cast<std::ptrdiff_t>(from + kSpan));
    double best = 0.0;
    for (std::size_t i = 0; i < r.h.size(); ++i) {
        if (std::abs(r.h[i]) > best) {
            best = std::abs(r.h[i]);
            r.peak = static_cast<long>(i);
        }
    }
    r.ok = best > 0.0;
    return r;
}

// Group delay at wire frequency w (Hz, signed), in output samples. The phase
// is taken relative to the impulse peak so the slope that is differentiated is
// only the residual, a few hundred samples at most -- a 10 Hz step can then
// never wrap. The peak is added back.
double groupDelayAt(const Impulse& r, double wireHz, int rate)
{
    const auto H = [&](double f) {
        std::complex<double> acc = 0.0;
        for (std::size_t n = 0; n < r.h.size(); ++n) {
            acc += r.h[n] * std::polar(1.0, -2.0 * kPi * f
                * (static_cast<double>(n) - static_cast<double>(r.peak))
                / static_cast<double>(rate));
        }
        return acc;
    };
    constexpr double kStep = 5.0;
    double dphi = std::arg(H(wireHz + kStep)) - std::arg(H(wireHz - kStep));
    while (dphi > kPi) dphi -= 2.0 * kPi;
    while (dphi < -kPi) dphi += 2.0 * kPi;
    return -dphi / (2.0 * kPi * 2.0 * kStep / static_cast<double>(rate))
        + static_cast<double>(r.peak);
}

struct Profile {
    double minimum = 1e300;
    double maximum = -1e300;
    double atOneKhz = 0.0;
};

// Group delay over the passband, 50 Hz in from each edge, every 100 Hz.
Profile delayProfile(const Impulse& r, WdspChannel::Mode mode, double lowHz,
                     double highHz, int rate, const char* label)
{
    Profile p;
    std::fprintf(stderr, "PROBE  group delay %-18s", label);
    for (double f = std::ceil((lowHz + 50.0) / 100.0) * 100.0; f <= highHz - 50.0; f += 100.0) {
        const double g = groupDelayAt(r, wireSign(mode) * f, rate);
        p.minimum = std::min(p.minimum, g);
        p.maximum = std::max(p.maximum, g);
        if (static_cast<long>(f) % 500 == 0) {
            std::fprintf(stderr, " %4.0f:%7.1f", f, g);
        }
    }
    p.atOneKhz = groupDelayAt(r, wireSign(mode) * 1000.0, rate);
    std::fprintf(stderr, "  spread %.1f samples (%.2f ms)\n",
                 p.maximum - p.minimum, 1000.0 * (p.maximum - p.minimum) / rate);
    return p;
}

double binPower(const std::vector<std::complex<double>>& iq, std::size_t from,
                std::size_t count, double wireHz, int rate)
{
    std::complex<double> acc = 0.0;
    for (std::size_t n = 0; n < count; ++n) {
        acc += iq[from + n] * std::polar(1.0, -2.0 * kPi * wireHz
            * static_cast<double>(n) / static_cast<double>(rate));
    }
    return std::norm(acc) / (static_cast<double>(count) * static_cast<double>(count));
}

struct Tone {
    bool ok = false;
    double wantedDb = 0.0;       // wanted-bin power, dB
    double suppressionDb = 0.0;  // wanted over image, dB
};

// A steady tone well past every ramp. The window is a WHOLE number of the
// tone's periods, so the image bin sits on a null of the rectangular window's
// kernel and the figure is the channel's, not the window's (the reason is
// spelled out above wholeCycles() in hl2_txdsp_test.cpp).
Tone measureTone(const WdspChannel::Config& config, double toneHz)
{
    constexpr std::size_t kBlocks = 120;
    std::vector<float> audio(config.inputBlockSize * kBlocks);
    for (std::size_t n = 0; n < audio.size(); ++n) {
        audio[n] = 0.1f * static_cast<float>(
            std::sin(2.0 * kPi * toneHz * static_cast<double>(n)
                     / static_cast<double>(config.inputSampleRate)));
    }
    const std::vector<std::complex<double>> iq = runChannel(config, audio);
    Tone t;
    constexpr std::size_t kSettle = 12288;
    const int rate = config.outputSampleRate;
    const long f = std::lround(toneHz);
    long a = rate;
    long b = f;
    while (b != 0) { const long r = a % b; a = b; b = r; }
    const std::size_t period = static_cast<std::size_t>(rate / a);
    if (iq.size() <= kSettle + period) {
        return t;
    }
    const std::size_t count = ((iq.size() - kSettle) / period) * period;
    const double w = wireSign(config.mode) * toneHz;
    const double wanted = binPower(iq, kSettle, count, w, rate);
    const double image = binPower(iq, kSettle, count, -w, rate);
    constexpr double kEps = 1e-300;
    t.wantedDb = 10.0 * std::log10(wanted + kEps);
    t.suppressionDb = 10.0 * std::log10((wanted + kEps) / (image + kEps));
    t.ok = wanted > 0.0;
    return t;
}

// EP2 carries I and Q as signed 16-bit (MetisProtocol.cpp, ep2WriteTxIq): one
// LSB of full scale is 20 log10(32768) = 90.3 dB down, and no image below that
// can reach the radio. Held with 30 dB to spare, so a real regression of the
// filter fails here long before it could reach the wire.
constexpr double kSuppressionFloorDb = 90.3 + 30.0;

void runBandpassCase()
{
    struct Leg {
        const char* name;
        WdspChannel::Mode mode;
        double lowHz;
        double highHz;
        bool voice;
    };
    // The passbands Hl2Backend pushes by default (hl2::defaultTxPassbandForModeName).
    const Leg legs[] = {
        {"USB {300,2700}", WdspChannel::Mode::Usb, 300.0, 2700.0, true},
        {"LSB {300,2700}", WdspChannel::Mode::Lsb, 300.0, 2700.0, true},
        {"DIGU {150,3000}", WdspChannel::Mode::Digu, 150.0, 3000.0, false},
        {"DIGL {150,3000}", WdspChannel::Mode::Digl, 150.0, 3000.0, false},
    };
    for (const Leg& leg : legs) {
        const WdspChannel::Config product = productChannel(leg.mode, leg.lowHz, leg.highHz);
        WdspChannel::Config linear = product;
        linear.minimumPhase = false;
        const int rate = product.outputSampleRate;

        const Impulse now = measureImpulse(product);
        const Impulse ref = measureImpulse(linear);
        check(now.ok && ref.ok, "(b) both impulse legs measured");
        if (!now.ok || !ref.ok) {
            continue;
        }
        std::fprintf(stderr,
            "PROBE  (b) %-16s impulse peak: product %ld (%.2f ms), linear-phase %ld"
            " (%.2f ms), delta %ld samples (%.2f ms)\n",
            leg.name, now.peak, ms(now.peak, rate), ref.peak, ms(ref.peak, rate),
            ref.peak - now.peak, ms(ref.peak - now.peak, rate));
        const Profile pNow = delayProfile(now, leg.mode, leg.lowHz, leg.highHz, rate,
                                          leg.voice ? "product (MP)" : "product (linear)");
        const Profile pRef = delayProfile(ref, leg.mode, leg.lowHz, leg.highHz, rate,
                                          "linear reference");
        check(pRef.maximum - pRef.minimum <= 1.0,
              "(b) the instrument: a linear-phase channel reads a flat group delay");

        if (leg.voice) {
            // THE CLAIM. Minimum phase at the same length moves the 1 kHz group
            // delay at least 12.5 ms (600 samples) earlier; measured, not read
            // back from Config.
            check(pRef.atOneKhz - pNow.atOneKhz >= 600.0,
                  "(b) voice modes: the transmit bandpass runs minimum phase, and "
                  "a 1 kHz tone reaches the wire at least 12.5 ms sooner");
            check(pNow.maximum < pRef.minimum,
                  "(b) and it is sooner across the WHOLE voice passband, edges included");
        } else {
            check(now.peak == ref.peak && pNow.maximum - pNow.minimum <= 1.0,
                  "(b) data modes are untouched: linear phase, flat group delay, "
                  "the same delay as before");
        }

        // SELECTIVITY AND FLATNESS, the cost the latency must not have. In-band
        // tones near both edges and mid-band.
        const double tones[] = {leg.lowHz + 100.0, 1000.0, leg.highHz - 100.0};
        for (const double f : tones) {
            const Tone a = measureTone(product, f);
            const Tone b = measureTone(linear, f);
            check(a.ok && b.ok, "(b) both tone legs measured");
            if (!a.ok || !b.ok) {
                continue;
            }
            std::fprintf(stderr,
                "PROBE  (b) %-16s %6.0f Hz: suppression product %6.1f dB, linear %6.1f dB;"
                " in-band level product - linear %+.4f dB\n",
                leg.name, f, a.suppressionDb, b.suppressionDb, a.wantedDb - b.wantedDb);
            check(a.suppressionDb >= kSuppressionFloorDb,
                  "(b) opposite-sideband suppression stays 30 dB beyond the 16-bit wire");
            check(std::abs(a.wantedDb - b.wantedDb) <= 0.1,
                  "(b) the passband level is unchanged (minimum phase keeps |H|)");
        }
    }

    // THE RUNTIME DOOR. Hl2TxDsp::applyModeAndFilter() switches the phase on a
    // live channel when the operator changes mode (USB <-> DIGU), through
    // WdspChannel::setMinimumPhase -- which refused every transmit channel
    // before this change. Open linear, switch, and land where open() lands.
    {
        const WdspChannel::Config product =
            productChannel(WdspChannel::Mode::Usb, 300.0, 2700.0);
        WdspChannel::Config linear = product;
        linear.minimumPhase = false;
        const Impulse opened = measureImpulse(product);
        double switchMs = -1.0;
        const Impulse switched = measureImpulse(linear, [&switchMs](WdspChannel& ch) {
            // Timed because the product makes this call on the HL2 I/O thread,
            // the one that paces EP2. Printed, not asserted: it is wall clock.
            const auto t0 = std::chrono::steady_clock::now();
            const bool ok = ch.setMinimumPhase(true);
            switchMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t0).count();
            return ok;
        });
        std::fprintf(stderr, "PROBE  (b) setMinimumPhase(true) on a live TX channel took %.2f ms"
                     " (planner bounded by the test environment)\n", switchMs);
        check(opened.ok && switched.ok,
              "(b) setMinimumPhase(true) is accepted on a live transmit channel");
        if (opened.ok && switched.ok) {
            std::fprintf(stderr,
                "PROBE  (b) USB opened minimum-phase peak %ld; opened linear then "
                "setMinimumPhase(true) peak %ld\n", opened.peak, switched.peak);
            check(std::abs(opened.peak - switched.peak) <= 1,
                  "(b) switching phase at runtime lands where opening does");
        }
    }
    // And the Config the stage asks for, per mode -- a readback, which is why
    // it comes last: every claim above was measured on emitted IQ.
    check(Hl2TxDsp::txMinimumPhaseFor(WdspChannel::Mode::Usb)
              && Hl2TxDsp::txMinimumPhaseFor(WdspChannel::Mode::Lsb)
              && !Hl2TxDsp::txMinimumPhaseFor(WdspChannel::Mode::Digu)
              && !Hl2TxDsp::txMinimumPhaseFor(WdspChannel::Mode::Digl)
              && !Hl2TxDsp::txMinimumPhaseFor(WdspChannel::Mode::Cwu)
              && !Hl2TxDsp::txMinimumPhaseFor(WdspChannel::Mode::Cwl),
          "(b) minimum phase in USB/LSB only");

    // The key-down onset, after both changes, for the record.
    const WdspChannel::Config usb = productChannel(WdspChannel::Mode::Usb, 300.0, 2700.0);
    WdspChannel::Config before = usb;
    before.minimumPhase = false;
    before.muteDelayUpSec = WdspChannel::Config{}.muteDelayUpSec;
    const Onset a = measureOnset(before);
    const Onset b = measureOnset(usb);
    if (a.ok && b.ok) {
        printOnset("USB, 10 ms delay + linear phase", a, usb.outputSampleRate);
        printOnset("USB, as shipped by this branch", b, usb.outputSampleRate);
    }
}

}  // namespace

int main()
{
    runMuteDelayCase();
    runBandpassCase();
    if (g_failures != 0) {
        std::fprintf(stderr, "hl2_tx_latency_test: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "hl2_tx_latency_test: all checks passed\n");
    return 0;
}
