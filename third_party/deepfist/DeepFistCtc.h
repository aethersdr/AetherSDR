// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Brent Crier (N9BC) - part of Lyra (GPLv3+) per NOTICE.md
//
// Lyra — DeepFist neural CW decoder: greedy CTC decode.
//
// Faithful port of DeepFist's own MIT-licensed decoder
// (deepfist/model/decode.py, greedy_ctc_decode): argmax per frame, collapse
// repeats, drop the CTC blank (index 0), map ids -> token strings.
//
// This is a from-scratch re-implementation of the standard, published CTC
// greedy-decode algorithm from DeepFist's MIT source + the model's JSON
// sidecar token table.  No AGPL (DeepCW/HamNoise) code is used.
#pragma once

#include <string>
#include <vector>

namespace lyra::dsp {

// logProbs: row-major [T][C] (T time frames, C classes), as produced by the
// DeepFist model output squeezed to a single batch.  `tokens` is the C-entry
// token table from the model sidecar (tokens[0] == "<blank>").  Returns the
// decoded text (token strings concatenated, e.g. "CQ TEST <SK>").
std::string greedyCtcDecode(const float* logProbs, int T, int C,
                            const std::vector<std::string>& tokens);

// Frame-aware greedy CTC decode (mirrors DeepFist's tci_decode.greedy_frames).
// For each emitted (non-blank, repeat-collapsed) token, records the frame index
// where it was emitted — used by the streaming decoder to commit characters
// once their audio has settled.  `blankPenalty` is subtracted from the blank
// logit before argmax to counter CTC blank over-prediction (0 = off).
struct CtcFrames {
    std::vector<int> ids;      // emitted token ids
    std::vector<int> frames;   // frame index of each emitted id
    int              T = 0;    // total frame count
};
CtcFrames greedyCtcFrames(const float* logProbs, int T, int C,
                          float blankPenalty = 0.0f);

}  // namespace lyra::dsp
