#pragma once

// Provenance: the spectrogram + greedy-CTC pipeline in this engine is a C++ port
// of decode_morse.py from e04/deepcw-engine (https://github.com/e04/deepcw-engine),
// licensed AGPL-3.0. AetherSDR is GPL-3.0; GPLv3 §13 permits the combination.
// The ported code ships in the binary; the trained weights do not (download-on-
// demand, SHA-256 pinned). See THIRD_PARTY_LICENSES for the AGPL-3.0 entry.

#include <string>
#include <vector>
#include <cstdint>
#include <memory>

#ifdef HAVE_ONNX
#include <onnxruntime_cxx_api.h>
#endif

namespace AetherSDR {

// Neural CW (Morse) decoder backend — the e04/deepcw-engine ONNX model
// (AGPL-3.0; download-on-demand, not shipped). A small CTC CNN over a
// log-magnitude spectrogram. Given a mono audio window already at the model's
// 3200 Hz rate, the engine builds the exact spectrogram the model was trained
// on, runs one ONNX Runtime inference, and greedy-CTC-decodes the log-probs to
// text. Resampling to 3200 Hz is the caller's job (CwDecoder does it with an
// anti-aliased r8brain SRC) — a non-integer decimation without a filter folds
// energy straight into the 400-1200 Hz analysis band.
//
// Deliberately Qt-free (pure std + ORT, self-contained radix-2 FFT) so it can be
// unit-tested and prototyped standalone; the CwDecoder wrapper adapts
// std::string -> QString. ORT calls compile out without HAVE_ONNX (decode()
// then returns empty).
//
// Model contract — pinned to model.onnx.json (verbatim), do not drift:
//   input  "spectrogram" float32 [1, 1, T, 65]   (NCHW; T = time frames)
//   output "log_probs"   float32 [1, T, 42]       (42 = 41 chars + CTC blank)
//   sample_rate 3200, fft_length 256, hop_length 48, Hann window,
//   band 400-1200 Hz -> 65 bins, normalization log1p, blank_index 41.
//   Trained on 5-20 s windows (the time dim is dynamic, so other lengths run,
//   but stay in range for in-distribution accuracy).
class DeepCwEngine {
public:
    DeepCwEngine();
    ~DeepCwEngine();

    // Load model.onnx from disk. Returns false (and logs to stderr) on error.
    bool loadModel(const std::string& path);
    bool isLoaded() const { return m_loaded; }

    // Decode a mono audio window. `audio` is float PCM in [-1, 1] and MUST already
    // be at the model's rate — pass sampleRateHz == kModelSampleRate (3200). The
    // engine does not resample (that needs an anti-aliased filter it deliberately
    // leaves to the caller); a mismatched rate logs once and returns empty.
    // Returns the decoded CW text (may be empty). Feed ~5-20 s of audio for best
    // accuracy.
    // When avgConfidence is non-null, it receives the mean per-emitted-character
    // softmax confidence in [0,1] (1.0 when nothing decoded) — the caller can map
    // this to a cost for the sensitivity filter (cost = 1 - confidence).
    // When pitchHz is non-null, it receives the dominant CW tone frequency in the
    // model's 400-1200 Hz band (peak-energy spectrogram bin), 0 if indeterminate
    // — lets the client zero-beat and show a pitch readout even in neural mode.
    std::string decode(const std::vector<float>& audio, int sampleRateHz,
                       float* avgConfidence = nullptr, float* pitchHz = nullptr) const;

    // Run the same spectrogram + inference as decode() on a 3200 Hz window and
    // return the raw log_probs, row-major [*frames x kNumClasses]. Empty on error.
    // Used by DeepCwCommitter, which needs per-character timing.
    // pitchHz (optional) gets the same dominant-tone estimate decode() gives.
    std::vector<float> inferLogProbs(const std::vector<float>& audio3200, int* frames,
                                     float* pitchHz = nullptr) const;

    // One greedy-CTC emission: the character, the frame it was emitted at
    // (frame f ~ f * kHopLength / kModelSampleRate seconds into the window) and
    // the softmax posterior of its class at that frame.
    struct Emission { char ch; int frame; float conf; };
    // Greedy CTC over log_probs [frames x kNumClasses], same rule as
    // ctcDecode(), keeping per-character timing. blankFrame (optional) gets 1
    // for every frame whose argmax is the CTC blank.
    std::vector<Emission> greedyEmissions(const float* logProbs, int frames,
                                          std::vector<uint8_t>* blankFrame = nullptr) const;

    // Greedy CTC decode of log_probs [frames, kNumClasses] -> text. When
    // avgConfidence is non-null, sets it to the mean softmax probability of the
    // argmax class across the timesteps that emitted a character.
    std::string ctcDecode(const float* logProbs, int frames, float* avgConfidence) const;

    // Model contract constants (from model.onnx.json).
    static constexpr int    kModelSampleRate = 3200;
    static constexpr int    kFftLength       = 256;
    static constexpr int    kHopLength       = 48;
    static constexpr double kMinFreqHz       = 400.0;
    static constexpr double kMaxFreqHz       = 1200.0;
    static constexpr int    kFreqBins        = 65;   // stop_bin(97) - start_bin(32)
    static constexpr int    kNumClasses      = 42;
    static constexpr int    kBlankIndex      = 41;
    static constexpr double kMinWindowSec    = 5.0;
    static constexpr double kMaxWindowSec    = 20.0;

private:
    // Build the model spectrogram, flat row-major [frames * kFreqBins]
    // (time-major, freq inner), from 3200 Hz mono audio. *frames set on return.
    std::vector<float> spectrogram(const std::vector<float>& audio3200, int* frames) const;

    bool m_loaded{false};
#ifdef HAVE_ONNX
    Ort::Env            m_env;
    Ort::SessionOptions m_sessionOpts;
    std::unique_ptr<Ort::Session> m_session;
    Ort::MemoryInfo     m_memInfo;
#endif
};

} // namespace AetherSDR
