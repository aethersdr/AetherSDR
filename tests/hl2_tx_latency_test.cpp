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
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdio>
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
std::vector<std::complex<double>> runChannel(WdspChannel::Config config,
                                             const std::vector<float>& audio)
{
    config.blockForOutput = true;   // see the file header: moves no sample
    std::string error;
    std::unique_ptr<WdspChannel> channel = WdspChannel::create(config, &error);
    if (!channel) {
        std::fprintf(stderr, "channel refused: %s\n", error.c_str());
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

}  // namespace

int main()
{
    runMuteDelayCase();
    if (g_failures != 0) {
        std::fprintf(stderr, "hl2_tx_latency_test: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "hl2_tx_latency_test: all checks passed\n");
    return 0;
}
