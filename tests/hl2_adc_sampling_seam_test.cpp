// aetherd HL2 — the ADC pairing's slice-sampling gate, at the seam it guards.
//
// hl2_adc_pairing_test pins adcPairing() as a truth table over six arguments.
// A truth table cannot see this bug, because the bug is not in the function: it
// is in the ARGUMENT, and specifically in the moment the argument changes
// relative to the moment the thing it describes changes.
//
// Hl2Backend sets m_keyed and m_txMonitor SYNCHRONOUSLY and delivers the
// setAudioMuted they imply to the DSP thread over a QUEUED connection. At
// key-down that is harmless: the flag says "not sampling" slightly before
// Hl2RxDsp stops, and an input whose job is to withhold an assertion may
// safely be early. At key-UP it inverts — the flag says "sampling" before
// Hl2RxDsp has unmuted or produced a single new peak, and if the peak frozen
// at key-down is still inside kSliceStaleMs (a short key-down, or the TX audio
// monitor switched on mid-transmission) the age gate is open too. Both gates
// open, nothing sampling, and the pairing asserts a causal sentence about a
// band the operator is not listening to. That is precisely the failure the
// synchronous input was added to prevent, arriving through the other door.
//
// SliceSamplingGate closes it by comparing timestamps instead of predicting:
// Hl2RxDsp writes m_adcPeakAtNs only on the !m_audioMuted path, so a peak
// stamped after the resume was REQUESTED is proof the chain is sampling again.
//
// This test reproduces the asynchrony rather than describing it. Hl2RxDsp lives
// on this thread, setAudioMuted is delivered with Qt::QueuedConnection exactly
// as Hl2Backend delivers it, and the queued call therefore runs only when the
// event loop is spun — which is what lets the test stand in the window between
// "asked" and "applied" and look at what each input says there.

#include "core/backends/hl2/Hl2AdcPairing.h"
#include "core/backends/hl2/Hl2RxDsp.h"

#include <QCoreApplication>

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
static constexpr int kFs = 48000;
static constexpr std::size_t kBlock = 1024;

// A tone well clear of the meter's silence sentinel, so every block WDSP
// actually runs produces a real peak and a stamp to go with it.
static void feedOneBlock(Hl2RxDsp& dsp, double& phase)
{
    std::vector<std::complex<float>> block(kBlock);
    for (std::size_t n = 0; n < kBlock; ++n) {
        block[n] = 0.3f * std::complex<float>(static_cast<float>(std::cos(phase)),
                                              static_cast<float>(-std::sin(phase)));
        phase += 2.0 * kPi * 1000.0 / kFs;
    }
    dsp.processIqBlock(std::move(block));
}

