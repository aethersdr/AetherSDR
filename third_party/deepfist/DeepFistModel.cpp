// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Brent Crier (N9BC) - part of Lyra (GPLv3+) per NOTICE.md

#include "DeepFistModel.h"

#include <array>
#include <fstream>
#include <sstream>

#include <onnxruntime_cxx_api.h>

#include "DeepFistCtc.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace lyra::dsp {

// ONNX Runtime handles kept out of the header.
struct DeepFistModel::Impl {
    Ort::Env      env{ORT_LOGGING_LEVEL_WARNING, "deepfist"};
    Ort::SessionOptions opts;
    std::unique_ptr<Ort::Session> session;
};

namespace {

// The canonical 48-token table (DeepFist sidecar order).  Used as a fallback if
// the sidecar can't be read; the sidecar is still the runtime source of truth.
std::vector<std::string> fallbackTokens() {
    std::vector<std::string> t = {"", " "};   // blank rendered empty, then space
    const char* chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZ1234567890.,?/=+-@";
    for (const char* p = chars; *p; ++p) t.emplace_back(1, *p);
    t.emplace_back("<SK>");
    t.emplace_back("<KN>");
    // 48 total: blank + space + A-Z(26) + digits(10) + ". , ? / = + - @"(8) + <SK> + <KN>
    return t;
}

// Minimal parse of the sidecar "tokens": [...] array of quoted strings.
// Avoids pulling a JSON lib into this DSP path; the format is fixed + simple.
std::vector<std::string> parseSidecarTokens(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string s = ss.str();

    const std::string key = "\"tokens\"";
    size_t k = s.find(key);
    if (k == std::string::npos) return {};
    size_t lb = s.find('[', k);
    size_t rb = s.find(']', lb == std::string::npos ? k : lb);
    if (lb == std::string::npos || rb == std::string::npos || rb < lb) return {};

    std::vector<std::string> out;
    size_t i = lb + 1;
    while (i < rb) {
        size_t q1 = s.find('"', i);
        if (q1 == std::string::npos || q1 >= rb) break;
        size_t q2 = s.find('"', q1 + 1);
        if (q2 == std::string::npos || q2 > rb) break;
        std::string tok = s.substr(q1 + 1, q2 - q1 - 1);
        if (tok == "<blank>") tok.clear();   // blank -> empty (never emitted)
        out.push_back(tok);
        i = q2 + 1;
    }
    return out;
}

#ifdef _WIN32
std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}
#endif

}  // namespace

DeepFistModel::DeepFistModel() : impl_(std::make_unique<Impl>()) {}
DeepFistModel::~DeepFistModel() = default;

bool DeepFistModel::load(const std::string& modelDir) {
    ready_ = false;
    lastError_.clear();

    const std::string onnx    = modelDir + "/deepfist.onnx";
    const std::string sidecar = modelDir + "/deepfist.onnx.json";

    // tokens: sidecar first, fallback to the built-in table
    tokens_ = parseSidecarTokens(sidecar);
    if (tokens_.size() < 2) tokens_ = fallbackTokens();

    // Display prosigns in fldigi-style angle notation.  The model emits the
    // aliased punctuation tokens "=" / "+" for BT / AR (that's how they're
    // written in CW); relabel them to <BT>/<AR> so the copy matches the classic
    // decoder and operator expectation (consistent with <SK>/<KN>).  Display
    // only — a MISREAD prosign still shows whatever the model actually decoded.
    for (std::string& t : tokens_) {
        if (t == "=") t = "<BT>";
        else if (t == "+") t = "<AR>";
    }

    try {
        impl_->opts.SetIntraOpNumThreads(1);
        impl_->opts.SetGraphOptimizationLevel(ORT_ENABLE_ALL);
#ifdef _WIN32
        impl_->session = std::make_unique<Ort::Session>(
            impl_->env, widen(onnx).c_str(), impl_->opts);
#else
        impl_->session = std::make_unique<Ort::Session>(
            impl_->env, onnx.c_str(), impl_->opts);
#endif
    } catch (const Ort::Exception& e) {
        lastError_ = std::string("ONNX load failed: ") + e.what();
        return false;
    } catch (const std::exception& e) {
        lastError_ = std::string("model load failed: ") + e.what();
        return false;
    }

    ready_ = true;
    return true;
}

bool DeepFistModel::infer(const float* audio, int n,
                          std::vector<float>& logits, int& T, int& C) {
    T = 0; C = 0;
    logits.clear();
    if (!ready_ || !audio || n <= 0) return false;

    // fldigi-style conditioner FIRST (exp14+ models are trained on conditioned,
    // single-signal, 600 Hz-centred, level-normalised audio — train==inference).
    condScratch_ = cond_.condition(audio, n);
    const float* condAudio = condScratch_.empty() ? audio : condScratch_.data();

    int specT = 0;
    fe_.compute(condAudio, n, specScratch_, specT);  // [65 * specT] row-major
    if (specT == 0 || specScratch_.empty()) return false;

    try {
        Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(
            OrtArenaAllocator, OrtMemTypeDefault);
        const std::array<int64_t, 4> inShape{
            1, 1, DeepFistSpectrogram::kFreqBins, specT};
        Ort::Value input = Ort::Value::CreateTensor<float>(
            mem, specScratch_.data(), specScratch_.size(),
            inShape.data(), inShape.size());

        const char* inNames[]  = {"spectrogram"};
        const char* outNames[] = {"log_probs"};
        auto outputs = impl_->session->Run(
            Ort::RunOptions{nullptr}, inNames, &input, 1, outNames, 1);

        auto info = outputs[0].GetTensorTypeAndShapeInfo();
        auto oshape = info.GetShape();                // [t_out, batch=1, class]
        if (oshape.size() != 3) return false;
        T = static_cast<int>(oshape[0]);
        C = static_cast<int>(oshape[2]);
        const float* logp = outputs[0].GetTensorData<float>();
        // [t_out, 1, class] row-major == [t_out][class] for batch 1
        logits.assign(logp, logp + static_cast<size_t>(T) * C);
        return true;
    } catch (const Ort::Exception& e) {
        lastError_ = std::string("inference failed: ") + e.what();
        return false;
    }
}

std::string DeepFistModel::decode3200(const float* audio, int n) {
    std::vector<float> logits;
    int T = 0, C = 0;
    if (!infer(audio, n, logits, T, C)) return {};
    return greedyCtcDecode(logits.data(), T, C, tokens_);
}

}  // namespace lyra::dsp
