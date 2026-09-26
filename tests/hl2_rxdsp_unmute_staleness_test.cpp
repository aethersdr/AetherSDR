// aetherd HL2 -- #5498: does pre-mute audio emerge after an unmute, and for how
// many milliseconds? Today that question is answered by listening. This test
// answers it with a number, offline, with no radio and no antenna.
//
// WHAT THIS ANSWERS, AND WHAT IT DOES NOT
//
// #5498 asks two things: how long receive audio is LOST after an unkey, and how
// abruptly it returns. THIS TEST MEASURES NEITHER. It measures a third
// quantity -- how long PRE-MUTE content persists across the mute edge, in
// milliseconds, on each side of it. That is "unmute staleness", it is the part
// of #5498 that can be settled offline, and it is all this file claims. A
// dropout is the absence of NEW signal, and there is no new signal here whose
// absence could be timed (see the note on marker 2 below for why not).
//
// THE TWO BUILDS THIS MUST SURVIVE UNCHANGED
//
//   ZERO  -- the behaviour on origin/main today: Hl2RxDsp::processIqBlock's
//            m_audioMuted branch fills m_i/m_q with 0.0f and FALLS THROUGH to
//            WdspChannel::processIq. The RXA chain is clocked, so the
//            overlap-save history flushes DURING the mute.
//   HOLD  -- an UNLANDED candidate (the #5497 triage's fix 2), not present in
//            this tree and not referenced by it: the branch consumes the block,
//            emits a zeroed m_stereo and `continue`s. fexchange2 is never
//            reached and the history freezes.
//
// The two are predicted to put the SAME stale audio in two different places,
// and this test measures BOTH places for marker 1:
//
//   staleDuringMuteMs   -- pre-mute content emerging while still muted
//   staleAfterUnmuteMs  -- pre-mute content emerging after setAudioMuted(false)
//
// HOLD should report ~0 for the first and a real number for the second; ZERO
// the other way round. Note that ZERO's during-mute audio is NOT silent: the
// `emit audioReady(m_stereo)` at the bottom of that loop is not guarded on
// m_audioMuted, and Hl2Backend's relay says so in as many words ("Emitted even
// while keyed"). The speaker mixer drops it; per-slice consumers do not.
//
// WHY THERE ARE TWO MARKERS, AND WHY THE SECOND ONE IS THE POINT
//
// An earlier revision of this fixture fed ZERO IQ during the mute. That made
// setAudioMuted() INERT for the fixture on the current tree: main's muted
// branch substitutes zeros for the input, the unmuted branch copies the input,
// and if the input IS zeros the two branches hand processIq the same samples.
// Deleting both setAudioMuted() calls would have printed the same numbers. The
// instrument could only discriminate against the HOLD build, which is not here.
//
// That is not just a problem for the test's honesty. In a REAL over the input
// during the mute is NOT zeros -- the receiver is still being fed IQ off the
// DDC, and main's zero-fill genuinely does something: it substitutes zeros for
// LIVE INPUT. Feeding zeros is what hid that.
//
// So the mute is now fed a SECOND, distinguishable marker at its own in-passband
// frequency, and the fixture measures two separate things:
//
//   marker 1 (kMarker1Hz, fed BEFORE the mute)  -- how much of what was on the
//       air before the key survives, during the mute and after it. The original
//       question. Unaffected by marker 2: on ZERO the input is overwritten with
//       zeros whatever it was, and on HOLD it is discarded unread.
//   marker 2 (kMarker2Hz, fed DURING the mute)  -- how much of the live input
//       during the over leaks out. Both builds should suppress it, by different
//       mechanisms (ZERO overwrites it before fexchange2; HOLD never calls
//       fexchange2 at all), and a build with NO mute leaks it at full level.
//       This is the column that makes the mute visible on the tree we are on.
//
// Both markers sit inside the USB passband [150, 3000] Hz, ten detector bins
// apart, clear of DC and of both filter skirts. The same matched filter runs at
// each marker's own frequency; leg 3 proves it does not cross-read one as the
// other.
//
// AFTER the unmute the input is zero IQ again, deliberately: feeding a marker
// back would make a splice indistinguishable from a signal. That is also why
// this file cannot time a dropout.
//
// MARKER 2 NEEDS A REFERENCE, SO THERE IS A FOURTH PHASE
//
// "How much of marker 2 leaked" is meaningless without knowing what marker 2
// reads when nothing is suppressing it. A calibration phase after the
// observation feeds marker 2 UNMUTED through the same chain, and its settled
// level is the 0 dB reference for every marker-2 figure. It is also the control
// that stops "marker 2 was suppressed" being confused with "marker 2 never got
// through anything" -- that one IS asserted.
//
// THE AGC IS OFF, ON PURPOSE
//
// Hl2RxDsp::Config defaults to agcMode 3 (medium) with a 39 dB ceiling. Across
// a stretch of zero input WDSP's AGC recovers toward that ceiling and
// re-amplifies exactly the decaying filter tail this test is timing, so the
// -40 dB crossing would be substantially a measurement of the AGC's RECOVERY
// CURVE rather than of the filter's support. The question here is about FILTER
// HISTORY, so the fixture sets cfg.agcMode = 0.
//
// That is a real off and not a slow setting. WDSP's xwcpagc() treats mode 0 as
// an explicit agcOFF: it returns out = fixed_gain * in BEFORE it touches the
// ring buffer, the attack/decay multipliers or the hang state machine, so no
// AGC state advances across the zero-fed stretch and there is nothing to
// recover. The ceiling is inert with the mode off -- SetRXAAGCTop only feeds
// min_volts and slope_constant, neither of which is read on that path -- and is
// pinned in the config only so the config states it.
//
// ONE CONSEQUENCE FOR LEVEL, which is not obvious from the config. With the AGC
// out of the way the remaining gain is NOT just WdspChannel's fixed 10 dB:
// WDSP's RXA patch panel multiplies by a hard-coded 4.0 downstream of it,
// unconditionally -- that gain loop sits outside the stage's own run check, and
// nothing in this tree ever calls SetRXAPanelGain1. So the marker meets about
// +22 dB, not +10, and kMarkerAmplitude is set small enough that this cannot
// drive the demodulated audio into the rail. Controls assert both ends of that:
// not clipping, and not so small that the -40 dB crossing sits in the grass.
// Reasoning of this kind goes stale silently, which is why it is checked.
//
// A SEPARATE ENVELOPE, worth knowing for a test that times a decay: OpenChannel
// installs a one-shot up-slew (10 ms delay, 25 ms raised-cosine ramp) that fires
// on the first non-zero sample after the channel starts. It does NOT re-fire at
// the mute, which never stops the channel -- but it does colour the first ~35 ms
// of marker 1, which is a second reason the first half of each marker feed is
// excluded from that marker's own settled reference.
//
// WHAT THE NUMBER IS NOT COMPARED AGAINST
//
// This test deliberately does NOT assert leg 1 against Hl2RxDsp::kRxFilterTaps /
// kWdspDspSampleRateHz. A test that retypes -- or even re-reads -- the constant
// it is supposed to be measuring agrees with itself while the code it guards is
// wrong. The taps ARE read from the real header, but only to (a) print the
// predicted support alongside the measurement and (b) size the detector's own
// positive control. Leg 1 REPORTS.
//
// ONE MEASUREMENT FIGURE IS ASSERTED, and only one: that marker 2 -- the live
// input during the over -- stays below -20 dB while muted. That is not a
// retyped constant and not a verdict on #5497: BOTH candidate builds satisfy
// it, by different mechanisms. It is asserted because it is the only figure
// here that goes red when the mute itself is deleted, which makes this fixture
// protect the thing it measures. The ZERO/HOLD distinction -- which side of
// the edge marker 1 lands on, and whether marker 2 reads -29.8 dB or -300 --
// remains unasserted on purpose. See the check beside the marker-2 table.
//
// WHAT TO EXPECT IF A BUILD SPLICES
//
// kRxFilterTaps is the FIR length handed to RXASetNC (WdspChannel::open), so the
// frozen history spans kRxFilterTaps samples at kWdspDspSampleRateHz -- 170.667
// ms at the current values. But a windowed-sinc bandpass has its energy
// CONCENTRATED near tap nc/2, so feeding zeros into a full history does not
// necessarily produce a 170 ms rectangle: the output may hold near full
// amplitude for roughly the filter's group delay (~85 ms) and then fall away.
// That is why the report below is an ENERGY PROFILE with three threshold
// crossings, not one number: "how long is the splice" has a different answer at
// -6 dB than at -40 dB, and the shape is the finding. With the AGC out of the
// loop this prediction is about the filter and nothing else.
//
// THE ONE UNCERTAINTY THE CONTROLS DO NOT CALIBRATE
//
// Audio blocks are attributed to a phase by a flag set around the feed, so an
// output block that lags its input block by one WdspChannel pass is credited to
// the wrong side of a mute edge. That is bounded by one block -- 21.333 ms at
// dspBlockSize 1024 and 48 kHz, printed below -- and it applies to the phase
// BOUNDARY, not to the decay inside a phase. Legs 2 and 3 never touch the DSP,
// so they cannot calibrate it. Read every figure below as +/- one block.
//
// THE DETECTOR
//
// A sliding Hann-windowed one-bin DFT at a given frequency -- a Goertzel by
// another name. It is a matched filter for that tone and nothing else:
// broadband energy, DC and any other tone are rejected, so "the marker is still
// coming out" cannot be confused with "something is coming out". The same
// function runs over every leg at both marker frequencies, which is what makes
// leg 1's numbers mean something in either build.

