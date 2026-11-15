// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Brent Crier (N9BC) - part of Lyra (GPLv3+) per NOTICE.md
//
// Lyra — DeepFist neural CW decoder: ONNX model engine.
//
// C++ mirror of the verified Rust reference `CwEngine` (diddle
// src-tauri/src/dsp/cw_neural.rs): owns the ONNX Runtime session + token table
// + spectrogram front-end, and decodes a window of 3200 Hz audio to text.
//
// This is the ONLY DeepFist file that depends on ONNX Runtime; the rest of the
// pipeline (resampler, spectrogram, CTC) is plain C++.  ONNX Runtime is MIT
// (Microsoft); the model is DeepFist (MIT, © Brent Crier, N9BC).  No AGPL code.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "DeepFistConditioner.h"
#include "DeepFistSpectrogram.h"

namespace lyra::dsp {

class DeepFistModel {
public:
    DeepFistModel();
    ~DeepFistModel();

    DeepFistModel(const DeepFistModel&)            = delete;
    DeepFistModel& operator=(const DeepFistModel&) = delete;

    // Load deepfist.onnx (+ deepfist.onnx.json sidecar) from `modelDir`.
    // Returns false on any failure (missing file, bad model); the caller can
    // then report "model not found" and stay on the classic engine.
    bool load(const std::string& modelDir);

    bool ready() const { return ready_; }

    // Decode one window of mono audio already at 3200 Hz.  Returns the decoded
    // text (may be empty).  Safe to call only when ready().
    std::string decode3200(const float* audio, int n);

    // Run the model on a 3200 Hz window and expose the raw CTC log-probs
    // (row-major [T][C], one batch).  Lets the streaming decoder do its own
    // frame-timed commit.  Returns false on failure.  Safe only when ready().
    bool infer(const float* audio, int n,
               std::vector<float>& logits, int& T, int& C);

    // Keying-activity ratio (p90/p10) of the window at the auto-locked tone.
    // The streamer gates on this so the model doesn't hallucinate on dead air /
    // steady carriers (see DeepFistConditioner::keyingRatio).
    float keyingRatio(const float* audio, int n) const {
        return cond_.keyingRatio(audio, n, cond_.detectTone(audio, n));
    }

    // Estimated CW speed (WPM) of the window, 0 if indeterminate.
    int keyingWpm(const float* audio, int n) const {
        const float w = cond_.estimateWpm(audio, n, cond_.detectTone(audio, n));
        return static_cast<int>(w + 0.5f);
    }

    const std::vector<std::string>& tokens() const { return tokens_; }
    const std::string& lastError() const { return lastError_; }

private:
    struct Impl;                        // hides the ONNX Runtime types
    std::unique_ptr<Impl> impl_;

    DeepFistConditioner      cond_;     // fldigi-style front-end (exp14+ models)
    DeepFistSpectrogram      fe_;
    std::vector<std::string> tokens_;   // 48-entry table from the sidecar
    std::vector<float>       condScratch_;
    std::vector<float>       specScratch_;
    bool                     ready_ = false;
    std::string              lastError_;
};

}  // namespace lyra::dsp
