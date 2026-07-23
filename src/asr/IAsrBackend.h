#pragma once

#include <QString>

#include <vector>

// Backend-agnostic ASR inference interface (RFC #4333). AsrEngine drives this
// on its worker thread and never depends on a concrete engine. WhisperAsrBackend
// is the production implementor; tests inject a deterministic fake so the
// engine's threading/segmentation logic can be verified without a model.
//
// All calls happen on the worker thread; implementations need not be
// thread-safe.

namespace AetherSDR {

// One transcription result: the recognized text plus a confidence in [0, 1]
// (1 = most confident). Confidence drives the panel's color-coding, mirroring
// the CW decoder's cost-based coloring.
struct AsrTranscript {
    QString text;
    float confidence = 0.0f;
};

class IAsrBackend {
public:
    virtual ~IAsrBackend() = default;

    // Load a model from disk. Returns false and sets *error (if non-null) on
    // failure. May be called again to switch models.
    virtual bool load(const QString& modelPath, QString* error) = 0;

    virtual bool isLoaded() const = 0;

    // Transcribe one utterance of 16 kHz mono float samples in [-1, 1]. Returns
    // the recognized text + confidence (text empty for non-speech). Sets *error
    // on failure.
    virtual AsrTranscript transcribe(const std::vector<float>& pcm16k, QString* error) = 0;

    // Release the loaded model. Called before destruction; idempotent.
    virtual void unload() = 0;
};

} // namespace AetherSDR