#include "core/backends/hl2/Hl2RxDsp.h"
#include "core/backends/hl2/MetisProtocol.h"   // kEp6BlockSamples (EP6 block size)

#include <QCoreApplication>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdio>
#include <span>
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

// ---------------------------------------------------------------------------
// The detector
// ---------------------------------------------------------------------------

// Window geometry. 10 ms of Hann at 48 kHz is 100 Hz per bin -- narrow enough
// that the two markers, 1000 Hz apart, are ten bins apart and each is buried in
// the other's Hann sidelobe roll-off (leg 3 measures that) -- stepped 1 ms at a
// time so the crossing is resolved finer than one audio block.
static constexpr int kWindowMs = 10;
static constexpr int kHopMs = 1;

// A crossing is only accepted when the tone stays below threshold for this many
// consecutive windows: one whole window length of hop advance. See
// staleMilliseconds().
static constexpr int kBelowRunWindows = kWindowMs / kHopMs;

// Amplitude of `toneHz` in each sliding window, normalised so a pure tone of
// amplitude A reads A. Window k spans samples [k*hop, k*hop+win).
static std::vector<double> toneAmplitudeProfile(std::span<const float> x,
                                                double toneHz, int fs,
                                                int win, int hop)
{
    std::vector<double> out;
    if (win <= 1 || hop <= 0 || fs <= 0) return out;
    if (x.size() < static_cast<std::size_t>(win)) return out;

    std::vector<double> wc(static_cast<std::size_t>(win));
    std::vector<double> ws(static_cast<std::size_t>(win));
    double wsum = 0.0;
    const double k = 2.0 * kPi * toneHz / static_cast<double>(fs);
    for (int n = 0; n < win; ++n) {
        const double w = 0.5 * (1.0 - std::cos(2.0 * kPi * static_cast<double>(n)
                                               / static_cast<double>(win - 1)));
        wsum += w;
        wc[static_cast<std::size_t>(n)] = w * std::cos(k * static_cast<double>(n));
        ws[static_cast<std::size_t>(n)] = w * std::sin(k * static_cast<double>(n));
    }
    if (wsum <= 0.0) return out;

    out.reserve((x.size() - static_cast<std::size_t>(win)) / static_cast<std::size_t>(hop) + 1);
    for (std::size_t off = 0; off + static_cast<std::size_t>(win) <= x.size();
         off += static_cast<std::size_t>(hop)) {
        double re = 0.0, im = 0.0;
        for (int n = 0; n < win; ++n) {
            const double s = static_cast<double>(x[off + static_cast<std::size_t>(n)]);
            re += s * wc[static_cast<std::size_t>(n)];
            im -= s * ws[static_cast<std::size_t>(n)];
        }
        // Coherent gain of the window is wsum/2 for a real tone.
        out.push_back(2.0 * std::hypot(re, im) / wsum);
    }
    return out;
}

