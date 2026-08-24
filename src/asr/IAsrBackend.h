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

    // Opt-in: when true, condition each decode on the text the backend
    // itself produced on its last confident segment, for continuity across
    // segment boundaries. The whisper backend does this by passing the carried
    // text as an explicit decode prompt (not by reusing whisper's own internal
    // prompt state), so the confidence gate and resetContext() fully control what
    // conditions the next decode. Default is off (independent decodes) — the
    // historical behavior. No-op for backends that don't support it.
    virtual void setContextCarryEnabled(bool) {}

    // Drop any carried context so the next decode starts clean.
    // The engine calls this on a long silence gap and on an explicit Clear, so a
    // noisy/garbled prior can't keep conditioning later utterances. No-op for
    // backends that don't carry context.
    virtual void resetContext() {}
};

} // namespace AetherSDR
