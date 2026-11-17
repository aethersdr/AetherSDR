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
};

// What the stream showed: per-frame centre-bin prominence, and the power
// average of every frame, for a floor and a peak that do not jitter.
struct Seen {
    std::vector<double> centreOverFloorDb;    // one per frame, against the averaged floor
    std::vector<double> meanPower;            // per bin, averaged over the frames
    int frames = 0;
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
            double seconds, bool collect, double capFps = 0.0)
{
    Seen seen;
    seen.meanPower.assign(kStreamN, 0.0);
    std::vector<std::vector<float>> frames;
    std::vector<std::complex<float>> block(kEp6Block);
    std::vector<float> bins;
    const std::uint64_t begin = n;
    const std::uint64_t end = n + static_cast<std::uint64_t>(seconds * kStreamFs);
    const std::uint64_t dueEvery = capFps > 0.0 ? static_cast<std::uint64_t>(kStreamFs / capFps) : 0;
    std::uint64_t lastFrame = n;
    while (n < end) {
        for (int i = 0; i < kEp6Block; ++i, ++n) {
            const double ph = 2.0 * kPi * sig.toneHz * static_cast<double>(n) / kStreamFs;
            const std::complex<double> x = sig.toneAmp * std::complex<double>(std::cos(ph), std::sin(ph))
                                         + sig.dc + noise.gaussian(kNoiseSigma);
            block[static_cast<std::size_t>(i)] = {static_cast<float>(x.real()), static_cast<float>(x.imag())};
        }
        if (dueEvery > 0 && n - lastFrame < dueEvery) {
            spec.accumulate(block);
            continue;
        }
        if (spec.process(block, bins) > 0) {
            lastFrame = n;
            if (collect && n - begin >= static_cast<std::uint64_t>(kStreamN + kEp6Block)) {
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
            // strong tone appears (the HL2's TX leaking into RX1).
            {
                Hl2Spectrum spec(kStreamN, kStreamFs);
                Noise noise;
                std::uint64_t n = 0;
                stream(spec, noise, n, {}, 1.0, false, capFps);
                const Seen keyed = stream(spec, noise, n, {hz, 0.5, {}}, 1.0, true, capFps);
                const double peakDb = 10.0 * std::log10(*std::max_element(keyed.meanPower.begin(),
                                                                           keyed.meanPower.end()));
                std::fprintf(stderr, "  off-bin %+.0f Hz cap %.0f fps, keyed: tone %.1f dB over floor, "
                             "centre %.1f dB (mean), %.1f dB (worst of %d frames)\n",
                             hz, capFps, peakDb - floorDbOf(keyed.meanPower), centreOverFloorDb(keyed),
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