static double amplitudeDb(double amp, double refAmp)
{
    if (refAmp <= 0.0) return -300.0;
    const double r = amp / refAmp;
    if (r < 1e-15) return -300.0;
    return 20.0 * std::log10(r);
}

// Centre time, in ms, of sliding window `index`.
static double windowCentreMs(std::size_t index, int fs, int win, int hop)
{
    const double startSample = static_cast<double>(index) * static_cast<double>(hop);
    return 1000.0 * (startSample + 0.5 * static_cast<double>(win))
           / static_cast<double>(fs);
}

// Does the tone stay below `thrDb` for `needBelow` windows starting at `i`?
//
// A RUN THAT CANNOT COMPLETE IS NOT A RUN. An earlier revision clamped `end` to
// the profile size, which waived the anti-dip guard for the last
// `needBelow - 1` windows: a single dip there counted as a full run, so
// staleMilliseconds() reported a crossing with `reachedEnd = false` and
// heldCheck() read a tone that was present throughout as having broken. The
// guard exists precisely to survive that class of transient, so it must not
// switch itself off where the evidence runs out. Refusing the short run lets
// staleMilliseconds() fall through to its `reachedEnd = true` tail, which is
// the honest answer for a tone that held to the end of the capture: a lower
// bound, labelled as one.
static bool belowRunAt(const std::vector<double>& profile, std::size_t i,
                       double refAmp, double thrDb, int needBelow)
{
    const std::size_t end = i + static_cast<std::size_t>(needBelow);
    if (end > profile.size()) return false;
    for (std::size_t k = i; k < end; ++k)
        if (amplitudeDb(profile[k], refAmp) >= thrDb) return false;
    return true;
}

// How many milliseconds from the START of `x` does the tone stay at or above
// `thrDb` relative to `refAmp`? Measured at window centres, linearly
// interpolated in dB across the crossing. Returns 0 when the profile is already
// below threshold at the start. `reachedEnd` reports that it never fell below,
// which makes the answer a lower bound rather than a measurement.
//
// THE CROSSING HAS TO STICK. An earlier revision returned at the FIRST window
// below threshold, so a single dip inside a splice -- a WDSP block seam, a
// DC-blocker transient -- truncated the reading and could fail the positive
// control for a reason that has nothing to do with the splice length. The
// crossing is now only accepted when the tone stays below threshold for
// `needBelow` consecutive windows. At a genuine edge every later window is
// below, so the run costs nothing there and the reported time is unchanged; a
// transient has to last longer than about 17 ms to end the measurement. The
// price is that a genuine RE-appearance shorter than a window is not resolved,
// which is not a question this test asks.
static double staleMilliseconds(const std::vector<double>& profile,
                                double refAmp, double thrDb,
                                int fs, int win, int hop,
                                bool* reachedEnd = nullptr,
                                int needBelow = kBelowRunWindows)
{
    if (reachedEnd) *reachedEnd = false;
    if (profile.empty()) return 0.0;

    if (belowRunAt(profile, 0, refAmp, thrDb, needBelow)) return 0.0;

    for (std::size_t i = 1; i < profile.size(); ++i) {
        if (!belowRunAt(profile, i, refAmp, thrDb, needBelow)) continue;
        const double prevDb = amplitudeDb(profile[i - 1], refAmp);
        const double db = amplitudeDb(profile[i], refAmp);
        const double t0 = windowCentreMs(i - 1, fs, win, hop);
        const double t1 = windowCentreMs(i, fs, win, hop);
        const double span = prevDb - db;
        // Clamped: profile[i-1] can itself be a dip that failed to start a run,
        // in which case the true crossing is earlier and t0 is the conservative
        // answer.
        const double frac = span > 0.0
                                ? std::clamp((prevDb - thrDb) / span, 0.0, 1.0)
                                : 0.0;
        return t0 + frac * (t1 - t0);
    }
    if (reachedEnd) *reachedEnd = true;
    return windowCentreMs(profile.size() - 1, fs, win, hop);
}

// Settled amplitude of a profile: the median of its final quarter. Median, not
// max -- a peak-hold would inherit any single-window artefact as the reference
// every other measurement is quoted against.
static double settledAmplitude(const std::vector<double>& profile)
{
    if (profile.empty()) return 0.0;
    const std::size_t from = profile.size() - std::max<std::size_t>(1, profile.size() / 4);
    std::vector<double> tail(profile.begin() + static_cast<std::ptrdiff_t>(from), profile.end());
    std::sort(tail.begin(), tail.end());
    return tail[tail.size() / 2];
}

// Highest window in a profile, in dB relative to `ref`, and when it occurred.
static double peakDb(const std::vector<double>& profile, double ref,
                     int fs, int win, int hop, double* atMs = nullptr)
{
    double worst = -300.0;
    std::size_t at = 0;
    for (std::size_t i = 0; i < profile.size(); ++i) {
        const double db = amplitudeDb(profile[i], ref);
        if (db > worst) { worst = db; at = i; }
    }
    if (atMs) *atMs = profile.empty() ? 0.0 : windowCentreMs(at, fs, win, hop);
    return worst;
}

