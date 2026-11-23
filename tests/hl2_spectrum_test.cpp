// aetherd HL2 Phase 1a — Hl2Spectrum unit test. Feeds synthetic IQ through the
// FFT panadapter path and checks: a complex tone peaks at the expected
// fftshifted bin, DC lands at the centre bin, partial frames accumulate across
// calls, a large DC offset is removed so it does not swamp a real tone, and a
// strong tone off a bin centre leaves no line at the centre bin.

#include "core/backends/hl2/Hl2Spectrum.h"
#include "core/dsp/WdspChannel.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
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

// Complex tone at integer bin k0: amp * exp(i 2π k0 n / N).
static std::vector<std::complex<float>> tone(int n, int k0, float amp, std::complex<float> dc = {})
{
    std::vector<std::complex<float>> v(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        const double ph = 2.0 * kPi * k0 * i / n;
        v[static_cast<std::size_t>(i)] =
            amp * std::complex<float>(static_cast<float>(std::cos(ph)),
                                      static_cast<float>(std::sin(ph))) + dc;
    }
    return v;
}

static int argmax(const std::vector<float>& v)
{
    int m = 0;
    for (int i = 1; i < static_cast<int>(v.size()); ++i)
        if (v[static_cast<std::size_t>(i)] > v[static_cast<std::size_t>(m)]) m = i;
    return m;
}

// The centre bin under a strong off-bin tone.
// A production-size stream: 1024-point frames at 192 kHz, fed in 126-sample
// EP6 blocks, with seeded Gaussian noise so the floor is a real floor. The tone
// sits ~90 dB over it, as the HL2's own TX does in RX1 while keyed.

