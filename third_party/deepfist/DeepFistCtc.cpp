// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Brent Crier (N9BC) - part of Lyra (GPLv3+) per NOTICE.md

#include "DeepFistCtc.h"

namespace lyra::dsp {

namespace {
constexpr int kBlank = 0;   // DeepFist sidecar: ctc.blank_index == 0
}  // namespace

std::string greedyCtcDecode(const float* logProbs, int T, int C,
                            const std::vector<std::string>& tokens) {
    std::string out;
    if (!logProbs || T <= 0 || C <= 0) return out;

    int prev = -1;
    for (int t = 0; t < T; ++t) {
        const float* row = logProbs + static_cast<size_t>(t) * C;
        // argmax over classes for this frame
        int best = 0;
        float bestVal = row[0];
        for (int c = 1; c < C; ++c) {
            if (row[c] > bestVal) { bestVal = row[c]; best = c; }
        }
        // collapse repeats, then drop blanks
        if (best != prev) {
            if (best != kBlank && best < static_cast<int>(tokens.size()))
                out += tokens[static_cast<size_t>(best)];
            prev = best;
        }
    }
    return out;
}

CtcFrames greedyCtcFrames(const float* logProbs, int T, int C,
                          float blankPenalty) {
    CtcFrames r;
    r.T = (T > 0) ? T : 0;
    if (!logProbs || T <= 0 || C <= 0) return r;

    int prev = -1;
    for (int t = 0; t < T; ++t) {
        const float* row = logProbs + static_cast<size_t>(t) * C;
        // argmax with a blank-logit penalty (blank at index 0)
        int   best    = 0;
        float bestVal = row[0] - blankPenalty;
        for (int c = 1; c < C; ++c) {
            if (row[c] > bestVal) { bestVal = row[c]; best = c; }
        }
        if (best != prev) {
            if (best != kBlank) {
                r.ids.push_back(best);
                r.frames.push_back(t);
            }
            prev = best;
        }
    }
    return r;
}

}  // namespace lyra::dsp
