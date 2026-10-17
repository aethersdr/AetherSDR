// DeepCW's greedy CTC decode, model-free: no ONNX Runtime, weights or sockets.
// Every log-prob row below is CONSTRUCTED; it exercises the decode rule only.
#include "core/DeepCwEngine.h"
#include "core/DeepCwCommitter.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using AetherSDR::DeepCwEngine;

namespace {

constexpr int kC = DeepCwEngine::kNumClasses;
constexpr int kBlank = DeepCwEngine::kBlankIndex;
constexpr int kA = 14, kB = 15, kSpace = 40;   // model.onnx.json "chars" order

// One row per frame: the named class gets `top`, every other class `top - 10`.
std::vector<float> rows(const std::vector<int>& argmax, float top = 0.0f)
{
    std::vector<float> lp(argmax.size() * kC, top - 10.0f);
    for (size_t t = 0; t < argmax.size(); ++t) { lp[t * kC + argmax[t]] = top; }
    return lp;
}

bool near(double a, double b) { return std::fabs(a - b) < 1e-5; }

} // namespace

int main()
{
    const DeepCwEngine eng;
    const double expectConf = 1.0 / (1.0 + (kC - 1) * std::exp(-10.0));

    // Repeats collapse, a blank separates a repeated letter, space is a letter.
    const std::vector<int> seq{kA, kA, kBlank, kA, kB, kB, kBlank, kSpace, kBlank};
    const std::vector<float> lp = rows(seq);
    const int n = static_cast<int>(seq.size());
    float avg = 0.0f;
    const std::string text = eng.ctcDecode(lp.data(), n, &avg);
    if (text != "AAB ") { std::fprintf(stderr, "ctcDecode: got '%s'\n", text.c_str()); return 1; }
    if (!near(avg, expectConf)) { std::fprintf(stderr, "ctcDecode confidence %f\n", avg); return 2; }

    // The committer's path emits the same letters, at the frame each appeared.
    std::vector<uint8_t> blank;
    const auto em = eng.greedyEmissions(lp.data(), n, &blank);
    const std::vector<int> expectFrame{0, 3, 4, 7};
    if (em.size() != expectFrame.size()) { return 3; }
    for (size_t i = 0; i < em.size(); ++i) {
        if (em[i].ch != text[i] || em[i].frame != expectFrame[i] || !near(em[i].conf, expectConf)) { return 4; }
    }
    if (blank != std::vector<uint8_t>{0, 0, 1, 0, 0, 0, 1, 0, 1}) { return 5; }

    // Logits or log-probabilities: a constant offset per row changes nothing.
    const std::vector<float> shifted = rows(seq, 7.5f);
    float avgShifted = 0.0f;
    if (eng.ctcDecode(shifted.data(), n, &avgShifted) != text || !near(avgShifted, avg)) { return 6; }

    // No frames: no text, and confidence reads 1.0 (nothing to doubt).
    float avgEmpty = 0.0f;
    if (!eng.ctcDecode(nullptr, 0, &avgEmpty).empty() || avgEmpty != 1.0f) { return 7; }
    if (!eng.greedyEmissions(nullptr, 0, &blank).empty() || !blank.empty()) { return 8; }
    // A hop longer than the analysis window must not underflow its trim size.
    AetherSDR::DeepCwCommitter longHop(5.0, 3.0, 100.0);
    const std::vector<float> audio(DeepCwEngine::kModelSampleRate * 20, 0.0f);
    longHop.push(audio.data(), audio.size(), eng);
    longHop.push(audio.data(), audio.size(), eng);
    return 0;
}
