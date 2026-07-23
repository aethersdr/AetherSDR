#pragma once

namespace AetherSDR {

// Per-frame voice-activity decision, pluggable into AsrSegmenter (RFC #4333).
//
// The segmenter owns the utterance state machine (hangover, min/max length); an
// IVad only answers "is this frame speech?" — via an energy threshold, a neural
// model (Silero), etc. This keeps the well-tested segmentation logic backend-
// agnostic and lets the detector be swapped without touching it.
class IVad {
public:
    virtual ~IVad() = default;

    // One segmenter frame of 16 kHz mono samples in [-1, 1]. Implementations may
    // buffer internally when their native window differs from the frame size
    // (e.g. Silero's 512-sample window vs. the segmenter's 10 ms frame).
    virtual bool isSpeech(const float* frame, int frameSamples) = 0;

    // Drop internal state (LSTM state, partial window) between utterances.
    virtual void reset() = 0;
};

} // namespace AetherSDR