namespace {

constexpr int kStreamN = 1024;
constexpr double kStreamFs = 192000.0;
constexpr int kEp6Block = 126;
constexpr double kNoiseSigma = 3e-4;   // per component: ~-90 dBFS per bin

// xorshift64* + Box-Muller: the same noise on every standard library.
struct Noise {
    std::uint64_t state = 0x9E3779B97F4A7C15ull;
    double uniform()
    {
        state ^= state >> 12; state ^= state << 25; state ^= state >> 27;
        return (static_cast<double>((state * 0x2545F4914F6CDD1Dull) >> 11) + 0.5) / 9007199254740992.0;
    }
    std::complex<double> gaussian(double sigma)
    {
        const double r = sigma * std::sqrt(-2.0 * std::log(uniform()));
        const double th = 2.0 * kPi * uniform();
        return {r * std::cos(th), r * std::sin(th)};
    }
};

struct Signal {
    double toneHz = 0.0;     // offset from centre; 0 with toneAmp 0 = none
    double toneAmp = 0.0;
    std::complex<double> dc{};
    double onsetS = 0.0;     // tone and DC start this long into the call (noise before)
    double endS = 1e9;       // ...and stop this long into it
    double rampS = 0.0;      // raised-cosine ramp at the end, and at onset when onsetS > 0
};

// What the stream showed: per-frame centre-bin prominence, and the power
// average of every frame, for a floor and a peak that do not jitter.
struct Seen {
    std::vector<double> centreOverFloorDb;    // one per frame, against the averaged floor
    std::vector<double> meanPower;            // per bin, averaged over the frames
    int frames = 0;
    int rampFrames = 0;                       // left out: they hold envelope-ramp samples
};

double floorDbOf(const std::vector<double>& power)
{
    // Median of the bins 4..60 either side of centre: clear of DC and of the
    // tones used below (which sit >= 66 bins out).
    std::vector<double> ring;
    const int half = kStreamN / 2;
    for (int k = 4; k <= 60; ++k) {
        ring.push_back(power[static_cast<std::size_t>(half + k)]);
        ring.push_back(power[static_cast<std::size_t>(half - k)]);
    }
    std::nth_element(ring.begin(), ring.begin() + ring.size() / 2, ring.end());
    return 10.0 * std::log10(ring[ring.size() / 2]);
}

double centreOverFloorDb(const Seen& seen)
{
    return 10.0 * std::log10(seen.meanPower[kStreamN / 2]) - floorDbOf(seen.meanPower);
}

// Streams `seconds` of `sig` through `spec`. capFps > 0 mirrors Hl2RxDsp's
// display cap: accumulate() between due frames, process() once one is due.
// Frames are collected only when `collect` is set, and only those that lie
// wholly inside this call: a frame straddling a key-on carries the onset's
// real splatter, which is not what is under test.
Seen stream(Hl2Spectrum& spec, Noise& noise, std::uint64_t& n, const Signal& sig,
            double seconds, bool collect, double capFps = 0.0, bool collectAll = false)
{
    Seen seen;
    seen.meanPower.assign(kStreamN, 0.0);
    std::vector<std::vector<float>> frames;
    std::vector<std::complex<float>> block(kEp6Block);
    std::vector<float> bins;
    const std::uint64_t begin = n;
    const std::uint64_t onset = n + static_cast<std::uint64_t>(sig.onsetS * kStreamFs);
    const double endN = static_cast<double>(n) + sig.endS * kStreamFs;
    const std::uint64_t end = n + static_cast<std::uint64_t>(seconds * kStreamFs);
    const std::uint64_t dueEvery = capFps > 0.0 ? static_cast<std::uint64_t>(kStreamFs / capFps) : 0;
    std::uint64_t lastFrame = n;
    while (n < end) {
        for (int i = 0; i < kEp6Block; ++i, ++n) {
            const double ph = 2.0 * kPi * sig.toneHz * static_cast<double>(n) / kStreamFs;
            const double t = static_cast<double>(n);
            double on = n >= onset && t < endN ? 1.0 : 0.0;
            if (on > 0.0 && sig.rampS > 0.0) {
                const double ramp = sig.rampS * kStreamFs;
                const double fromOn = sig.onsetS > 0.0 ? t - static_cast<double>(onset) : 1e18;
                const double edge = std::min(fromOn, endN - t);
                if (edge < ramp) {
                    on = 0.5 * (1.0 - std::cos(kPi * edge / ramp));
                }
            }
            const std::complex<double> x = on * (sig.toneAmp * std::complex<double>(std::cos(ph), std::sin(ph))
                                                 + sig.dc)
                                         + noise.gaussian(kNoiseSigma);
            block[static_cast<std::size_t>(i)] = {static_cast<float>(x.real()), static_cast<float>(x.imag())};
        }
        if (dueEvery > 0 && n - lastFrame < dueEvery) {
            spec.accumulate(block);
            continue;
        }
        if (spec.process(block, bins) > 0) {
            lastFrame = n;
            // A frame holding samples of an envelope ramp carries a DC in
            // transition, which no constant estimate cancels: left out.
            const double first = static_cast<double>(n) - kStreamN;
            const double last = static_cast<double>(n);
            const double ramp = sig.rampS * kStreamFs;
            const double on0 = static_cast<double>(onset);
            const bool inRamp = ramp > 0.0
                && ((sig.onsetS > 0.0 && first < on0 + ramp && last >= on0)
                    || (first < endN && last >= endN - ramp));
            if (collect && inRamp) {
                ++seen.rampFrames;
            } else if (collect && (collectAll || n - begin >= static_cast<std::uint64_t>(kStreamN + kEp6Block))) {
                frames.push_back(bins);
            }
        }
    }
    seen.frames = static_cast<int>(frames.size());
    for (const auto& f : frames) {
        for (int k = 0; k < kStreamN; ++k) {
            seen.meanPower[static_cast<std::size_t>(k)] +=
                std::pow(10.0, f[static_cast<std::size_t>(k)] / 10.0) / static_cast<double>(frames.size());
        }
    }
    if (!frames.empty()) {
        const double floorDb = floorDbOf(seen.meanPower);
        for (const auto& f : frames) {
            seen.centreOverFloorDb.push_back(f[kStreamN / 2] - floorDb);
        }
    }
    return seen;
}

double maxOf(const std::vector<double>& v)
{
    return v.empty() ? 1e9 : *std::max_element(v.begin(), v.end());
}

void centreBinTests()
{
    // 12544 Hz (66.901 bins) and -41000 Hz (-218.667 bins): neither completes
    // a whole number of cycles in a frame, so each has a non-zero frame mean.
    const double kOffBinHz[] = {12544.0, -41000.0};
    for (const double hz : kOffBinHz) {
        for (const double capFps : {0.0, 30.0}) {
            // 1 · KEY-ON: the display has been running on noise, then the
            // strong tone appears: the HL2's TX leaking into RX1 (with the T/R
            // edge), or a strong received carrier (without it).
            for (const bool viaTransmit : {true, false}) {
                Hl2Spectrum spec(kStreamN, kStreamFs);
                Noise noise;
                std::uint64_t n = 0;
                stream(spec, noise, n, {}, 1.0, false, capFps);
                spec.setTransmitting(viaTransmit);
                const Seen keyed = stream(spec, noise, n, {hz, 0.5, {}}, 1.0, true, capFps);
                const double peakDb = 10.0 * std::log10(*std::max_element(keyed.meanPower.begin(),
                                                                           keyed.meanPower.end()));
                std::fprintf(stderr, "  off-bin %+.0f Hz cap %.0f fps, %s: tone %.1f dB over floor, "
                             "centre %.1f dB (mean), %.1f dB (worst of %d frames)\n",
                             hz, capFps, viaTransmit ? "keyed" : "received", peakDb - floorDbOf(keyed.meanPower), centreOverFloorDb(keyed),
                             maxOf(keyed.centreOverFloorDb), keyed.frames);
                check(keyed.frames > 20, "key-on: frames were produced");
                check(peakDb - floorDbOf(keyed.meanPower) > 80.0,
                      "key-on control: the tone stands ~90 dB over the floor");
                check(centreOverFloorDb(keyed) < 3.0,
                      "key-on: a strong off-bin tone with no DC leaves the centre bin at the floor");
                check(maxOf(keyed.centreOverFloorDb) < 12.0,
                      "key-on: no single frame shows a line at the centre bin");
            }
            // 1b · COLD START with the tone already present: a line may show
            // while the estimate's memory is short, and is gone after 1 s.
            {
                Hl2Spectrum spec(kStreamN, kStreamFs);
                Noise noise;
                std::uint64_t n = 0;
                stream(spec, noise, n, {hz, 0.5, {}}, 1.0, false, capFps);
                const Seen later = stream(spec, noise, n, {hz, 0.5, {}}, 1.0, true, capFps);
                check(centreOverFloorDb(later) < 3.0,
                      "cold start under a strong tone: the centre bin is at the floor after 1 s");
            }
        }
    }

    // 2 · A CONSTANT complex DC offset (a converter bias; hpsdrsim carries
    // one on I) is removed: from the first frame on when alone, and under a
    // strong tone once the estimate's memory has grown (1 s).
    for (const double toneAmp : {0.0, 0.5}) {
        Hl2Spectrum spec(kStreamN, kStreamFs);
        Noise noise;
        std::uint64_t n = 0;
        const Signal sig{12544.0, toneAmp, {0.02, -0.01}};      // DC ~-32 dBFS
        std::vector<float> firstBins;
        std::vector<std::complex<float>> firstFrame(kStreamN);
        for (auto& x : firstFrame) {
            const double ph = 2.0 * kPi * sig.toneHz * static_cast<double>(n++) / kStreamFs;
            const std::complex<double> v = sig.toneAmp * std::complex<double>(std::cos(ph), std::sin(ph))
                                         + sig.dc + noise.gaussian(kNoiseSigma);
            x = {static_cast<float>(v.real()), static_cast<float>(v.imag())};
        }
        check(spec.process(firstFrame, firstBins) == 1, "DC: one frame from the first 1024 samples");
        stream(spec, noise, n, sig, 1.0, false);
        const Seen rest = stream(spec, noise, n, sig, 1.0, true);
        const double floorDb = floorDbOf(rest.meanPower);
        std::fprintf(stderr, "  DC offset, tone amp %.1f: centre %.1f dB first frame, %.1f dB mean of %d after 1 s\n",
                     toneAmp, firstBins[kStreamN / 2] - floorDb, centreOverFloorDb(rest), rest.frames);
        if (toneAmp == 0.0) {
            check(firstBins[kStreamN / 2] - floorDb < 12.0,
                  "DC: a constant offset alone is removed from the first frame");
        }
        check(centreOverFloorDb(rest) < 3.0, "DC: and is removed in steady state");
        // Control: this offset is ~58 dB over the floor when not removed.
        check(20.0 * std::log10(std::abs(sig.dc)) + Hl2Spectrum::kToneGainDb - floorDb > 50.0,
              "DC control: the offset would stand far over the floor if left in");
    }

    // 4 · T/R: a key whose IQ carries a strong DC term (seen at LNA 48 dB)
    // as well as the tone, then receive with neither. Receive must be at the
    // floor from the first full frame after the release hold, and a second
    // key must start from the DC the first one learned.
    for (const double capFps : {0.0, 30.0}) {
        constexpr double kHoldMs = 70.0;
        const Signal keyedSig{12544.0, 0.5, {5e-4, -2e-4}};   // DC ~30 dB over the floor
        Hl2Spectrum spec(kStreamN, kStreamFs);
        Noise noise;
        std::uint64_t n = 0;
        stream(spec, noise, n, {}, 1.0, false, capFps);
        spec.setTransmitting(true);
        stream(spec, noise, n, keyedSig, 4.0, false, capFps);
        spec.setTransmitting(false, kHoldMs);
        check(spec.dcTransmitting(), "T/R: the transmit estimate stays in use through the hold");
        stream(spec, noise, n, {}, kHoldMs / 1000.0, false, capFps);
        check(!spec.dcTransmitting(), "T/R: receive resumes when the hold has run");
        const Seen rx = stream(spec, noise, n, {}, 2.0, true, capFps);
        std::fprintf(stderr, "  T/R cap %.0f fps, after unkey: centre %.1f dB (mean), %.1f dB (first), "
                     "%.1f dB (worst of %d frames)\n", capFps, centreOverFloorDb(rx),
                     rx.centreOverFloorDb.empty() ? 1e9 : rx.centreOverFloorDb.front(),
                     maxOf(rx.centreOverFloorDb), rx.frames);
        check(!rx.centreOverFloorDb.empty() && rx.centreOverFloorDb.front() < 12.0,
              "T/R: the first full receive frame after the unkey has no centre line");
        check(maxOf(rx.centreOverFloorDb) < 12.0 && centreOverFloorDb(rx) < 3.0,
              "T/R: receive after the unkey stays at the floor");

        spec.setTransmitting(true);
        const Seen second = stream(spec, noise, n, keyedSig, 1.0, true, capFps);
        std::fprintf(stderr, "  T/R cap %.0f fps, second key: centre %.1f dB (mean), %.1f dB (worst)\n",
                     capFps, centreOverFloorDb(second), maxOf(second.centreOverFloorDb));
        check(maxOf(second.centreOverFloorDb) < 12.0 && centreOverFloorDb(second) < 3.0,
              "T/R: a second key starts from the transmit DC the first one learned");

        // Control: the keyed DC is real and would show if not removed.
        check(20.0 * std::log10(std::abs(keyedSig.dc)) + Hl2Spectrum::kToneGainDb
                  - floorDbOf(rx.meanPower) > 25.0,
              "T/R control: the keyed DC stands far over the receive floor");
    }

    // 5 · KEY-ON LATENCY: the keyed IQ arrives ~120 ms after the host's MOX
    // edge on the wire, behind a few-ms envelope ramp. A second key (carried
    // transmit DC) must keep every frame from the MOX edge on at the floor,
    // the receive-only latency frames and the one straddling the edge included;
    // only frames holding ramp samples are left out (see stream()).
    // 6 · dropAverage() on both T/R edges (as a T/R-time average drop does)
    // leaves the DC estimates alone.
    for (const double latencyS : {0.0005, 0.0021, 0.12}) {
        for (const bool dropOnEdges : {false, true}) {
            for (const double capFps : {0.0, 30.0}) {
                constexpr double kHoldMs = 70.0;
                const Signal keyedSig{12544.0, 0.5, {5e-4, -2e-4}, latencyS, 1e9, 0.004};
                Hl2Spectrum spec(kStreamN, kStreamFs);
                Noise noise;
                std::uint64_t n = 0;
                auto edge = [&](bool on) {
                    spec.setTransmitting(on, kHoldMs);
                    if (dropOnEdges) {
                        spec.dropAverage();
                    }
                };
                // The keyed IQ outlasts the unkey by ~30 ms on the wire.
                Signal tail = keyedSig;
                tail.onsetS = 0.0;
                tail.endS = 0.03;
                stream(spec, noise, n, {}, 1.0, false, capFps);
                edge(true);
                stream(spec, noise, n, keyedSig, 6.0, false, capFps);
                edge(false);
                stream(spec, noise, n, tail, 1.0, false, capFps);
                edge(true);
                const Seen second = stream(spec, noise, n, keyedSig, 1.0, true, capFps, true);
                edge(false);
                // 6 s: a wrong estimate left selected after a release would persist.
                const Seen rx = stream(spec, noise, n, tail, 6.0, true, capFps, true);
                std::fprintf(stderr, "  key-on latency %.4f s, cap %.0f fps%s: second key worst %.1f dB "
                             "(first %.1f, mean %.1f), after unkey worst %.1f dB; ramp frames left out %d + %d\n",
                             latencyS, capFps, dropOnEdges ? ", dropAverage on edges" : "",
                             maxOf(second.centreOverFloorDb),
                             second.centreOverFloorDb.empty() ? 1e9 : second.centreOverFloorDb.front(),
                             centreOverFloorDb(second), maxOf(rx.centreOverFloorDb), second.rampFrames, rx.rampFrames);
                check(second.frames > 20, "latency: frames from the edge on were produced");
                check(second.rampFrames <= 2 && rx.rampFrames <= 2,
                      "latency: at most two frames per edge are left out for the ramp");
                check(maxOf(second.centreOverFloorDb) < 12.0,
                      dropOnEdges ? "dropAverage() on T/R edges: the second key's frames stay at the floor"
                                  : "latency: every frame of the second key, from its edge on, stays at the floor");
                check(centreOverFloorDb(second) < 3.0, "latency: the second key's mean centre is at the floor");
                check(maxOf(rx.centreOverFloorDb) < 12.0 && centreOverFloorDb(rx) < 3.0,
                      dropOnEdges ? "dropAverage() on T/R edges: receive after the unkey stays at the floor"
                                  : "latency: receive after the second unkey stays at the floor for 6 s");
            }
        }
    }

    // 7 · A SEQUENCE GAP mid-key (reset(), as Hl2RxDsp::onSequenceGap()
    // calls it) restarts the receive estimate only: the keyed frames after it
    // keep the transmit DC the key learned, with no centre line.
    for (const double capFps : {0.0, 30.0}) {
        const Signal keyedSig{12544.0, 0.5, {5e-4, -2e-4}};   // DC ~30 dB over the floor
        Hl2Spectrum spec(kStreamN, kStreamFs);
        Noise noise;
        std::uint64_t n = 0;
        stream(spec, noise, n, {}, 1.0, false, capFps);
        spec.setTransmitting(true);
        stream(spec, noise, n, keyedSig, 4.0, false, capFps);
        spec.reset();
        check(spec.dcTransmitting(), "gap mid-key: the transmit estimate stays in use");
        const Seen after = stream(spec, noise, n, keyedSig, 1.0, true, capFps);
        std::fprintf(stderr, "  gap mid-key cap %.0f fps: centre %.1f dB (mean), %.1f dB (first), "
                     "%.1f dB (worst of %d frames)\n", capFps, centreOverFloorDb(after),
                     after.centreOverFloorDb.empty() ? 1e9 : after.centreOverFloorDb.front(),
                     maxOf(after.centreOverFloorDb), after.frames);
        check(after.frames > 20, "gap mid-key: frames were produced");
        check(maxOf(after.centreOverFloorDb) < 12.0 && centreOverFloorDb(after) < 3.0,
              "gap mid-key: the keyed frames after the gap keep the transmit DC (no centre line)");
    }

    // 8 · A REBUILD mid-key (Hl2RxDsp::installChannel() on a pan rate
    // crossing): the new spectrum takes the DC estimates and the T/R state from
    // the old one. Receive after the unkey stays at the floor and the next key
    // switches to transmit. Control: a fresh spectrum given only the key learns
    // the keyed DC as receive DC and shows a line after the unkey.
    for (const bool carry : {true, false}) {
        for (const double capFps : {0.0, 30.0}) {
            constexpr double kHoldMs = 70.0;
            const Signal keyedSig{12544.0, 0.5, {5e-4, -2e-4}};   // DC ~30 dB over the floor
            Signal tail = keyedSig;
            tail.endS = 0.03;                                    // keyed IQ outlasts the unkey
            auto spec = std::make_unique<Hl2Spectrum>(kStreamN, kStreamFs);
            Noise noise;
            std::uint64_t n = 0;
            stream(*spec, noise, n, {}, 1.0, false, capFps);
            spec->setTransmitting(true);
            stream(*spec, noise, n, keyedSig, 3.0, false, capFps);
            auto rebuilt = std::make_unique<Hl2Spectrum>(kStreamN, kStreamFs);
            if (carry) {
                rebuilt->carryDcFrom(*spec);
            }
            rebuilt->setTransmitting(true);                      // as installChannel() re-applies it
            spec = std::move(rebuilt);
            const Seen rest = stream(*spec, noise, n, keyedSig, 2.0, true, capFps);
            spec->setTransmitting(false, kHoldMs);
            stream(*spec, noise, n, tail, 0.1, false, capFps);
            const Seen rx = stream(*spec, noise, n, {}, 3.0, true, capFps);
            spec->setTransmitting(true);
            const Seen next = stream(*spec, noise, n, keyedSig, 3.0, true, capFps);
            const bool nextSwitched = spec->dcTransmitting();
            std::fprintf(stderr, "  rebuild mid-key%s, cap %.0f fps: rest of key %.1f dB (mean), after unkey "
                         "%.1f dB (mean) %.1f dB (worst), next key %.1f dB (mean), switched %s\n",
                         carry ? "" : " (control, no carry)", capFps, centreOverFloorDb(rest),
                         centreOverFloorDb(rx), maxOf(rx.centreOverFloorDb), centreOverFloorDb(next),
                         nextSwitched ? "yes" : "no");
            if (carry) {
                check(centreOverFloorDb(rest) < 6.0, "rebuild mid-key: the rest of the key keeps the transmit DC");
                check(centreOverFloorDb(rx) < 3.0 && maxOf(rx.centreOverFloorDb) < 12.0,
                      "rebuild mid-key: receive after the unkey stays at the floor");
                check(nextSwitched && centreOverFloorDb(next) < 3.0,
                      "rebuild mid-key: the next key switches to transmit, at the floor");
            } else {
                check(centreOverFloorDb(rx) > 12.0,
                      "rebuild control: without the carry, receive after the unkey shows the keyed DC");
            }
        }
    }

    // 3 · An ON-BIN tone (bin 100 exactly) reads its textbook level and the
    // centre bin stays at the floor.
    {
        Hl2Spectrum spec(kStreamN, kStreamFs);
        Noise noise;
        std::uint64_t n = 0;
        const double hz = 100.0 * kStreamFs / kStreamN;
        const Seen seen = stream(spec, noise, n, {hz, 0.5, {}}, 1.0, true);
        const double peakDb = 10.0 * std::log10(seen.meanPower[static_cast<std::size_t>(kStreamN / 2 + 100)]);
        check(std::fabs(peakDb - (20.0 * std::log10(0.5) + Hl2Spectrum::kToneGainDb)) < 0.05,
              "on-bin tone: reads 20*log10(A) + kToneGainDb, unchanged");
        check(centreOverFloorDb(seen) < 3.0, "on-bin tone: the centre bin is at the floor");
    }
}

}  // namespace