// Total milliseconds' worth of windows at or above `thrDb` ANYWHERE in the
// profile.
//
// THIS IS THE RIGHT QUESTION FOR MARKER 2 AND THE WRONG ONE FOR MARKER 1, and
// the difference is the whole reason both exist. Marker 1 is already in the
// filter when the phase starts and DECAYS OUT of it, so "how many ms from the
// start does it stay above threshold" is exactly its splice length. Marker 2 is
// fed at the start of the phase and RAMPS IN through the same 8192 taps, so it
// is below threshold for the first ~130 ms whatever happens afterwards, and
// staleMilliseconds() would report 0.0 for a marker that then sits at full
// level for half a second. Asking how much of the phase was above threshold
// gets an answer in both directions.
//
// Windows overlap, so each contributes its hop rather than its width; the
// figure is a duration, not an integral.
static double msAboveThreshold(const std::vector<double>& profile, double ref,
                               double thrDb, int fs, int hop)
{
    std::size_t n = 0;
    for (const double a : profile)
        if (amplitudeDb(a, ref) >= thrDb) ++n;
    return 1000.0 * static_cast<double>(n) * static_cast<double>(hop)
           / static_cast<double>(fs);
}

static std::vector<float> leftChannel(const std::vector<float>& interleaved)
{
    std::vector<float> mono(interleaved.size() / 2);
    for (std::size_t k = 0; k < mono.size(); ++k)
        mono[k] = interleaved[2 * k];
    return mono;
}

static float peakAbs(std::span<const float> x)
{
    float peak = 0.0f;
    for (const float s : x) peak = std::max(peak, std::fabs(s));
    return peak;
}

static void printProfile(const char* title, const std::vector<double>& profile,
                         double refAmp, int fs, int win, int hop, double untilMs)
{
    std::fprintf(stderr, "\n  %s  (ms, dB re that marker's settled level)\n", title);
    double nextMs = 0.0;
    for (std::size_t i = 0; i < profile.size(); ++i) {
        const double t = windowCentreMs(i, fs, win, hop);
        if (t < nextMs) continue;
        if (t > untilMs) break;
        std::fprintf(stderr, "    %7.1f  %8.1f\n", t, amplitudeDb(profile[i], refAmp));
        // Fine early, coarse late: the decay is where the answer lives.
        nextMs = t + (t < 250.0 ? 5.0 : 25.0);
    }
}

// ---------------------------------------------------------------------------
// Feeding
// ---------------------------------------------------------------------------

// EP6-shaped blocks, exactly as MetisClient delivers them -- the 126 -> 1024
// buffering inside processIqBlock is part of what is under test.
static void feed(Hl2RxDsp& dsp, std::span<const std::complex<float>> s)
{
    for (std::size_t off = 0; off < s.size(); off += kEp6BlockSamples) {
        const std::size_t n = std::min<std::size_t>(kEp6BlockSamples, s.size() - off);
        const auto sub = s.subspan(off, n);
        dsp.processIqBlock(std::vector<std::complex<float>>(sub.begin(), sub.end()));
    }
}