// FED IS NOT PROCESSED. Hl2RxDsp::processIqBlock runs WDSP only once its input
// buffer holds a full dspBlockSize, and WdspChannel returns Underrun while the
// pipeline fills — the peak is stamped on the output side, so the first few
// blocks in stamp nothing. Every "a new sample lands" step below therefore
// feeds until the stamp moves rather than assuming one block is one sample.
// The bound is what keeps a chain that has genuinely stopped from hanging the
// test: it returns false and the caller asserts on that.
static constexpr int kMaxBlocksForOneSample = 16;
// Comfortably more than Hl2RxDsp's post-unmute S-meter settle window, which is
// ~15 blocks at this rate and block size. DELIBERATELY NOT the same arithmetic
// the implementation uses: a test that recomputes the window it is measuring
// agrees with itself whatever the window is actually doing. This only has to be
// "enough blocks that the meter has had its say"; what is asserted is the VALUE
// that comes out, which no amount of extra feeding can fake.
static constexpr int kBlocksPastSettle = 40;
// An over, in blocks: 235 blocks of 1024 samples at 48 kHz is ~5 s, which is a
// short over and already ~50 time constants of the meter's 0.1 s average. If
// the settle window were sized wrong this is long enough to bury the needle.
static constexpr int kBlocksPerOver = 235;
// How close the first reading published after an over has to be to the last one
// published before it. The tone is unchanged across the over, so the honest
// answer is "the same", and three decibels is slack rather than a target: the
// settle window is three time constants of the meter's average, but the RXA
// bandpass's group delay spends about 85 ms of that still carrying the mute's
// zeros, which leaves the average a little over two taus and ~0.5 dB of
// residual. The measured gap is printed next to this check so a drift toward
// the bound is visible rather than merely still passing. Before the settle
// window existed the gap was ~214 dB, so this bound is nowhere near tight
// enough to be the thing that fails first if the fix is wrong.
static constexpr float kMeterResumeToleranceDb = 3.0f;
static bool feedUntilNewPeak(Hl2RxDsp& dsp, double& phase, std::int64_t previousStamp)
{
    for (int i = 0; i < kMaxBlocksForOneSample; ++i) {
        feedOneBlock(dsp, phase);
        if (dsp.adcPeakObservedAtNs() > previousStamp) {
            return true;
        }
    }
    return false;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    Hl2RxDsp dsp;
    Hl2RxDsp::Config cfg;
    cfg.inputSampleRateHz = kFs;
    cfg.audioSampleRateHz = kFs;
    cfg.dspBlockSize = static_cast<int>(kBlock);
    cfg.fftSize = 256;
    cfg.mode = WdspChannel::Mode::Usb;
    cfg.blockForOutput = true;
    std::string err;
    check(dsp.configure(cfg, &err), err.empty() ? "Hl2RxDsp configures" : err.c_str());

    double phase = 0.0;
    SliceSamplingGate gate;

    // THE S-METER TAP, WATCHED FROM ONE CONNECTION FOR THE WHOLE TEST.
    //
    // Every assertion about the meter below reads this recorder, so the
    // negative ones ("nothing was published") and the positive ones ("this
    // value was published") are statements about the same wire. A connection
    // that was never made, a renamed signal or a chain that stopped producing
    // blocks cannot satisfy the negatives while the positives still hold — that
    // is what makes an assertion about an absence mean anything here.
    int meterEmissions = 0;
    float firstMeterDbfs = 0.0f;
    float lastMeterDbfs = 0.0f;
    QObject::connect(&dsp, &Hl2RxDsp::meterUpdate, &dsp, [&](float dbfs) {
        // Re-armed by setting meterEmissions back to 0, which every section
        // below does before the window it cares about.
        if (meterEmissions == 0)
            firstMeterDbfs = dbfs;
        ++meterEmissions;
        lastMeterDbfs = dbfs;
    });

    // The backend's two call sites, reproduced. `requested` is the predicted
    // input as it stood before this gate existed — `!(keyed && !monitor)` — and
    // the test keeps it alongside the gate so the assertions below can show the
    // two disagreeing, which is the whole point.
    bool keyed = false;
    bool monitor = false;
    const auto requested = [&] { return !(keyed && !monitor); };
    const auto queueMute = [&] {
        gate.setRequested(requested(), steadyNowNs());
        QMetaObject::invokeMethod(&dsp, "setAudioMuted", Qt::QueuedConnection,
                                  Q_ARG(bool, !requested()));
    };

    // ── Receiving. A peak exists and the chain is sampling. ──────────────
    check(feedUntilNewPeak(dsp, phase, 0), "a processed block stamps the peak");
    check(gate.applied(dsp.adcPeakObservedAtNs()),
          "an untouched gate admits a live reading — never keyed means never interrupted");

    // ── Key down. The flag leads the mute, and leading is safe here. ─────
    keyed = true;
    queueMute();
    const std::int64_t atKeyDown = dsp.adcPeakObservedAtNs();
    check(!requested(), "the predicted input shuts at key-down");
    check(!gate.applied(dsp.adcPeakObservedAtNs()), "so does the gate");
    // The mute has NOT been delivered yet, so the DSP is still sampling and
    // still stamping. Both inputs already say it is not, which is the early
    // direction — an omission, not an assertion.
    check(feedUntilNewPeak(dsp, phase, atKeyDown),
          "the DSP is still sampling in the window before the queued mute lands");
    check(!gate.applied(dsp.adcPeakObservedAtNs()),
          "the gate stays shut through that window");

    // Deliver it. From here the stamp must freeze.
    app.processEvents();
    const std::int64_t frozen = dsp.adcPeakObservedAtNs();
    check(!feedUntilNewPeak(dsp, phase, frozen),
          "a muted chain holds its peak and its stamp for as long as it is fed");

    // ── AND THE S-METER MUST FREEZE WITH IT ──────────────────────────────
    //
    // The muted branch of processIqBlock clocks the channel with literal zeros
    // on purpose. The ADC peak above is guarded against that; the S-meter tap
    // was not, so `avg` measured the silence this code fed it and published it
    // as a level. Because the mute is the TRANSMIT mute, that dropped the
    // needle to the floor on every key-down.
    //
    // Asserted on the same muted chain the peak assertion just used, so the two
    // cannot disagree about what "muted" meant.
    //
    // THE POSITIVE CONTROL FOR THIS ABSENCE IS NOT HERE — it is the unkey-edge
    // section at the end of this file, which unmutes the same chain through the
    // same recorder and requires the emissions to start. It used to sit inline,
    // and ten9876 caught what that cost: the sixteen unmuted blocks it fed
    // advanced adcPeakObservedAtNs() past `frozen`, so the "the chain resumes
    // sampling" assertion further down became true from its first block whether
    // or not the unmute had landed. Re-capturing `frozen` afterwards would have
    // repaired that assertion; moving the control out of this window instead
    // means nothing has to be repaired, and the mute never gets released inside
    // a stretch of the test whose whole subject is what a MUTED chain does.
    {
        const int emissionsBefore = meterEmissions;
        for (int i = 0; i < kMaxBlocksForOneSample; ++i) {
            feedOneBlock(dsp, phase);
        }
        app.processEvents();
        check(meterEmissions == emissionsBefore,
              "a muted chain publishes NO S-meter level — it is not a signal, "
              "it is the silence we clocked in");
    }

    // ── Key up, short. THE BUG. ──────────────────────────────────────────
    //
    // The held stamp is milliseconds old — this test keys up immediately, which
    // is the short key-down that makes the age gate useless. Nothing about the
    // DSP has changed: it is still muted, and will stay muted until the event
    // loop runs.
    keyed = false;
    queueMute();
    const std::int64_t ago = (steadyNowNs() - dsp.adcPeakObservedAtNs()) / 1'000'000;
    check(ago <= kSliceStaleMs,
          "the held peak is still FRESH BY AGE at key-up, so the age gate is open");
    // What the predicted input says here is the defect, asserted as a fact so
    // that a future simplification back to `!(keyed && !monitor)` fails loudly
    // instead of quietly restoring the inverted verdict.
    check(requested(),
          "the predicted input claims sampling the instant the key is released");
    check(!gate.applied(dsp.adcPeakObservedAtNs()),
          "the gate does not — no peak has been stamped since the resume was asked for");
    // And that is the difference between an assertion and an omission.
    const double peak = *dsp.adcPeakDbfs();
    check(adcPairing(true, peak, /*current=*/true, /*sampling=*/requested(), true, true)
              != AdcPairing::Unknown,
          "predicted: a causal verdict from a value nothing is sampling");
    check(adcPairing(true, peak, /*current=*/true,
                     /*sampling=*/gate.applied(dsp.adcPeakObservedAtNs()), true, true)
              == AdcPairing::Unknown,
          "gated: Unknown until the chain has actually resumed");

    // ── The unmute lands. One block later the pairing is a sentence again. ─
    app.processEvents();
    check(!gate.applied(dsp.adcPeakObservedAtNs()),
          "applying the unmute is not itself a sample");
    check(feedUntilNewPeak(dsp, phase, frozen), "the chain resumes sampling");
    check(gate.applied(dsp.adcPeakObservedAtNs()),
          "the first post-unmute peak reopens the gate");
    check(adcPairing(true, *dsp.adcPeakDbfs(), true,
                     gate.applied(dsp.adcPeakObservedAtNs()), true, true)
              != AdcPairing::Unknown,
          "and the verdict comes back");

    // ── The monitor path, which is the same edge reached differently. ─────
    //
    // Enabling the TX audio monitor mid-transmission asks a muted chain to
    // resume without the key ever coming up. setTxAudioMonitor sets m_txMonitor
    // synchronously and queues the unmute, so it has the identical window.
    keyed = true;
    monitor = false;
    queueMute();
    app.processEvents();
    const std::int64_t heldUnderMonitorOff = dsp.adcPeakObservedAtNs();
    check(!feedUntilNewPeak(dsp, phase, heldUnderMonitorOff),
          "keyed with the monitor off, the chain is muted and holds");
    monitor = true;
    queueMute();
    check(requested(), "the predicted input claims sampling as soon as the monitor is on");
    check(!gate.applied(dsp.adcPeakObservedAtNs()),
          "the gate waits for a sample taken after the monitor was switched on");
    app.processEvents();
    check(feedUntilNewPeak(dsp, phase, heldUnderMonitorOff),
          "with the monitor applied the chain samples through the transmission");
    check(gate.applied(dsp.adcPeakObservedAtNs()),
          "and the gate reopens on that sample");

    // ── Only the edge moves the bar. ─────────────────────────────────────
    //
    // healthSnapshot runs every 500 ms and setTxAudioMonitor may be re-asserted
    // by a restore; a gate that re-stamped on every call would invalidate a
    // perfectly good reading each time and never report a pairing at all.
    {
        SliceSamplingGate g;
        g.setRequested(false, 1000);
        g.setRequested(true, 2000);
        check(!g.applied(1500), "a pre-resume stamp is refused");
        check(g.applied(2500), "a post-resume stamp is admitted");
        g.setRequested(true, 3000);   // same state, re-asserted
        check(g.applied(2500), "re-asserting an unchanged request does not push the bar");
    }
    // Never sampled is not sampling, whatever the request says.
    {
        SliceSamplingGate g;
        check(!g.applied(0), "a chain that has never produced a block is not sampling");
    }

    // ── THE UNKEY EDGE: what the mute leaves BEHIND inside WDSP ──────────
    //
    // Not publishing while muted is only half of it, and on its own it moves
    // the artefact rather than removing it. WDSP's S_AV is an EMA over
    // I*I + Q*Q with a 0.100 s time constant (meter.c, tau from RXA.c), it
    // integrated every zero Hl2RxDsp clocked in for the whole over, and there
    // is no flush that helps — flush_meter sets the accumulator to 0, which is
    // the -400 dB floor. So the first block after the unmute reads the silence
    // at full depth, and the needle dives on UNKEY instead of on key-down.
    // ten9876 measured that on this branch's head: -224.5 dBFS against a
    // pre-mute -10.5, which Hl2Backend's attack/decay EMA turns into a ~32 dB
    // step at the instant of unkey and ~300 ms of climb back.
    //
    // This is the assertion that pins the whole fix, because it is the only one
    // stated in the units an operator sees: the FIRST number published after an
    // over, against the last number published before it, with the input signal
    // unchanged across the over. It cannot be satisfied by a guard alone and it
    // cannot be satisfied by suppressing everything — the positive control one
    // line above it requires the publishing to have restarted.
    //
    // It is also the positive control the muted-chain assertion earlier in this
    // file needs: same dsp, same recorder, same connection.
    {
        // Back on air, and let the meter settle onto the tone.
        keyed = false;
        monitor = false;
        queueMute();
        app.processEvents();
        meterEmissions = 0;
        for (int i = 0; i < kBlocksPastSettle; ++i)
            feedOneBlock(dsp, phase);
        app.processEvents();
        check(meterEmissions > 0,
              "an UNMUTED chain publishes — so every 'nothing was published' in "
              "this file is a guard firing, not a dead connection");
        const float beforeTheOver = lastMeterDbfs;

        // Key down and hold it for an over.
        keyed = true;
        queueMute();
        app.processEvents();
        meterEmissions = 0;
        for (int i = 0; i < kBlocksPerOver; ++i)
            feedOneBlock(dsp, phase);
        app.processEvents();
        check(meterEmissions == 0,
              "nothing is published for the length of the over, however long it is");

        // Unkey. The tone never went away; only our own mute did.
        keyed = false;
        queueMute();
        app.processEvents();
        meterEmissions = 0;
        for (int i = 0; i < kBlocksPastSettle; ++i)
            feedOneBlock(dsp, phase);
        app.processEvents();
        check(meterEmissions > 0, "and publishing resumes after the over");
        // Only once it has resumed: firstMeterDbfs is whatever the recorder
        // last captured, so comparing it after a failed resume would print a
        // reading nothing published and turn one cause into two failures.
        if (meterEmissions > 0) {
            std::printf("  S-meter across an over: %.1f dBFS before, %.1f dBFS on the "
                        "first reading after (%+.1f dB)\n",
                        static_cast<double>(beforeTheOver),
                        static_cast<double>(firstMeterDbfs),
                        static_cast<double>(firstMeterDbfs - beforeTheOver));
            check(std::fabs(firstMeterDbfs - beforeTheOver) <= kMeterResumeToleranceDb,
                  "THE FIRST READING AFTER AN OVER IS A READING OF THE BAND, not of "
                  "the silence we clocked into WDSP's average while we transmitted");
        }
    }

    if (g_failures == 0) {
        std::printf("hl2_adc_sampling_seam_test: OK\n");
        return 0;
    }
    std::fprintf(stderr, "hl2_adc_sampling_seam_test: %d failure(s)\n", g_failures);
    return 1;
}
