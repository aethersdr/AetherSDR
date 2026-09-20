// aetherd HL2 -- #5498: does pre-mute audio emerge after an unmute, and for how
// many milliseconds? Today that question is answered by listening. This test
// answers it with a number, offline, with no radio and no antenna.
//
// THE TWO BUILDS THIS MUST SURVIVE UNCHANGED
//
//   ZERO (origin/main):  Hl2RxDsp::processIqBlock's m_audioMuted branch fills
//                        m_i/m_q with 0.0f and FALLS THROUGH to
//                        WdspChannel::processIq. The RXA chain is clocked, so
//                        the overlap-save history flushes DURING the mute.
//   HOLD (this worktree, integration/2026-09-20-listen): the branch consumes
//                        the block, emits a zeroed m_stereo and `continue`s.
//                        fexchange2 is never reached and the history freezes.
//
// So the two builds are predicted to put the SAME stale audio in two different
// places, and this test measures BOTH places:
//
//   staleAfterUnmuteMs  -- pre-mute content emerging after setAudioMuted(false)
//   staleDuringMuteMs   -- pre-mute content emerging while still muted
//
// HOLD should report ~0 for the second and a real number for the first; ZERO
// the other way round. Note that ZERO's during-mute audio is NOT silent: the
// `emit audioReady(m_stereo)` at the bottom of that loop is not guarded on
// m_audioMuted, and Hl2Backend's relay says so in as many words ("Emitted even
// while keyed"). The speaker mixer drops it; per-slice consumers do not.
//
// WHAT THE NUMBER IS NOT COMPARED AGAINST
//
// This test deliberately does NOT assert leg 1 against Hl2RxDsp::kRxFilterTaps /
// kWdspDspSampleRateHz. A test that retypes -- or even re-reads -- the constant
// it is supposed to be measuring agrees with itself while the code it guards is
// wrong. The taps ARE read from the real header, but only to (a) print the
// predicted support alongside the measurement and (b) size the detector's own
// positive control. Leg 1 REPORTS. Only the controls assert.
//
// WHAT TO EXPECT IF THE HOLD BUILD SPLICES
//
// kRxFilterTaps is the FIR length handed to RXASetNC (WdspChannel::open), so the
// frozen history spans kRxFilterTaps samples at kWdspDspSampleRateHz -- 170.667
// ms at the current values. But a windowed-sinc bandpass has its energy
// CONCENTRATED near tap nc/2, so feeding zeros into a full history does not
// produce a 170 ms rectangle: the output holds near full amplitude for roughly
// the filter's group delay (~85 ms) and then falls away, reaching nothing at
// ~170 ms. That is why the report below is an ENERGY PROFILE with three
// threshold crossings, not one number: "how long is the splice" has a different
// answer at -6 dB than at -40 dB, and the shape is the finding.
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
// A sliding Hann-windowed one-bin DFT at the marker frequency -- a Goertzel by
// another name. It is a matched filter for the marker tone and nothing else:
// broadband energy, DC and any other tone are rejected, so "the marker is still
// coming out" cannot be confused with "something is coming out". The same
// function runs over all four legs, which is what makes leg 1's number mean
// something in either build.

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
// that the marker's nearest synthetic competitor (leg 3b, 1000 Hz away) is ten
// bins out and buried by Hann's sidelobe roll-off -- stepped 1 ms at a time so
// the crossing is resolved finer than one audio block.
static constexpr int kWindowMs = 10;
static constexpr int kHopMs = 1;

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

