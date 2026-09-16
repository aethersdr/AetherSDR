#pragma once

#include "MonoDspStereoAdapter.h"
#include "NnrControls.h"

#include <QByteArray>
#include <atomic>
#include <memory>
#include <vector>

namespace AetherSDR {

class Resampler;

// Client-side neural noise reduction using WDSP 2.10's NNR, as a seventh
// method in the ADSP suite beside DFNR and RN2 (RFC #5684).
//
// Shaped after DeepFilterFilter deliberately: same input/output domain of
// 24 or 48 kHz stereo float32, same mono analysis with a delayed stereo
// level-balance through MonoDspStereoAdapter, same "recreate for a new rate"
// contract. The differences are WDSP's, not ours:
//
//   - NNR's rate must be an integer multiple of 16 kHz, so the 24 kHz path
//     resamples to 48 and back exactly as processBnr() already does, while a
//     48 kHz source reaches it untouched.
//   - its buffers are interleaved DOUBLES with the signal in I, so there is a
//     float->double staging step DeepFilterNet does not need.
//   - its block size is fixed at construction: setSize_nnr() rebuilds the
//     block, FFTW plans and both models, so blocks are accumulated to a fixed
//     size instead.
//
// It is a SPEECH model — a steady carrier is attenuated ~28 dB — so the caller
// must keep it away from CW, the digital modes and the data path.
//
// Thread-safety matches the siblings: main thread writes the atomics, the
// audio thread applies them at the top of process(). WDSP's own setters take
// no lock (AETHERSDR-PATCHES.md patch 5), which is why they are called only
// from process() and never directly from a setter.
class NnrFilter {
public:
    // Unsupported rates leave isValid() false. Recreate for a new rate/source.
    explicit NnrFilter(int sampleRate = 24000);
    ~NnrFilter();

    NnrFilter(const NnrFilter&) = delete;
    NnrFilter& operator=(const NnrFilter&) = delete;

    // Process a block of 24/48 kHz stereo float32 PCM.
    // Returns the processed block (same format, same size).
    QByteArray process(const QByteArray& pcmStereo);

    bool isValid() const { return m_nnr != nullptr; }
    int sampleRate() const { return m_sampleRate; }

    // Clears the FIFO, overlap-add state and the network's recurrent state.
    void reset();

    // End-to-end delay through this filter, in samples at sampleRate() --
    // NNR's own plus the resamplers' group delay on the 24 kHz path, where the
    // latter is the larger of the two.
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
    void* m_nnr{nullptr};                   // NNR, opaque to keep WDSP out of this header

    std::unique_ptr<Resampler> m_up;        // 24 kHz mono -> 48 kHz mono
    std::unique_ptr<Resampler> m_down;      // 48 kHz mono -> 24 kHz mono

    std::vector<float>  m_monoInput;
    std::vector<double> m_blockIn;          // interleaved I/Q, m_blockFrames complex
    std::vector<double> m_blockOut;
    std::vector<float>  m_processed48k;
    QByteArray m_inAccum;                   // 48 kHz mono float awaiting a full block
    MonoDspStereoAdapter m_stereoAdapter;

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