// A steady tone `toneHz` above centre, in WIRE ORDER -- note the negative sine:
// the HPSDR wire is the conjugate of the analytic convention, so a signal ABOVE
// centre arrives as exp(-j.2.pi.f.t). Demodulated USB puts it at toneHz of
// audio. Same generator and same convention as tests/hl2_rxdsp_test.cpp.
static std::vector<std::complex<float>> wireTone(std::size_t n, double toneHz,
                                                 double amplitude, int inputRateHz)
{
    std::vector<std::complex<float>> out(n);
    for (std::size_t k = 0; k < n; ++k) {
        const double ph = 2.0 * kPi * toneHz * static_cast<double>(k)
                          / static_cast<double>(inputRateHz);
        out[k] = static_cast<float>(amplitude)
                 * std::complex<float>(static_cast<float>(std::cos(ph)),
                                       static_cast<float>(-std::sin(ph)));
    }
    return out;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    Hl2RxDsp dsp;
    Hl2RxDsp::Config cfg;
    cfg.inputSampleRateHz = 48000;
    // 48 kHz audio, NOT the 24 kHz default: it makes the audio rate equal to
    // kWdspDspSampleRateHz, so an output sample and a DSP sample are the same
    // instant and the millisecond conversion carries no resampling to explain.
    cfg.audioSampleRateHz = 48000;
    cfg.dspBlockSize = 1024;
    cfg.fftSize = 256;
    cfg.mode = WdspChannel::Mode::Usb;
    cfg.filterLowHz = 150.0;
    cfg.filterHighHz = 3000.0;
    // AGC OFF -- see the header. Mode 0 takes applyRxAgc()'s default branch and
    // is an explicit agcOFF inside WDSP, so what this test times is the FILTER's
    // decay and not the AGC's recovery. The ceiling is inert with the mode off
    // and is pinned only so the config states it.
    cfg.agcMode = 0;
    cfg.maximumAgcGainDb = 39.0;
    cfg.blockForOutput = true;   // deterministic audio for an offline burst feed
    std::string err;
    // Sequenced deliberately: err.empty()/err.c_str() must not share an argument
    // list with the call that FILLS err. Argument evaluation order is
    // unspecified, so the diagnostic could be read before configure() wrote it
    // -- and err.c_str() taken before a reallocating assign is a dangling
    // pointer, not merely a lost message. Same shape fixed in
    // tests/hl2_rxdsp_test.cpp, which this was copied from.
    const bool configured = dsp.configure(cfg, &err);
    check(configured, err.empty() ? "Hl2RxDsp configures" : err.c_str());
    if (g_failures != 0) return 1;

    const int fs = cfg.audioSampleRateHz;
    const int win = kWindowMs * fs / 1000;
    const int hop = kHopMs * fs / 1000;

    // The two markers, both mid-passband for USB [150, 3000], ten detector bins
    // apart, and clear of the DC bin and of both filter skirts.
    //   1 -- what was on the air BEFORE the key.
    //   2 -- what the DDC is handing up DURING the over.
    const double kMarker1Hz = 1500.0;
    const double kMarker2Hz = 2500.0;
    // Small, because with the AGC off the marker meets a fixed ~+22 dB (10 dB of
    // WdspChannel fixed gain, 12 dB of WDSP's unconditional RXA panel gain of
    // 4.0) and nothing is left to hold it down. Every figure below is relative,
    // so the absolute level is free; the two level controls are what make this
    // choice safe rather than lucky. 0.1 would have hit the rail.
    const double kMarkerAmplitude = 0.02;

    enum class Phase { Marker, Muted, Unmuted, Calib2 };
    Phase phase = Phase::Marker;
    std::vector<float> markerAudio, mutedAudio, unmutedAudio, calib2Audio;
    int markerBlocks = 0, mutedBlocks = 0, unmutedBlocks = 0, calib2Blocks = 0;

    // Direct connection (same thread, context object is the dsp), so this fires
    // synchronously inside processIqBlock and the phase tag is always right.
    QObject::connect(&dsp, &Hl2RxDsp::audioReady, &dsp,
                     [&](const std::vector<float>& pcm) {
        const auto mono = leftChannel(pcm);
        switch (phase) {
        case Phase::Marker:
            markerAudio.insert(markerAudio.end(), mono.begin(), mono.end());
            ++markerBlocks;
            break;
        case Phase::Muted:
            mutedAudio.insert(mutedAudio.end(), mono.begin(), mono.end());
            ++mutedBlocks;
            break;
        case Phase::Unmuted:
            unmutedAudio.insert(unmutedAudio.end(), mono.begin(), mono.end());
            ++unmutedBlocks;
            break;
        case Phase::Calib2:
            calib2Audio.insert(calib2Audio.end(), mono.begin(), mono.end());
            ++calib2Blocks;
            break;
        }
    });

    // Each phase length is a whole multiple of dspBlockSize, so processIqBlock's
    // m_iqBuffer drains EMPTY at every phase boundary and no block straddles a
    // setAudioMuted() call.
    //
    // Note that this is NOT because the phase length is a multiple of
    // kEp6BlockSamples -- it is not. 147456 / 126 = 1170.3, so feed() ends each
    // marker phase with a short 36-sample block. What makes the claim hold is
    // that the phase TOTAL is a multiple of dspBlockSize and processIqBlock
    // consumes whole dspBlockSize chunks, so `consumed` reaches the end of
    // m_iqBuffer exactly at the phase boundary however the EP6 blocks fell.
    const std::size_t blk = static_cast<std::size_t>(cfg.dspBlockSize);
    const std::size_t markerSamples   = 144 * blk;   // ~3.07 s: fills the 8192-tap
                                                     // history many times over
    const std::size_t muteSamples     =  24 * blk;   // ~512 ms, well over the
                                                     // 170.667 ms under test
    const std::size_t unmuteSamples   =  48 * blk;   // ~1.02 s of observation
    const std::size_t calibSamples    = 144 * blk;   // marker 2's own reference

    const auto marker1 = wireTone(markerSamples, kMarker1Hz, kMarkerAmplitude,
                                  cfg.inputSampleRateHz);
    // MARKER 2, fed DURING the mute: the live input an over does not silence.
    // This is what makes setAudioMuted() observable on a ZERO build -- delete
    // the two calls below and this marker walks straight through.
    const auto marker2Mute = wireTone(muteSamples, kMarker2Hz, kMarkerAmplitude,
                                      cfg.inputSampleRateHz);
    const auto marker2Calib = wireTone(calibSamples, kMarker2Hz, kMarkerAmplitude,
                                       cfg.inputSampleRateHz);
    // ZERO IQ for the observation. Feeding a marker back after the unmute would
    // make a splice indistinguishable from a signal -- which is also why this
    // file cannot time a dropout.
    const std::vector<std::complex<float>> silenceObs(unmuteSamples,
                                                      std::complex<float>(0.0f, 0.0f));

    // ---- LEG 1: the measurement -------------------------------------------
    phase = Phase::Marker;
    feed(dsp, std::span<const std::complex<float>>(marker1));

    phase = Phase::Muted;
    dsp.setAudioMuted(true);
    feed(dsp, std::span<const std::complex<float>>(marker2Mute));

    phase = Phase::Unmuted;
    dsp.setAudioMuted(false);
    feed(dsp, std::span<const std::complex<float>>(silenceObs));

    // Marker 2's own 0 dB reference, unmuted, through this same chain.
    phase = Phase::Calib2;
    feed(dsp, std::span<const std::complex<float>>(marker2Calib));

    check(markerBlocks > 0, "marker phase produced audio blocks");
    check(mutedBlocks > 0, "muted phase produced audio blocks (both builds emit at cadence)");
    check(unmutedBlocks > 0, "post-unmute phase produced audio blocks");
    check(calib2Blocks > 0, "marker-2 calibration phase produced audio blocks");
    // mutedAudio belongs in this guard as much as the others: a build whose
    // muted branch stops emitting would otherwise print a full table whose
    // during-mute column reads 0.0 ms / -300 dB -- indistinguishable from clean
    // suppression. The exit code would still be 1, but the table is what gets
    // quoted.
    if (markerAudio.empty() || mutedAudio.empty() || unmutedAudio.empty()
        || calib2Audio.empty()) {
        std::fprintf(stderr, "FAIL: no audio to measure; the rest is meaningless\n");
        return 1;
    }

    const auto marker1Profile = toneAmplitudeProfile(markerAudio, kMarker1Hz, fs, win, hop);
    const double ref1 = settledAmplitude(marker1Profile);
    const auto calib2Profile = toneAmplitudeProfile(calib2Audio, kMarker2Hz, fs, win, hop);
    const double ref2 = settledAmplitude(calib2Profile);

    std::fprintf(stderr,
                 "\nhl2_rxdsp_unmute_staleness_test\n"
                 "  audio rate      %d Hz, block %zu samples (%.3f ms)\n"
                 "  AGC             OFF (cfg.agcMode = 0) so the decay shape is\n"
                 "                  the FILTER's and not the AGC's recovery curve\n"
                 "  marker 1        %.0f Hz, amplitude %.2f, fed BEFORE the mute,\n"
                 "                  %d blocks captured, settled level %.6g (its 0 dB)\n"
                 "  marker 2        %.0f Hz, amplitude %.2f, fed DURING the mute,\n"
                 "                  %d calibration blocks, settled level %.6g (its 0 dB)\n",
                 fs, blk, 1000.0 * static_cast<double>(blk) / fs,
                 kMarker1Hz, kMarkerAmplitude, markerBlocks, ref1,
                 kMarker2Hz, kMarkerAmplitude, calib2Blocks, ref2);
    check(ref1 > 0.0, "marker 1 was actually demodulated (its reference is non-zero)");
    // THE CONTROL THAT MAKES THE MARKER-2 COLUMN MEAN ANYTHING. Without it,
    // "marker 2 was suppressed during the mute" is indistinguishable from
    // "marker 2 never gets through this chain at all".
    check(ref2 > 0.0, "marker 2 is demodulated by this chain when NOT muted "
                      "(so a zero during the mute is suppression, not deafness)");

    // The AGC is off, so nothing is holding the level down: assert that the
    // fixed gain did not drive the demodulated audio into the rail. A clipped
    // marker would put harmonics under the other marker's detector and quietly
    // corrupt every figure below.
    {
        const float peak1 = peakAbs(std::span<const float>(markerAudio));
        const float peak2 = peakAbs(std::span<const float>(calib2Audio));
        std::fprintf(stderr, "  peak audio      marker 1 %.4f, marker 2 %.4f "
                             "(AGC off; must stay off the rail)\n", peak1, peak2);
        check(peak1 < 0.95f && peak2 < 0.95f,
              "demodulated marker audio is not clipping with the AGC off");
        // The other end of the same guard: a marker so small that the -40 dB
        // crossing sits in the numerical grass would report a short splice for a
        // reason that has nothing to do with the filter.
        check(peak1 > 0.01f && peak2 > 0.01f,
              "demodulated marker audio has real level to decay from");
    }

    // Each marker must read as PRESENT throughout the SETTLED half of its own
    // feed. This is leg 1's own sanity check: if the detector cannot see a tone
    // while it is being fed, its silence afterwards proves nothing. The first
    // half is excluded on purpose -- the chain starts with an empty history, so
    // the marker ramps in over roughly the same filter support this test is
    // about to measure on the way out.
    {
        auto heldCheck = [&](const char* what, const std::vector<float>& audio,
                             double toneHz, double ref) {
            const std::size_t half = audio.size() / 2;
            const double settledHalfMs = 1000.0 * static_cast<double>(audio.size() - half) / fs;
            const auto tailProfile = toneAmplitudeProfile(
                std::span<const float>(audio).subspan(half), toneHz, fs, win, hop);
            bool ranToEnd = false;
            const double held = staleMilliseconds(tailProfile, ref, -20.0, fs, win, hop,
                                                  &ranToEnd);
            check(ranToEnd, what);
            std::fprintf(stderr,
                         "  %s held %.1f ms above -20 dB without a break, across the\n"
                         "    %.1f ms SETTLED HALF of its own feed (the first half is\n"
                         "    excluded: empty history, tone still ramping in)\n",
                         toneHz == kMarker1Hz ? "marker 1" : "marker 2",
                         held, settledHalfMs);
        };
        heldCheck("marker 1 is detected continuously across its settled feed",
                  markerAudio, kMarker1Hz, ref1);
        heldCheck("marker 2 is detected continuously across its settled feed",
                  calib2Audio, kMarker2Hz, ref2);
    }

    const auto m1Muted   = toneAmplitudeProfile(mutedAudio,   kMarker1Hz, fs, win, hop);
    const auto m1Unmuted = toneAmplitudeProfile(unmutedAudio, kMarker1Hz, fs, win, hop);
    const auto m2Muted   = toneAmplitudeProfile(mutedAudio,   kMarker2Hz, fs, win, hop);
    const auto m2Unmuted = toneAmplitudeProfile(unmutedAudio, kMarker2Hz, fs, win, hop);

    struct Crossing { double thrDb; double duringMuteMs; double afterUnmuteMs;
                      bool duringRan; bool afterRan; };
    auto measure = [&](const std::vector<double>& during,
                       const std::vector<double>& after, double ref) {
        std::vector<Crossing> rows = { { -6.0, 0, 0, false, false },
                                       { -20.0, 0, 0, false, false },
                                       { -40.0, 0, 0, false, false } };
        for (auto& c : rows) {
            c.duringMuteMs  = staleMilliseconds(during, ref, c.thrDb, fs, win, hop, &c.duringRan);
            c.afterUnmuteMs = staleMilliseconds(after,  ref, c.thrDb, fs, win, hop, &c.afterRan);
        }
        return rows;
    };
    const auto marker1Rows = measure(m1Muted, m1Unmuted, ref1);

    auto printRows = [&](const char* title, const std::vector<Crossing>& rows) {
        std::fprintf(stderr, "\n  %s\n"
                             "    threshold   during mute      after unmute\n", title);
        for (const auto& c : rows) {
            std::fprintf(stderr, "    %6.0f dB   %8.1f ms%s   %8.1f ms%s\n",
                         c.thrDb,
                         c.duringMuteMs, c.duringRan ? " (>=)" : "    ",
                         c.afterUnmuteMs, c.afterRan ? " (>=)" : "    ");
        }
    };

    std::fprintf(stderr, "\n  ==== WHAT THE INSTRUMENT READ ====\n"
                         "  Milliseconds each marker survived, quoted against that\n"
                         "  marker's OWN settled level through this same chain.\n"
                         "  REPORTED, never asserted -- see the header.\n");
    printRows("MARKER 1 (fed BEFORE the mute) -- the staleness #5498 is about.\n"
              "  Milliseconds from the START of each phase that it stayed above\n"
              "  threshold, i.e. the length of the splice:",
              marker1Rows);
    std::fprintf(stderr,
                 "  (\">=\" means it never fell below threshold inside the capture,\n"
                 "   so that figure is a lower bound, not a measurement.)\n");

    // Marker 2 gets a DIFFERENT statistic, and msAboveThreshold() says why: it
    // ramps in rather than decaying out, so a run from the start of the phase
    // reads zero however loudly it arrives later.
    std::fprintf(stderr,
                 "\n  MARKER 2 (fed DURING the mute) -- how much of the LIVE INPUT\n"
                 "  during the over leaks out. Peak window and total time above\n"
                 "  threshold, anywhere in the phase:\n"
                 "    phase           peak level (at)      above -20 dB   above -40 dB\n");
    {
        auto leakRow = [&](const char* name, const std::vector<double>& prof) {
            double atMs = 0.0;
            const double pk = peakDb(prof, ref2, fs, win, hop, &atMs);
            std::fprintf(stderr, "    %-14s %7.1f dB (%6.1f ms)  %8.1f ms   %8.1f ms\n",
                         name, pk, atMs,
                         msAboveThreshold(prof, ref2, -20.0, fs, hop),
                         msAboveThreshold(prof, ref2, -40.0, fs, hop));
        };
        leakRow("during mute", m2Muted);
        leakRow("after unmute", m2Unmuted);

        // THE ONE MEASUREMENT LEG THAT IS ASSERTED, and the reason it can be.
        //
        // Everything else here reports, because asserting it would mean
        // comparing against a retyped DSP constant or taking a side between
        // #5497's candidates. This figure is neither. "The live input during
        // the over stays below -20 dB" is a property BOTH candidates satisfy
        // -- ZERO reads -29.8 dB because it overwrites the input before
        // fexchange2, HOLD reads -300 dB because it never reaches fexchange2
        // -- so pinning it pre-empts neither #5497 nor #5498 and says nothing
        // about which mechanism is right. What it does do is make this the one
        // fixture in the tree that notices the mute being LOST: with both
        // setAudioMuted() calls deleted this row reads 0.0 dB and this check
        // goes red.
        //
        // Note what is deliberately NOT asserted here: nothing compares ZERO's
        // -29.8 dB against HOLD's -300 dB, and nothing asserts which side of
        // the mute edge marker 1 lands on. That boundary is #5497's to settle,
        // and a fixture that pre-judged it would stop being an instrument.
        check(peakDb(m2Muted, ref2, fs, win, hop) < -20.0,
              "the mute keeps the live input during the over below -20 dB");
    }
    std::fprintf(stderr,
                 "\n  How to read the marker-2 column: BOTH mute implementations are\n"
                 "  expected to suppress it, by different mechanisms -- the\n"
                 "  clock-with-zeros branch overwrites the input before fexchange2,\n"
                 "  the hold branch never reaches fexchange2 at all. A build with NO\n"
                 "  mute passes it through at full level, which is what makes this\n"
                 "  column the one that tells setAudioMuted() from its absence on the\n"
                 "  tree this test is compiled in.\n");

    printProfile("marker 1, post-unmute", m1Unmuted, ref1, fs, win, hop, 500.0);
    printProfile("marker 1, during mute", m1Muted, ref1, fs, win, hop, 500.0);
    printProfile("marker 2, during mute", m2Muted, ref2, fs, win, hop, 500.0);

    // Context, printed and NOT asserted against -- see the header comment.
    const double predictedSupportMs =
        1000.0 * static_cast<double>(Hl2RxDsp::kRxFilterTaps)
        / static_cast<double>(Hl2RxDsp::kWdspDspSampleRateHz);
    std::fprintf(stderr,
                 "\n  For context only (NOT an assertion): the RX filter handed to\n"
                 "  RXASetNC is Hl2RxDsp::kRxFilterTaps = %d taps at\n"
                 "  kWdspDspSampleRateHz = %d Hz, so its history spans %.3f ms\n"
                 "  and its group delay is about half that. A frozen history is\n"
                 "  expected to hold near full amplitude for roughly the group\n"
                 "  delay and reach nothing near the full support. With the AGC\n"
                 "  switched off this is a prediction about the FILTER alone.\n",
                 Hl2RxDsp::kRxFilterTaps, Hl2RxDsp::kWdspDspSampleRateHz,
                 predictedSupportMs);

    // ---- LEG 2: the detector's POSITIVE control ---------------------------
    //
    // Splice a known length of real marker audio -- taken from the END of the
    // marker capture, where it is settled -- onto silence, and require the same
    // detector to measure the length that was built. If it cannot find a splice
    // that is there by construction, leg 1's numbers mean nothing in either
    // direction. Nothing here goes through the DSP, so nothing here can be
    // influenced by which mute branch is compiled in.
    {
        auto runControl = [&](const char* what, std::size_t spliceSamples) {
            std::vector<float> synth(unmutedAudio.size(), 0.0f);
            // The length asserted against is the length actually WRITTEN, never
            // the length asked for -- a short marker capture must shorten the
            // control, not fail it for the wrong reason.
            const std::size_t take = std::min({ spliceSamples, markerAudio.size(), synth.size() });
            const std::size_t from = markerAudio.size() - take;
            for (std::size_t k = 0; k < take; ++k)
                synth[k] = markerAudio[from + k];
            const double expectedMs = 1000.0 * static_cast<double>(take) / fs;

            const auto prof = toneAmplitudeProfile(synth, kMarker1Hz, fs, win, hop);
            const double at20 = staleMilliseconds(prof, ref1, -20.0, fs, win, hop);
            const double at40 = staleMilliseconds(prof, ref1, -40.0, fs, win, hop);
            std::fprintf(stderr,
                         "\n  POSITIVE CONTROL %s: built %.1f ms, detector read\n"
                         "    %.1f ms at -20 dB, %.1f ms at -40 dB\n",
                         what, expectedMs, at20, at40);
            // One window's worth of tolerance, and the margin inside it is NOT
            // the whole 10 ms: a Hann window straddling the splice edge still
            // reads the partial splice, so the crossing sits late by
            // construction -- about +2.4 ms at -20 dB (the window needs ~26%
            // coverage to read 0.1 of full) and about +4.1 ms at -40 dB (~9%).
            // So the real headroom here is ~6 ms, not 10. The bias is identical
            // in leg 1, which is why leg 1's figures are quoted +/- a block
            // rather than to the millisecond. What USED to eat the rest of the
            // margin -- a one-window dip ending the reading early -- is handled
            // in staleMilliseconds() by the below-threshold run requirement,
            // not by widening this number.
            const double tol = static_cast<double>(kWindowMs);
            check(std::fabs(at20 - expectedMs) <= tol,
                  "detector measures a known splice at -20 dB, within one window");
            check(std::fabs(at40 - expectedMs) <= tol,
                  "detector measures a known splice at -40 dB, within one window");
        };

        // Two lengths, so the control proves the detector is a RULER and not
        // merely a bell. The long one is sized from the real filter constants
        // purely so it sits in the range leg 1 is being asked about.
        std::size_t full = static_cast<std::size_t>(Hl2RxDsp::kRxFilterTaps)
                           * static_cast<std::size_t>(fs)
                           / static_cast<std::size_t>(Hl2RxDsp::kWdspDspSampleRateHz);
        full = std::clamp<std::size_t>(full, static_cast<std::size_t>(fs) / 50,
                                       unmutedAudio.size() / 2);
        runControl("A (long splice)", full);
        runControl("B (half splice)", full / 2);
    }

    // ---- LEG 3: the detector's NEGATIVE controls --------------------------
    //
    // Every one of these asserts on the WORST window in the whole profile, not
    // on staleMilliseconds(). staleMilliseconds asks "how long from the START",
    // so it returns 0 the moment the first window is quiet and never looks at
    // what happens later -- fine for a measurement, useless as a rejection
    // control, because a burst in the middle would pass unseen. "The detector
    // never mistakes this for the marker" is a statement about every window.
    {
        auto rejects = [&](const char* what, std::span<const float> audio,
                           double detectHz, double ref, double mustBeBelowDb) {
            const auto prof = toneAmplitudeProfile(audio, detectHz, fs, win, hop);
            // peakDb() already IS the worst-window scan, out-param included.
            double worstAtMs = 0.0;
            const double worstDb = peakDb(prof, ref, fs, win, hop, &worstAtMs);
            const double ms = staleMilliseconds(prof, ref, -20.0, fs, win, hop);
            std::fprintf(stderr,
                         "  NEGATIVE CONTROL %s\n"
                         "    worst window %.1f dB at %.1f ms (must be below %.0f), "
                         "staleMilliseconds %.1f ms\n",
                         what, worstDb, worstAtMs,
                         mustBeBelowDb, ms);
            check(worstDb < mustBeBelowDb, what);
        };

        std::fprintf(stderr, "\n");

        // A: pure silence. Nothing at all, at either frequency.
        const std::vector<float> silence(unmutedAudio.size(), 0.0f);
        rejects("A (pure silence, read at marker 1)",
                std::span<const float>(silence), kMarker1Hz, ref1, -20.0);

        // B: a full-length SYNTHETIC tone at marker 2's frequency, at marker
        // 1's own level, read by marker 1's detector. This is the control that
        // separates a tone detector from an energy detector: an RMS gate would
        // call this 1024 ms of splice.
        std::vector<float> other(unmutedAudio.size());
        for (std::size_t n = 0; n < other.size(); ++n) {
            const double ph = 2.0 * kPi * kMarker2Hz * static_cast<double>(n) / fs;
            other[n] = static_cast<float>(ref1 * std::cos(ph));
        }
        rejects("B (synthetic 2500 Hz at marker 1's level, read at marker 1)",
                std::span<const float>(other), kMarker1Hz, ref1, -20.0);

        // C and D: THE CROSS-READ CONTROLS, and the reason the two-marker table
        // is readable at all. REAL demodulated audio -- not a synthetic sine, so
        // WDSP's own harmonic distortion, filter skirts and block seams are in
        // it -- from ONE marker, read by the OTHER marker's detector. If either
        // read above threshold, "marker 2 leaked" and "marker 1 survived" would
        // be the same measurement wearing two labels.
        //
        // Scanned over the SETTLED HALF of each capture, for the same reason
        // that half is the one the references are taken from: a chain starting
        // from an empty history emits a broadband turn-on transient, and a
        // transient is not a cross-read. Excluding it is what makes this control
        // a statement about the detector rather than about the ramp.
        rejects("C (REAL marker-1 audio, settled half, read at marker 2's frequency)",
                std::span<const float>(markerAudio).subspan(markerAudio.size() / 2),
                kMarker2Hz, ref2, -20.0);
        rejects("D (REAL marker-2 audio, settled half, read at marker 1's frequency)",
                std::span<const float>(calib2Audio).subspan(calib2Audio.size() / 2),
                kMarker1Hz, ref1, -20.0);
    }

    if (g_failures == 0)
        std::fprintf(stderr,
                     "\nhl2_rxdsp_unmute_staleness_test: controls passed. The figures\n"
                     "above measure how long PRE-MUTE content persists across the mute\n"
                     "edge, and how much of the live input during the over leaks out.\n"
                     "That is NOT either of the two quantities #5498 asks for -- how\n"
                     "long receive audio is LOST, and how abruptly it returns. It is a\n"
                     "third quantity, and it is the part of #5498 that can be settled\n"
                     "offline with no radio and no listening judgement.\n");
    return g_failures == 0 ? 0 : 1;
}