// How many milliseconds from the START of `x` does the marker tone stay at or
// above `thrDb` relative to `refAmp`? Measured at window centres, linearly
// interpolated in dB across the crossing. Returns 0 when the very first window
// is already below threshold. `reachedEnd` reports that it never fell below,
// which makes the answer a lower bound rather than a measurement.
static double staleMilliseconds(const std::vector<double>& profile,
                                double refAmp, double thrDb,
                                int fs, int win, int hop,
                                bool* reachedEnd = nullptr)
{
    if (reachedEnd) *reachedEnd = false;
    if (profile.empty()) return 0.0;

    double prevDb = amplitudeDb(profile[0], refAmp);
    if (prevDb < thrDb) return 0.0;

    for (std::size_t i = 1; i < profile.size(); ++i) {
        const double db = amplitudeDb(profile[i], refAmp);
        if (db < thrDb) {
            const double t0 = windowCentreMs(i - 1, fs, win, hop);
            const double t1 = windowCentreMs(i, fs, win, hop);
            const double span = prevDb - db;
            const double frac = span > 0.0 ? (prevDb - thrDb) / span : 0.0;
            return t0 + frac * (t1 - t0);
        }
        prevDb = db;
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

static std::vector<float> leftChannel(const std::vector<float>& interleaved)
{
    std::vector<float> mono(interleaved.size() / 2);
    for (std::size_t k = 0; k < mono.size(); ++k)
        mono[k] = interleaved[2 * k];
    return mono;
}

static void printProfile(const char* title, const std::vector<double>& profile,
                         double refAmp, int fs, int win, int hop, double untilMs)
{
    std::fprintf(stderr, "\n  %s  (ms, dB re marker)\n", title);
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
    cfg.blockForOutput = true;   // deterministic audio for an offline burst feed
    std::string err;
    check(dsp.configure(cfg, &err), err.empty() ? "Hl2RxDsp configures" : err.c_str());
    if (g_failures != 0) return 1;

    const int fs = cfg.audioSampleRateHz;
    const int win = kWindowMs * fs / 1000;
    const int hop = kHopMs * fs / 1000;

    // The marker: a steady tone 1500 Hz above centre, mid-passband for USB
    // [150, 3000] and far from both the DC bin and either filter skirt. In WIRE
    // ORDER -- note the negative sine: the HPSDR wire is the conjugate of the
    // analytic convention, so a signal ABOVE centre arrives as exp(-j.2.pi.f.t).
    // Demodulated USB puts it at 1500 Hz of audio.
    const double markerHz = 1500.0;

    enum class Phase { Marker, Muted, Unmuted };
    Phase phase = Phase::Marker;
    std::vector<float> markerAudio, mutedAudio, unmutedAudio;
    int markerBlocks = 0, mutedBlocks = 0, unmutedBlocks = 0;

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
        }
    });

    // Phase lengths are whole multiples of dspBlockSize so processIqBlock's
    // m_iqBuffer drains empty at every phase boundary and no block straddles a
    // setAudioMuted() call.
    const std::size_t blk = static_cast<std::size_t>(cfg.dspBlockSize);
    const std::size_t markerSamples   = 144 * blk;   // ~3.07 s: fills the 8192-tap
                                                     // history many times over and
                                                     // settles the AGC
    const std::size_t muteSamples     =  24 * blk;   // ~512 ms, well over the
                                                     // 170.667 ms under test
    const std::size_t unmuteSamples   =  48 * blk;   // ~1.02 s of observation

    std::vector<std::complex<float>> marker(markerSamples);
    for (std::size_t n = 0; n < markerSamples; ++n) {
        const double ph = 2.0 * kPi * markerHz * static_cast<double>(n) / cfg.inputSampleRateHz;
        marker[n] = 0.3f * std::complex<float>(static_cast<float>(std::cos(ph)),
                                               static_cast<float>(-std::sin(ph)));
    }
    // ZERO IQ for both the mute and the observation. This is the honest input:
    // the receiver is muted because WE are transmitting, and what the DDC hands
    // up during an over is not the band, it is nothing. Feeding the marker again
    // after the unmute would make a splice indistinguishable from a signal.
    const std::vector<std::complex<float>> silenceMute(muteSamples, std::complex<float>(0.0f, 0.0f));
    const std::vector<std::complex<float>> silenceObs(unmuteSamples, std::complex<float>(0.0f, 0.0f));

    // ---- LEG 1: the measurement -------------------------------------------
    phase = Phase::Marker;
    feed(dsp, std::span<const std::complex<float>>(marker));

    phase = Phase::Muted;
    dsp.setAudioMuted(true);
    feed(dsp, std::span<const std::complex<float>>(silenceMute));

    phase = Phase::Unmuted;
    dsp.setAudioMuted(false);
    feed(dsp, std::span<const std::complex<float>>(silenceObs));

    check(markerBlocks > 0, "marker phase produced audio blocks");
    check(mutedBlocks > 0, "muted phase produced audio blocks (both builds emit at cadence)");
    check(unmutedBlocks > 0, "post-unmute phase produced audio blocks");
    if (markerAudio.empty() || unmutedAudio.empty()) {
        std::fprintf(stderr, "FAIL: no audio to measure; the rest is meaningless\n");
        return 1;
    }

    const auto markerProfile = toneAmplitudeProfile(markerAudio, markerHz, fs, win, hop);
    const double refAmp = settledAmplitude(markerProfile);
    std::fprintf(stderr,
                 "\nhl2_rxdsp_unmute_staleness_test\n"
                 "  audio rate      %d Hz, block %zu samples (%.3f ms)\n"
                 "  marker          %.0f Hz, %d blocks captured\n"
                 "  marker settled  amplitude %.6g (the 0 dB reference)\n",
                 fs, blk, 1000.0 * static_cast<double>(blk) / fs,
                 markerHz, markerBlocks, refAmp);
    check(refAmp > 0.0, "the marker tone was actually demodulated (reference is non-zero)");

    // The marker must read as PRESENT throughout the SETTLED half of its own
    // feed. This is leg 1's own sanity check: if the detector cannot see the
    // tone while it is being fed, its silence afterwards proves nothing. The
    // first half is excluded on purpose -- the chain starts with an empty
    // history and an unsettled AGC, so the marker ramps in over roughly the
    // same filter support this test is about to measure on the way out.
    {
        const std::size_t half = markerAudio.size() / 2;
        const auto tailProfile = toneAmplitudeProfile(
            std::span<const float>(markerAudio).subspan(half), markerHz, fs, win, hop);
        bool ranToEnd = false;
        const double held = staleMilliseconds(tailProfile, refAmp, -20.0, fs, win, hop, &ranToEnd);
        check(ranToEnd, "marker tone is detected continuously across the settled feed");
        std::fprintf(stderr, "  marker held     %.1f ms above -20 dB over the settled half\n"
                             "                  (fed %.1f ms in total)\n",
                     held, 1000.0 * static_cast<double>(markerSamples) / fs);
    }

    const auto mutedProfile   = toneAmplitudeProfile(mutedAudio, markerHz, fs, win, hop);
    const auto unmutedProfile = toneAmplitudeProfile(unmutedAudio, markerHz, fs, win, hop);

    struct Crossing { double thrDb; double duringMuteMs; double afterUnmuteMs;
                      bool duringRan; bool afterRan; };
    Crossing crossings[] = { { -6.0, 0, 0, false, false },
                             { -20.0, 0, 0, false, false },
                             { -40.0, 0, 0, false, false } };
    for (auto& c : crossings) {
        c.duringMuteMs  = staleMilliseconds(mutedProfile,   refAmp, c.thrDb, fs, win, hop, &c.duringRan);
        c.afterUnmuteMs = staleMilliseconds(unmutedProfile, refAmp, c.thrDb, fs, win, hop, &c.afterRan);
    }

    std::fprintf(stderr,
                 "\n  ==== THE ANSWER TO #5498 ====\n"
                 "  marker-tone energy surviving the mute, by threshold:\n"
                 "    threshold   during mute      after unmute\n");
    for (const auto& c : crossings) {
        std::fprintf(stderr, "    %6.0f dB   %8.1f ms%s   %8.1f ms%s\n",
                     c.thrDb,
                     c.duringMuteMs, c.duringRan ? " (>=)" : "    ",
                     c.afterUnmuteMs, c.afterRan ? " (>=)" : "    ");
    }
    std::fprintf(stderr,
                 "  (\">=\" means it never fell below threshold inside the capture,\n"
                 "   so that figure is a lower bound, not a measurement.)\n");

    printProfile("post-unmute energy profile", unmutedProfile, refAmp, fs, win, hop, 500.0);
    printProfile("during-mute energy profile", mutedProfile, refAmp, fs, win, hop, 500.0);

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
                 "  delay and reach nothing near the full support.\n",
                 Hl2RxDsp::kRxFilterTaps, Hl2RxDsp::kWdspDspSampleRateHz,
                 predictedSupportMs);

    // ---- LEG 2: the detector's POSITIVE control ---------------------------
    //
    // Splice a known length of real marker audio -- taken from the END of the
    // marker capture, where it is settled -- onto silence, and require the same
    // detector to measure the length that was built. If it cannot find a splice
    // that is there by construction, leg 1's number means nothing in either
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

            const auto prof = toneAmplitudeProfile(synth, markerHz, fs, win, hop);
            const double at20 = staleMilliseconds(prof, refAmp, -20.0, fs, win, hop);
            const double at40 = staleMilliseconds(prof, refAmp, -40.0, fs, win, hop);
            std::fprintf(stderr,
                         "\n  POSITIVE CONTROL %s: built %.1f ms, detector read\n"
                         "    %.1f ms at -20 dB, %.1f ms at -40 dB\n",
                         what, expectedMs, at20, at40);
            // One window's worth of tolerance. A Hann window centred on the
            // splice edge sees a partial splice, so the crossing sits a few ms
            // late by construction; the bias is identical in leg 1.
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
    {
        // 3a: pure silence. Must read zero.
        const std::vector<float> silence(unmutedAudio.size(), 0.0f);
        const auto prof = toneAmplitudeProfile(silence, markerHz, fs, win, hop);
        const double ms = staleMilliseconds(prof, refAmp, -20.0, fs, win, hop);
        std::fprintf(stderr, "\n  NEGATIVE CONTROL A (pure silence): detector read %.1f ms\n", ms);
        check(ms == 0.0, "detector reports zero on silence");
    }
    {
        // 3b: a full-length tone at a DIFFERENT audio frequency, at the marker's
        // own level. This is the control that separates a tone detector from an
        // energy detector: an RMS gate would call this 1024 ms of splice.
        const double otherHz = markerHz + 1000.0;
        std::vector<float> other(unmutedAudio.size());
        for (std::size_t n = 0; n < other.size(); ++n) {
            const double ph = 2.0 * kPi * otherHz * static_cast<double>(n) / fs;
            other[n] = static_cast<float>(refAmp * std::cos(ph));
        }
        const auto prof = toneAmplitudeProfile(other, markerHz, fs, win, hop);
        const double ms = staleMilliseconds(prof, refAmp, -20.0, fs, win, hop);
        std::fprintf(stderr,
                     "  NEGATIVE CONTROL B (%.0f Hz tone at marker level): detector read %.1f ms\n",
                     otherHz, ms);
        check(ms == 0.0, "detector rejects a full-amplitude tone at another frequency");
    }

    if (g_failures == 0)
        std::fprintf(stderr,
                     "\nhl2_rxdsp_unmute_staleness_test: controls passed; the two\n"
                     "figures above are the measurement #5498 asks for.\n");
    return g_failures == 0 ? 0 : 1;
}
