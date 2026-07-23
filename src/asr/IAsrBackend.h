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
    //
    // overlapMs: leading portion (in ms) of pcm16k that duplicates the tail end
    // of the PREVIOUS call's audio — the segmenter's segment-overlap feature
    // (experimental) carries this much trailing audio forward across a
    // force-closed segment so a word split at the boundary isn't lost. 0 means
    // no overlap (the common case). Backends that can identify which part of
    // their output corresponds to that leading audio should omit it from the
    // returned text so it isn't emitted twice; backends that can't (most
    // non-whisper backends) may just ignore this and pass the whole segment
    // through, at the cost of a duplicated word/phrase at re-joined boundaries.
    virtual AsrTranscript transcribe(const std::vector<float>& pcm16k, int overlapMs,
                                      QString* error) = 0;

    // Release the loaded model. Called before destruction; idempotent.
    virtual void unload() = 0;

    // Experimental: when true, condition each decode on the text the backend
    // itself produced last call (whisper.cpp's no_context=false), for
    // continuity across segment boundaries. Default is off (independent decodes)
    // — the historical behavior. No-op for backends that don't support it.
    virtual void setContextCarryEnabled(bool) {}
};

} // namespace AetherSDR
