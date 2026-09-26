// aetherd HL2 Phase 1a — Hl2Spectrum unit test. Feeds synthetic IQ through the
// FFT panadapter path and checks: a complex tone peaks at the expected
// fftshifted bin, DC lands at the centre bin, partial frames accumulate across
// calls, and a large DC offset (the direct-sampling ADC bias) is removed so it
// does not swamp a real tone — mirroring the tools/hl2/spectrum.py behavior.

#include "core/backends/hl2/Hl2Spectrum.h"
#include "core/dsp/WdspChannel.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#include <cmath>
#include <complex>
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

    if (g_failures == 0)
        std::fprintf(stderr, "hl2_spectrum_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
