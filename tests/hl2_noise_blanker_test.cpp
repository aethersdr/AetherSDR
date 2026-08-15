// HL2 impulse noise blanker — the host-side ANB stage ahead of the demodulator.
//
// The HL2 sends raw IQ and runs no firmware DSP, so its noise blanker either
// happens in WDSP on this host or it does not happen at all. That makes the
// interesting assertions BEHAVIOURAL rather than protocol-shaped: there is no
// register write to observe, only whether impulses actually stop reaching the
// audio. Every check here is differential — the same stimulus run twice, once
// with the blanker off and once on — so it measures the stage rather than the
// absolute levels of a chain that may be retuned later.
//
// Four things are pinned:
//   1. The blanker removes impulses and leaves the wanted signal alone.
//   2. The seam's 0..100 level maps onto WDSP's INVERTED threshold, with the
//      slice model's default of 50 landing on pihpsdr's fixed value of 20.
//   3. Transmit does not de-arm it. The mute path clocks the channel with
//      silence, and the blanker triggers on a RATIO against a running average
//      magnitude, so an unheld blanker would gate the start of every receive
//      period — the same failure family as the NR filter-state gap.
//   4. ...and it is still BLANKING there. (3) alone is satisfied by a stage
//      that does nothing at all, which is what flushing on the T->R edge
//      produces: flush sets the running average to full scale and the blanker
//      cannot trigger for ~200 ms. Both directions have to be pinned or the
//      hold can be "fixed" into uselessness without a test noticing.

#include "core/backends/hl2/Hl2RxDsp.h"
#include "core/backends/hl2/MetisProtocol.h"   // kEp6BlockSamples
#include "core/dsp/WdspChannel.h"

#include <QCoreApplication>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <utility>
#include <vector>

using namespace AetherSDR::hl2;

static int g_failures = 0;
static void check(bool cond, const char* what)
{
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
    else {
        std::fprintf(stdout, "ok: %s\n", what);
    }
}

static constexpr double kPi = 3.14159265358979323846;
static constexpr int kFs = 48000;
static constexpr double kToneHz = 1000.0;
// Well below the impulses, so the running average the blanker measures against
// is the tone's magnitude and the impulses are unambiguously outliers.
static constexpr double kToneAmp = 0.02;
static constexpr double kImpulseAmp = 0.5;
// Level 80 puts the trigger at ~7.6x the average magnitude — comfortably below
// the impulse/tone ratio of 25 and comfortably above the tone itself, so the
// test is not sitting on the threshold it is trying to exercise.
static constexpr int kNbLevel = 80;

namespace {

// One second of a +1 kHz tone IN WIRE ORDER (the HPSDR wire is the conjugate of
// the analytic convention — see Hl2RxDsp::processIqBlock), optionally with a
// one-sample impulse every `impulsePeriod` samples.
std::vector<std::complex<float>> makeStream(int samples, int impulsePeriod,
                                            int phaseOffset = 0)
{
    std::vector<std::complex<float>> out(static_cast<std::size_t>(samples));
    for (int n = 0; n < samples; ++n) {
        const double ph = 2.0 * kPi * kToneHz * (n + phaseOffset) / kFs;
        double i = kToneAmp * std::cos(ph);
        double q = -kToneAmp * std::sin(ph);
        if (impulsePeriod > 0 && ((n + phaseOffset) % impulsePeriod) == 0) {
            // Real-valued spike: broadband, which is what an impulse IS and what
            // makes it survive into the passband however the filter is set.
            i += kImpulseAmp;
        }
        out[static_cast<std::size_t>(n)] =
            std::complex<float>(static_cast<float>(i), static_cast<float>(q));
    }
    return out;
}

void feed(Hl2RxDsp& dsp, const std::vector<std::complex<float>>& stream)
{
    for (std::size_t off = 0; off < stream.size(); off += kEp6BlockSamples) {
        const std::size_t n =
            std::min<std::size_t>(kEp6BlockSamples, stream.size() - off);
        dsp.processIqBlock(std::vector<std::complex<float>>(
            stream.begin() + static_cast<std::ptrdiff_t>(off),
            stream.begin() + static_cast<std::ptrdiff_t>(off + n)));
    }
}

struct Capture {
    float peak = 0.0f;
    double sumSquares = 0.0;
    std::size_t count = 0;
    bool collecting = false;

