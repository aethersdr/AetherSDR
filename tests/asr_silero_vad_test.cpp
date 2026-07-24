// Silero VAD (ONNX) smoke test — validates the learned VAD end-to-end and that
// AsrSegmenter segments real speech when driven by it (RFC #4333 follow-up).
//
// Env-gated like asr_whisper_backend_test (needs the ~2 MB model + a 16 kHz mono
// speech WAV, neither in-repo):
//   AETHER_VAD_TEST_MODEL=/path/silero_vad.onnx
//   AETHER_VAD_TEST_WAV=/path/speech16k.wav   (16 kHz mono 16-bit PCM)
// Unset → prints SKIP and passes.

#include "asr/AsrSegmenter.h"
#include "asr/SileroVad.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <vector>

using namespace AetherSDR;

namespace {
int g_failures = 0;
void expect(bool cond, const char* what)
{
    std::printf("%s %s\n", cond ? "[ OK ]" : "[FAIL]", what);
    if (!cond) ++g_failures;
}

std::vector<float> readWav16(const char* path)
{
    std::ifstream f(path, std::ios::binary);
    f.seekg(44); // standard 44-byte PCM header
    std::vector<float> out;
    int16_t s;
    while (f.read(reinterpret_cast<char*>(&s), sizeof(s))) out.push_back(s / 32768.0f);
    return out;
}
} // namespace

int main()
{
    const char* model = std::getenv("AETHER_VAD_TEST_MODEL");
    const char* wav = std::getenv("AETHER_VAD_TEST_WAV");
    if (model == nullptr || wav == nullptr) {
        std::printf("SKIP: set AETHER_VAD_TEST_MODEL and AETHER_VAD_TEST_WAV to run\n");
        return 0;
    }

    SileroVad vad;
    expect(vad.load(model), "Silero model loads");
    if (!vad.isLoaded()) return 1; // ONNX absent or bad model

    const std::vector<float> speech = readWav16(wav);
    expect(speech.size() > 16000, "speech sample read (>1 s)");

    // Mean speech probability over silence vs. over the speech clip.
    auto meanProb = [&](const float* p, int n) {
        vad.reset();
        double sum = 0;
        int k = 0;
        for (int i = 0; i + SileroVad::kWindowSamples <= n; i += SileroVad::kWindowSamples) {
            vad.isSpeech(p + i, SileroVad::kWindowSamples);
            sum += vad.lastProbability();
            ++k;
        }
        return k ? sum / k : 0.0;
    };
    const std::vector<float> silence(16000, 0.0f);
    const double pSil = meanProb(silence.data(), (int)silence.size());
    const double pSpeech = meanProb(speech.data(), (int)speech.size());
    std::printf("       mean prob: silence=%.3f  speech=%.3f\n", pSil, pSpeech);
    expect(pSil < 0.2, "silence probability is low");
    expect(pSpeech > 0.5, "speech probability is high");

    // The segmenter, driven by Silero, closes at least one utterance for speech
    // bracketed by silence.
    std::vector<float> sig(silence);
    sig.insert(sig.end(), speech.begin(), speech.end());
    sig.insert(sig.end(), silence.begin(), silence.end());

    SileroVad vad2;
    vad2.load(model);
    AsrSegmenter seg{AsrSegmenter::Config{}};
    seg.setVad(&vad2);
    auto segs = seg.feed(sig.data(), (int)sig.size());
    auto tail = seg.flush();
    segs.insert(segs.end(), tail.begin(), tail.end());
    expect(!segs.empty(), "Silero-driven segmenter emits an utterance");

    std::printf(g_failures == 0 ? "\nSilero VAD: ALL PASS\n" : "\nSilero VAD: %d FAILURE(S)\n",
                g_failures);
    return g_failures == 0 ? 0 : 1;
}