int main()
{
    constexpr int N = 64;
    const int half = N / 2;

    // ---- tone at bin 10 -> peak at fftshifted bin (10 + 32) % 64 = 42 ----
    {
        Hl2Spectrum spec(N);
        std::vector<float> bins;
        const int frames = spec.process(tone(N, 10, 0.5f), bins);
        check(frames == 1, "one frame from N samples");
        check(bins.size() == static_cast<std::size_t>(N), "N bins produced");
        const int peak = argmax(bins);
        check(peak == (10 + half) % N, "tone peaks at expected fftshifted bin");
        check(bins[static_cast<std::size_t>(peak)] > -8.0f, "peak near -6 dBFS (amp 0.5)");
        check(bins[static_cast<std::size_t>((peak + half) % N)] < -30.0f, "opposite bin is floor");
    }

    // ---- DC (bin 0) lands at the centre bin ----
    {
        Hl2Spectrum spec(N);
        std::vector<float> bins;
        // A pure DC tone would be removed by DC subtraction, so use a near-DC
        // bin (k0 = 1) and confirm it maps just off centre, and a real DC-bin
        // signal that survives: feed bin 1.
        spec.process(tone(N, 1, 0.5f), bins);
        check(argmax(bins) == (1 + half) % N, "bin-1 tone maps adjacent to centre");
    }

    // ---- partial frames accumulate across process() calls ----
    {
        Hl2Spectrum spec(N);
        std::vector<float> bins;
        const auto t = tone(N, 7, 0.5f);
        std::span<const std::complex<float>> s(t);
        check(spec.process(s.subspan(0, 30), bins) == 0, "30/64 -> no frame yet");
        check(spec.process(s.subspan(30), bins) == 1, "remaining 34 completes the frame");
        check(argmax(bins) == (7 + half) % N, "accumulated frame decodes the tone");
    }

    // ---- large DC offset is removed, tone survives ----
    {
        Hl2Spectrum spec(N);
        std::vector<float> bins;
        // DC offset of 0.4 on I (as a real HL2 ADC bias) + a smaller tone at bin 20.
        spec.process(tone(N, 20, 0.1f, std::complex<float>(0.4f, 0.0f)), bins);
        const int peak = argmax(bins);
        check(peak == (20 + half) % N, "tone peak survives a large DC offset (DC removed)");
        check(bins[static_cast<std::size_t>(half)] < bins[static_cast<std::size_t>(peak)],
              "centre (DC) bin is below the tone after DC removal");
    }

    // ---- frame averaging integrates POWER, and takes the log once (#5794) ----
    //
    // The arithmetic mean of logarithms is the logarithm of the geometric
    // mean. An average taken on dBFS values therefore computes the geometric
    // mean of power while the operator reads the control as "average the power
    // over N frames", and the two are not close: the error depends on how the
    // signal varies, so it crushes transients and biases a noise floor low
    // without ever biasing a steady carrier, which is why it survives a test
    // tone. See Hl2Spectrum::setAverageFrames() and RFC #5782 §3.
    //
    // The reference is not a reimplementation of the transform. A second
    // instance with averaging OFF produces the per-frame spectrum, and the
    // expectations below are built from ITS output — so a change to the
    // window, the DC removal or the normalisation moves both sides together
    // and only the averaging domain is under test here.
    {
        constexpr int kBin = 10;                    // tone bin; peaks at kBin + half
        constexpr int kDepth = 4;                   // frames of integration
        const std::size_t peakBin = static_cast<std::size_t>((kBin + half) % N);

        // Three quiet frames and a burst in the last: a CW element, an SSB
        // syllable, a switching-supply tick. Nothing separates the two
        // estimators on a steady signal, which the control below relies on.
        const float kQuiet = 1e-5f;                 // ~-100 dBFS at the tone bin
        const float kBurst = 1e-2f;                 // ~ -40 dBFS at the tone bin
        const float kAmps[kDepth] = {kQuiet, kQuiet, kQuiet, kBurst};

        Hl2Spectrum ref(N);                         // averaging off: one periodogram
        Hl2Spectrum avg(N);
        avg.setAverageFrames(kDepth);
        check(avg.averageFrames() == kDepth, "the depth is what was asked for");
        check(ref.averageFrames() == 1, "averaging is off by default");

        const double alpha = 1.0 / kDepth;
        double powerEma = 0.0;                      // the correct estimator
        double dbEma = 0.0;                         // the one this replaces
        std::vector<float> refBins, avgBins;
        for (int f = 0; f < kDepth; ++f) {
            const auto frame = tone(N, kBin, kAmps[f]);
            check(ref.process(frame, refBins) == 1, "reference emits one frame");
            check(avg.process(frame, avgBins) == 1,
                  "averaging does not divide the display cadence by the depth");
            const double singleDb = refBins[peakBin];
            const double power = std::pow(10.0, singleDb / 10.0);
            powerEma = (f == 0) ? power    : powerEma + alpha * (power - powerEma);
            dbEma    = (f == 0) ? singleDb : dbEma + alpha * (singleDb - dbEma);
        }

        // THE PROPERTY: what the class emits is the log of the averaged power,
        // not the average of the logs.
        check(std::fabs(avgBins[peakBin] - 10.0 * std::log10(powerEma)) < 0.02,
              "the emitted bin is 10*log10 of the power average");

        // POSITIVE CONTROL 1 · the two estimators really do disagree on this
        // fixture, so the assertion above is discriminating and not merely
        // arithmetic that holds either way. The RFC's worked example puts the
        // gap at 39 dB of a 60 dB event for exactly this shape.
        check(std::fabs(avgBins[peakBin] - dbEma) > 30.0,
              "averaging in dB would have thrown away tens of dB of the burst");
        check(avgBins[peakBin] > dbEma,
              "...and it throws it away DOWNWARD: the geometric mean sits below");

        // POSITIVE CONTROL 2 · and the case that hid the defect. With a steady
        // amplitude the two estimators agree exactly, so a fixture built from
        // unvarying frames — a test tone, which is what a bench check reaches
        // for — cannot tell them apart. This is what the burst above is for.
        {
            Hl2Spectrum steadyRef(N), steadyAvg(N);
            steadyAvg.setAverageFrames(kDepth);
            std::vector<float> sr, sa;
            for (int f = 0; f < kDepth; ++f) {
                const auto frame = tone(N, kBin, 0.25f);
                steadyRef.process(frame, sr);
                steadyAvg.process(frame, sa);
            }
            check(std::fabs(sa[peakBin] - sr[peakBin]) < 0.02,
                  "on a steady carrier an average of any depth changes nothing");
        }

        // POSITIVE CONTROL 3 · depth 1 is a pass-through, so the restructure
        // into the power domain cannot have moved the absolute level. Both
        // instances see the same frame; the averaged one must report the
        // reference bit for bit.
        {
            Hl2Spectrum plain(N);
            plain.setAverageFrames(1);
            Hl2Spectrum other(N);
            std::vector<float> a, b;
            const auto frame = tone(N, kBin, 0.5f);
            plain.process(frame, a);
            other.process(frame, b);
            check(a == b, "depth 1 emits exactly the un-averaged spectrum");
            // AND THE dB SCALE ITSELF, which is the thing this change could
            // plausibly get wrong: 20*log10(magnitude) and 10*log10(power) are
            // the same scale, 10*log10(magnitude) and 20*log10(power) are not,
            // and every one of the four reads plausibly on a single trace.
            // Halving the amplitude must move the bin by exactly 20*log10(2);
            // a factor-of-two error in the scale would make it 3.01 or 12.04.
            // Stated as a RATIO because the absolute offset depends on the
            // window's coherent gain, which is not what is under test.
            std::vector<float> softer;
            Hl2Spectrum half4(N);
            half4.process(tone(N, kBin, 0.25f), softer);
            check(std::fabs((static_cast<double>(a[peakBin])
                             - static_cast<double>(softer[peakBin]))
                            - 20.0 * std::log10(2.0)) < 0.01,
                  "halving the amplitude moves the bin by 6.02 dB, not 3.01 or 12.04");
        }

        // A depth change drops the state rather than reinterpreting it: an
        // exponential built at one alpha is not a state at another.
        // The state carries the burst at this point, ~54 dB above the quiet
        // frame that follows, so a state that survived the change would be
        // unmistakable.
        avg.setAverageFrames(2);
        const auto quiet = tone(N, kBin, kQuiet);
        check(avg.process(quiet, avgBins) == 1, "a frame after the depth change");
        check(ref.process(quiet, refBins) == 1, "and the reference for it");
        check(std::fabs(avgBins[peakBin] - refBins[peakBin]) < 0.02,
              "the first frame after a depth change is taken whole, not blended "
              "into the old estimator's state");

        // A TRANSPORT GAP DOES NOT DROP THE AVERAGE, and that is a decision
        // rather than an oversight -- so it gets an assertion instead of only a
        // paragraph. reset()'s caller is a packet-loss gap; the frames already
        // integrated still measure the same spectrum, and dropping on every
        // burst of loss would make the display oscillate between averaged and
        // raw. A geometry change is the case that MUST drop, and it does,
        // because Hl2RxDsp::configure reconstructs the object outright.
        //
        // Built so it cannot pass vacuously: the state is loaded with the LOUD
        // frame, reset() is called, and a QUIET frame follows. If reset() threw
        // the average away, that quiet frame would be taken whole and read like
        // the reference. It has to stay pulled up toward the burst instead.
        avg.setAverageFrames(kDepth);
        for (int i = 0; i < 6; ++i) {
            avg.process(tone(N, kBin, kBurst), avgBins);
        }
        const double beforeGap = avgBins[peakBin];
        check(avg.reset() == 0,
              "positive control: the accumulator is empty on a frame boundary, "
              "so this reset discards no partial frame and tests only the "
              "averaging state");
        Hl2Spectrum wholeFrame(N);
        std::vector<float> wholeBins;
        wholeFrame.process(tone(N, kBin, kQuiet), wholeBins);
        check(avg.process(tone(N, kBin, kQuiet), avgBins) == 1,
              "a frame after the gap");
        check(avgBins[peakBin] > wholeBins[peakBin] + 20.0,
              "the averaging state SURVIVES a transport gap: the quiet frame "
              "after reset() is still pulled far above an unaveraged one");
        check(avgBins[peakBin] < beforeGap,
              "...and it is a blend rather than a freeze -- the quiet frame "
              "still moves the estimator down");
    }

    // ---- the FFTW planner lock, which merged with nothing covering it ----
    //
    // FFTW's planner is process-global and NOT thread-safe. Hl2Backend::
    // beginDspSetup() constructs an Hl2Spectrum on its worker while
    // WdspChannel::open() plans, allocates and frees through FFTW on another
    // thread -- on EVERY HL2 connect. #5424 serialised it by taking
    // WdspChannel::fftwSetupLock() in this class's constructor and destructor.
    //
    // It landed untested, and says so: the TSan evidence came from
    // radiomodel_pan_id_mapping_test, one of the eight tests deleted as
    // intermittent, and #5443 records the consequence in its own words --
    // "#5424 ... now merges with nothing covering it, because the test that
    // proved it was one of the eight". The race is not intermittent; only the
    // instrument was. This is that coverage, and it needs no sanitizer: hold
    // the lock and observe that construction and destruction WAIT for it.
    //
    // WHAT THIS DOES AND DOES NOT PROVE. It proves the two call sites take the
    // shared lock, which is the fact that can be deleted by an edit. It does
    // not reproduce the race, and no single-threaded assertion could.
    {
        using namespace std::chrono;

        // POSITIVE CONTROL FIRST, so the blocking assertions below cannot pass
        // merely because constructing an Hl2Spectrum is slow. Unlocked, both
        // construction and destruction are microseconds at N=64.
        const auto t0 = steady_clock::now();
        { Hl2Spectrum warm(N); }
        const auto unlockedMs = duration_cast<milliseconds>(steady_clock::now() - t0).count();
        check(unlockedMs < 100,
              "control: an unguarded construct+destroy is far below the wait "
              "window, so a blocked one is the lock and not the work");

        // The window. Generous against a loaded machine, and 2.5x the bound the
        // control above asserts on the unlocked cost.
        constexpr auto kWindow = milliseconds(250);

        // ---- constructor ----
        {
            std::atomic<bool> entered{false};
            std::atomic<bool> constructed{false};
            std::unique_ptr<Hl2Spectrum> spec;

            auto held = WdspChannel::fftwSetupLock();
            std::thread t([&] {
                entered.store(true, std::memory_order_release);
                spec = std::make_unique<Hl2Spectrum>(N);
                constructed.store(true, std::memory_order_release);
            });
            while (!entered.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            std::this_thread::sleep_for(kWindow);
            check(!constructed.load(std::memory_order_acquire),
                  "Hl2Spectrum's constructor BLOCKS while fftwSetupLock() is "
                  "held on another thread (#5424, #5443's defect 1)");
            held.unlock();
            t.join();
            check(constructed.load(std::memory_order_acquire) && spec != nullptr,
                  "and completes as soon as the lock is released");
        }

        // ---- destructor ----
        //
        // Its own assertion rather than a corollary: #5424 guards the frees as
        // well as the plan because the edge TSan named was a free against an
        // allocation, so a patch that kept only the constructor's lock would
        // still leave the reported half open.
        {
            auto spec = std::make_unique<Hl2Spectrum>(N);
            std::atomic<bool> entered{false};
            std::atomic<bool> destroyed{false};

            auto held = WdspChannel::fftwSetupLock();
            std::thread t([&] {
                entered.store(true, std::memory_order_release);
                spec.reset();
                destroyed.store(true, std::memory_order_release);
            });
            while (!entered.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            std::this_thread::sleep_for(kWindow);
            check(!destroyed.load(std::memory_order_acquire),
                  "~Hl2Spectrum BLOCKS on the same lock -- the teardown half "
                  "of the edge, guarded for its own reason");
            held.unlock();
            t.join();
            check(destroyed.load(std::memory_order_acquire) && spec == nullptr,
                  "and completes as soon as the lock is released");
        }
    }

    centreBinTests();

    if (g_failures == 0)
        std::fprintf(stderr, "hl2_spectrum_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