    void reset()
    {
        peak = 0.0f;
        sumSquares = 0.0;
        count = 0;
    }
    [[nodiscard]] double rms() const
    {
        return count > 0 ? std::sqrt(sumSquares / static_cast<double>(count)) : 0.0;
    }
};

Hl2RxDsp::Config baseConfig()
{
    Hl2RxDsp::Config cfg;
    cfg.inputSampleRateHz = kFs;
    cfg.audioSampleRateHz = kFs;
    cfg.dspBlockSize = 1024;
    cfg.fftSize = 256;
    cfg.mode = WdspChannel::Mode::Usb;
    // AGC OFF. With it on, the AGC would hide exactly what this test measures:
    // its whole job is to hold the output level constant, so a chain that lets
    // every impulse through and one that blanks them all would produce similar
    // peaks. Off, the audio is a scaled copy of the IF and the peak means
    // something.
    cfg.agcMode = 0;
    cfg.blockForOutput = true;   // deterministic for an offline burst feed
    return cfg;
}

}  // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // ── 1. The level -> threshold map ─────────────────────────────────────
    //
    // Pinned because it is a policy decision, and an inverted one: the seam's
    // level rises with aggression and WDSP's threshold falls with it. Getting
    // the direction wrong produces a control that works backwards, which is
    // worse than one that does nothing.
    {
        const double atZero = WdspChannel::noiseBlankerThresholdForLevel(0);
        const double atDefault = WdspChannel::noiseBlankerThresholdForLevel(50);
        const double atFull = WdspChannel::noiseBlankerThresholdForLevel(100);
        check(std::abs(atZero - 100.0) < 1e-6, "level 0 maps to threshold 100");
        check(std::abs(atDefault - 20.0) < 1e-6,
              "level 50 (the slice default) maps to 20, pihpsdr's fixed value");
        check(std::abs(atFull - 4.0) < 1e-6, "level 100 maps to threshold 4");
        bool monotone = true;
        for (int level = 1; level <= 100; ++level) {
            if (WdspChannel::noiseBlankerThresholdForLevel(level) >=
                WdspChannel::noiseBlankerThresholdForLevel(level - 1)) {
                monotone = false;
            }
        }
        check(monotone, "higher level is always a LOWER threshold (more aggressive)");
        // Out-of-range input is clamped rather than extrapolated: an
        // unvalidated level from the seam must not produce a threshold of zero,
        // which blanks every sample and mutes the receiver.
        check(std::abs(WdspChannel::noiseBlankerThresholdForLevel(-40) - 100.0) < 1e-6,
              "a negative level clamps to the least aggressive threshold");
        check(std::abs(WdspChannel::noiseBlankerThresholdForLevel(400) - 4.0) < 1e-6,
              "an over-range level clamps to the most aggressive threshold");
    }

    // ── 2. A transmit channel has no blanker ──────────────────────────────
    {
        WdspChannel::Config tx;
        tx.direction = WdspChannel::Direction::Transmit;
        tx.inputSampleRate = kFs;
        tx.dspSampleRate = kFs;
        tx.outputSampleRate = kFs;
        std::string err;
        auto channel = WdspChannel::create(tx, &err);
        check(channel != nullptr, err.empty() ? "transmit channel opens" : err.c_str());
        if (channel) {
            check(!channel->setNoiseBlanker(true, 50),
                  "setNoiseBlanker is refused on a transmit channel");
            check(!channel->noiseBlankerEnabled(),
                  "a refused enable does not leave the transmit channel claiming one");
        }
    }

    // ── 3. Blanking, measured ─────────────────────────────────────────────
    //
    // Two runs of the same stimulus through two identically configured chains.
    // Separate objects rather than one object toggled, so neither run inherits
    // the other's delay line or its running average.
    const int oneSecond = kFs;
    // A quiet lead-in with no impulses in it, because the blanker ARMS rather
    // than starting armed: creating or flushing the stage sets its running
    // average to 1.0 (full scale), so nothing exceeds the threshold until that
    // average has decayed to the real signal level. At backtau = 0.05 s that is
    // a few hundred milliseconds, and impulses inside that window pass through
    // by design.
    const auto leadIn = makeStream(oneSecond, 0);
    // 24 impulses a second — sparse enough that they do not themselves lift the
    // running average, dense enough that a one-second window contains plenty.
    const auto impulsive = makeStream(oneSecond, 2000, oneSecond);

    double peakOff = 0.0;
    double rmsOff = 0.0;
    double peakOn = 0.0;
    double rmsOn = 0.0;

    for (int pass = 0; pass < 2; ++pass) {
        const bool blankerOn = (pass == 1);
        Hl2RxDsp dsp;
        std::string err;
        check(dsp.configure(baseConfig(), &err),
              err.empty() ? "Hl2RxDsp configures" : err.c_str());
        if (blankerOn) {
            dsp.setNoiseBlanker(true, kNbLevel);
            check(dsp.noiseBlankerEnabled(), "Hl2RxDsp reports the blanker enabled");
            check(dsp.noiseBlankerLevel() == kNbLevel,
                  "Hl2RxDsp reports the level it was given");
        }

        Capture cap;
        QObject::connect(&dsp, &Hl2RxDsp::audioReady, &dsp,
                         [&cap](const std::vector<float>& pcm) {
            if (!cap.collecting)
                return;
            for (float s : pcm) {
                cap.peak = std::max(cap.peak, std::abs(s));
                cap.sumSquares += static_cast<double>(s) * s;
                ++cap.count;
            }
        });

        feed(dsp, leadIn);          // arm; nothing measured
        cap.collecting = true;
        cap.reset();
        feed(dsp, impulsive);

        if (blankerOn) {
            peakOn = cap.peak;
            rmsOn = cap.rms();
        } else {
            peakOff = cap.peak;
            rmsOff = cap.rms();
        }
    }

    check(peakOff > 0.0 && peakOn > 0.0, "both passes produced audio");
    // The headline claim. An impulse that reaches the demodulator dominates the
    // peak; one that is blanked before the bandpass never gets there.
    check(peakOn < peakOff * 0.5,
          "the blanker at least halves the impulse peak in the demodulated audio");
    // And the claim that makes it useful rather than merely quiet: the wanted
    // signal is still there. A stage that gated the whole passband would pass
    // the assertion above and fail this one.
    check(rmsOn > rmsOff * 0.5,
          "the wanted tone survives blanking (the stage is not just a mute)");
    std::fprintf(stdout, "  peak off=%.4f on=%.4f  rms off=%.5f on=%.5f\n",
                 peakOff, peakOn, rmsOff, rmsOn);

    // ── 4. The blanker survives a rate change ─────────────────────────────
    //
    // reconfigure() destroys the WDSP channel, and the blanker lives with it.
    // Without the mirror in Hl2RxDsp::Config, changing the HL2's sample rate
    // would silently switch off a blanker whose button stays lit — the same
    // defect the notch replay and the shift replay exist to prevent.
    {
        Hl2RxDsp dsp;
        std::string err;
        check(dsp.configure(baseConfig(), &err),
              err.empty() ? "Hl2RxDsp configures for the rate-change case" : err.c_str());
        dsp.setNoiseBlanker(true, 70);
        Hl2RxDsp::Config faster = baseConfig();
        faster.inputSampleRateHz = 96000;
        check(dsp.configure(faster, &err),
              err.empty() ? "Hl2RxDsp reconfigures to 96 kHz" : err.c_str());
        check(dsp.noiseBlankerEnabled() && dsp.noiseBlankerLevel() == 70,
              "the blanker and its level survive a sample-rate change");
    }

    // ── 5. Transmit must not de-arm it ────────────────────────────────────
    //
    // The regression this pins: the mute path feeds the channel ZEROS while
    // transmitting, and the blanker's trigger is relative, so an unheld blanker
    // watches its running average decay to nothing and then treats the first
    // real receive sample as an enormous impulse — a hole at the start of every
    // receive period.
    //
    // Differential again, and deliberately so: the mute path itself has a
    // settling cost (pipeline latency, the DC blocker) that both arms pay, so
    // the comparison is blanker-on recovery against blanker-off recovery rather
    // than against a hardcoded level.
    {
        const auto clean = makeStream(oneSecond, 0);
        const auto silence = std::vector<std::complex<float>>(
            static_cast<std::size_t>(kFs / 2), std::complex<float>(0.0f, 0.0f));
        // The measurement is HOW MANY BLOCKS UNTIL AUDIO IS BACK, not how loud
        // the first quarter second is. The hole is short — a blanker fed silence
        // re-converges its average within a few hundred samples of real signal,
        // so it gates roughly 7 ms — and spread over 250 ms that is a 3% dip
        // that no threshold can separate from the pipeline's own settling.
        // Counted in blocks it is unambiguous, and it is also the units the
        // defect is actually heard in.
        //
        // A SHORT block deliberately: at the production 1024 the whole hole
        // fits inside one block and shows up as a partial dip rather than a
        // delayed recovery. 256 samples is 5.3 ms, so the gap spans blocks and
        // the index moves.
        constexpr int kEdgeBlock = 256;
        int recoveryBlockOff = -1;
        int recoveryBlockOn = -1;

        for (int pass = 0; pass < 2; ++pass) {
            const bool blankerOn = (pass == 1);
            Hl2RxDsp dsp;
            Hl2RxDsp::Config cfg = baseConfig();
            cfg.dspBlockSize = kEdgeBlock;
            std::string err;
            check(dsp.configure(cfg, &err),
                  err.empty() ? "Hl2RxDsp configures for the TX-edge case" : err.c_str());
            if (blankerOn)
                dsp.setNoiseBlanker(true, kNbLevel);

            std::vector<double> blockRms;
            bool collecting = false;
            QObject::connect(&dsp, &Hl2RxDsp::audioReady, &dsp,
                             [&](const std::vector<float>& pcm) {
                if (!collecting)
                    return;
                double sum = 0.0;
                for (float s : pcm)
                    sum += static_cast<double>(s) * s;
                blockRms.push_back(pcm.empty() ? 0.0
                                               : std::sqrt(sum / static_cast<double>(pcm.size())));
            });

            feed(dsp, clean);            // arm on real signal
            dsp.setAudioMuted(true);
            feed(dsp, silence);          // "transmit"
            dsp.setAudioMuted(false);
            collecting = true;
            feed(dsp, makeStream(kFs / 2, 0, oneSecond));

            // Settled level from the tail, then the first block that reaches
            // half of it. Both arms share the same pipeline latency and the same
            // WDSP mute ramp, so any difference in this index is the blanker's.
            double steady = 0.0;
            const std::size_t tail = std::min<std::size_t>(10, blockRms.size());
            for (std::size_t k = blockRms.size() - tail; k < blockRms.size(); ++k)
                steady += blockRms[k];
            steady = tail > 0 ? steady / static_cast<double>(tail) : 0.0;
            int recovered = -1;
            for (std::size_t k = 0; k < blockRms.size(); ++k) {
                if (blockRms[k] > steady * 0.5) {
                    recovered = static_cast<int>(k);
                    break;
                }
            }
            (blankerOn ? recoveryBlockOn : recoveryBlockOff) = recovered;
        }

        check(recoveryBlockOff >= 0, "the blanker-off arm recovers audio after transmit");
        check(recoveryBlockOn >= 0, "the blanker-on arm recovers audio after transmit");
        check(recoveryBlockOn >= 0 && recoveryBlockOn <= recoveryBlockOff,
              "the blanker adds NO delay to the return of audio after transmit");
        std::fprintf(stdout, "  post-TX recovery block off=%d on=%d (%d samples each)\n",
                     recoveryBlockOff, recoveryBlockOn, kEdgeBlock);
    }

    // ── 6. ...and it must still BLANK once transmit ends ───────────────────
    //
    // Section 5 on its own is satisfied by a blanker that does nothing at all:
    // a permanently deaf stage adds no delay either. That is not hypothetical —
    // it is precisely what an implementation that FLUSHES the stage on the
    // transmit-to-receive edge produces. flush_anb() sets the running average
    // to 1.0 (full scale), and at backtau = 0.05 s nothing can exceed
    // threshold * average for ~200 ms, so the blanker is inert for the first
    // fifth of a second of every receive period while section 5 stays green.
    //
    // So this measures the thing section 5 cannot see: with impulses arriving
    // IMMEDIATELY after the edge, does the peak actually come down? Verified to
    // fail on the flushing implementation (ratio 1.000, bit-identical to the
    // blanker being off) and pass on this one (~0.52, matching the settled
    // ratio in section 3).
    {
        // Peak of the demodulated audio in an early window after the TX edge,
        // and in a late one, for a chain with the blanker on or off. The first
        // 40 ms are skipped: the WDSP mute ramp and the pipeline's own settling
        // are paid by BOTH arms and are not what is being measured.
        const auto postTxPeaks = [&](bool blankerOn) {
            Hl2RxDsp dsp;
            std::string err;
            check(dsp.configure(baseConfig(), &err),
                  err.empty() ? "Hl2RxDsp configures for the post-TX blanking case"
                              : err.c_str());
            if (blankerOn)
                dsp.setNoiseBlanker(true, kNbLevel);

            std::vector<float> audio;
            bool collecting = false;
            QObject::connect(&dsp, &Hl2RxDsp::audioReady, &dsp,
                             [&](const std::vector<float>& pcm) {
                if (collecting)
                    audio.insert(audio.end(), pcm.begin(), pcm.end());
            });

            feed(dsp, makeStream(oneSecond, 0));      // arm on real signal
            dsp.setAudioMuted(true);
            feed(dsp, std::vector<std::complex<float>>(
                          static_cast<std::size_t>(kFs / 2),
                          std::complex<float>(0.0f, 0.0f)));   // "transmit"
            dsp.setAudioMuted(false);
            collecting = true;
            // Receive returns WITH impulses, from the very first sample.
            feed(dsp, makeStream(oneSecond, 2000, oneSecond));

            // audioReady carries INTERLEAVED stereo at kFs.
            const std::size_t skip = static_cast<std::size_t>(0.040 * kFs) * 2;
            const std::size_t earlyEnd = static_cast<std::size_t>(0.200 * kFs) * 2;
            const std::size_t lateStart = static_cast<std::size_t>(0.600 * kFs) * 2;
            std::pair<double, double> peaks{0.0, 0.0};
            for (std::size_t k = skip; k < std::min(earlyEnd, audio.size()); ++k)
                peaks.first = std::max(peaks.first,
                                       static_cast<double>(std::abs(audio[k])));
            for (std::size_t k = lateStart; k < audio.size(); ++k)
                peaks.second = std::max(peaks.second,
                                        static_cast<double>(std::abs(audio[k])));
            return peaks;
        };

        const auto offPeaks = postTxPeaks(false);
        const auto onPeaks = postTxPeaks(true);
        check(offPeaks.first > 0.0 && onPeaks.first > 0.0,
              "both post-TX arms produced audio in the early window");
        const double earlyRatio =
            offPeaks.first > 0.0 ? onPeaks.first / offPeaks.first : 1.0;
        const double lateRatio =
            offPeaks.second > 0.0 ? onPeaks.second / offPeaks.second : 1.0;
        // THE assertion. 0.75 rather than the 0.5 of section 3 because the
        // stage is still converging its average over part of this window; the
        // defect it catches sits at 1.000, so the margin is not tight.
        check(earlyRatio < 0.75,
              "the blanker is ARMED immediately after transmit (it still blanks "
              "impulses in the first 200 ms of the receive period)");
        // And it is not merely gating the whole early window: whatever it does
        // there is in line with what it does once fully settled.
        check(earlyRatio < lateRatio * 2.0,
              "early-window blanking is comparable to the settled steady state");
        std::fprintf(stdout,
                     "  post-TX impulse peak: off=%.4f on=%.4f (40-200ms, ratio %.3f)"
                     "  settled ratio %.3f\n",
                     offPeaks.first, onPeaks.first, earlyRatio, lateRatio);
    }

    if (g_failures == 0)
        std::fprintf(stdout, "hl2_noise_blanker_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
