#pragma once

#include "NnrControls.h"

#include <QByteArray>
#include <array>
#include <atomic>
#include <memory>
#include <vector>

namespace AetherSDR {

class Resampler;

// Client-side neural NR using WDSP 2.10's NNR (RFC #5684), shaped like
// DeepFilterFilter: 24 or 48 kHz stereo float32, one instance per channel,
// recreate for a new rate. WDSP differences:
//   - NNR runs at a multiple of 16 kHz, so 24 kHz resamples to 48 and back (as
//     processBnr() does); 48 kHz passes untouched.
//   - buffers are interleaved doubles with the signal in I (float->double stage).
//   - block size is fixed (setSize_nnr() rebuilds everything), so input is
//     accumulated to it.
// A SPEECH model (a steady carrier drops ~28 dB): keep it off CW, digital and
// data paths. Main thread writes atomics; process() applies them, because WDSP's
// setters take no lock (AETHERSDR-PATCHES.md patch 5).
class NnrFilter {
public:
    // Unsupported rates leave isValid() false. Recreate for a new rate/source.
    explicit NnrFilter(int sampleRate = 24000);
    ~NnrFilter();

    NnrFilter(const NnrFilter&) = delete;
    NnrFilter& operator=(const NnrFilter&) = delete;

    // Process a block of 24/48 kHz stereo float32 PCM. Returns the processed
    // audio in the same format, released in whole NNR blocks: a call can
    // return fewer or more frames than it was given, and none while the first
    // block is still filling.
    QByteArray process(const QByteArray& pcmStereo);

    bool isValid() const { return m_nnr[0] != nullptr && m_nnr[1] != nullptr; }
    int sampleRate() const { return m_sampleRate; }

    // Clears the FIFO, overlap-add state and the network's recurrent state.
    void reset();

    // End-to-end delay through this filter, in samples at sampleRate() --
    // NNR's own plus the resamplers' group delay on the 24 kHz path, where the
    // latter is the larger of the two. Both channels have the same delay.
    int delaySamples() const;

    // ── The documented operator controls ──────────────────────────────────
    // 0..100 strength, mapped to the mask floor's -10..-50 dB (RFC #5684 §8).
    // Higher is more suppression; the dB value runs the other way.
    void setStrength(int strength);
    int strength() const { return m_strength.load(); }

    // 0 = Standard, 1 = Premium. Applied on the audio thread; modelSlot()
    // reports what WDSP actually switched to, which differs from the request
    // when a build has no model in that slot.
    void setModel(int slot);
    int modelSlot() const { return m_appliedModel.load(); }

    // ── The tuning controls WDSP leaves undocumented ──────────────────────
    // Ranges and defaults live in NnrControls.h; nothing here clamps, because
    // WDSP's own setters already do.
    void setAlpha(double alpha);
    void setAlphaKnee(double kneeDb);
    void setTau(double tau);
    void setMaxGain(double gainDb);
    void setSmoothing(double attackMs, double releaseMs);

private:
    void applyPendingParameters();
    int totalLatencyFrames() const;

    const int m_sampleRate;
    // Everything below is per channel, indexed 0 = left, 1 = right.
    std::array<void*, 2> m_nnr{};           // NNR, opaque to keep WDSP out of this header

    std::array<std::unique_ptr<Resampler>, 2> m_up;    // 24 kHz -> 48 kHz
    std::array<std::unique_ptr<Resampler>, 2> m_down;  // 48 kHz -> 24 kHz

    std::array<std::vector<float>, 2>  m_channelInput;
    std::array<std::vector<double>, 2> m_blockIn;   // interleaved I/Q, m_blockFrames complex
    std::array<std::vector<double>, 2> m_blockOut;
    std::array<std::vector<float>, 2>  m_processed48k;
    std::array<QByteArray, 2> m_inAccum;    // 48 kHz float awaiting a full block
    std::array<QByteArray, 2> m_channelOutput;
    bool m_lockstepWarned{false};                      // L/R output length mismatch logged

    int m_blockFrames{0};

    std::atomic<int>    m_strength{Nnr::kMaskFloorDefaultStrength};
    std::atomic<int>    m_requestedModel{0};
    std::atomic<int>    m_appliedModel{0};
    std::atomic<double> m_alpha{1.0};
    std::atomic<double> m_alphaKnee{10.0};
    std::atomic<double> m_tau{2.0};
    std::atomic<double> m_maxGain{12.0};
    std::atomic<double> m_smoothAttackMs{0.0};
    std::atomic<double> m_smoothReleaseMs{0.0};
    std::atomic<bool>   m_paramsDirty{true};
};

}  // namespace AetherSDR
