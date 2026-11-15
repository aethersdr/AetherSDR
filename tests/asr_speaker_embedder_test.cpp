// End-to-end speaker-embedding smoke test (Fbank + ONNX + clustering). Env-gated
// like asr_silero_vad_test — needs the model + two different-speaker 16 kHz mono
// WAVs, none in-repo:
//   AETHER_SPK_TEST_MODEL=/path/wespeaker_ecapa512.onnx
//   AETHER_SPK_TEST_WAV_A=/path/speakerA_16k.wav
//   AETHER_SPK_TEST_WAV_B=/path/speakerB_16k.wav
// Unset → prints SKIP and passes.
#include "asr/SpeakerClusterer.h"
#include "asr/SpeakerEmbedder.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
    char id[4];
    f.read(id, 4);
    f.ignore(4);
    f.read(id, 4); // RIFF....WAVE
    std::vector<float> out;
    while (f.read(id, 4)) {
        uint32_t sz = 0;
        f.read(reinterpret_cast<char*>(&sz), 4);
        if (std::memcmp(id, "data", 4) == 0) {
            std::vector<int16_t> pcm(sz / 2);
            f.read(reinterpret_cast<char*>(pcm.data()), sz);
            for (int16_t s : pcm) out.push_back(s / 32768.0f);
            break;
        }
        f.ignore(sz);
    }
    return out;
}

float cosine(const std::vector<float>& a, const std::vector<float>& b)
{
    float d = 0;
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) d += a[i] * b[i];
    return d;
}
} // namespace

int main()
{
    const char* model = std::getenv("AETHER_SPK_TEST_MODEL");
    const char* wavA = std::getenv("AETHER_SPK_TEST_WAV_A");
    const char* wavB = std::getenv("AETHER_SPK_TEST_WAV_B");
    if (model == nullptr || wavA == nullptr || wavB == nullptr) {
        std::printf("SKIP: set AETHER_SPK_TEST_MODEL, _WAV_A, _WAV_B to run\n");
        return 0;
    }

    SpeakerEmbedder emb;
    expect(emb.load(model), "speaker model loads");
    if (!emb.isLoaded()) return 1;

    const std::vector<float> a = readWav16(wavA);
    const std::vector<float> b = readWav16(wavB);
    const std::vector<float> eA = emb.embed(a.data(), (int)a.size());
    const std::vector<float> eB = emb.embed(b.data(), (int)b.size());
    expect(!eA.empty() && !eB.empty(), "embeddings produced");
    std::printf("       embedding dim=%d  cross-speaker cosine=%.3f\n",
                (int)eA.size(), cosine(eA, eB));
    expect(cosine(eA, eB) < 0.4f, "two speakers are well separated (cosine < 0.4)");

    SpeakerClusterer clus(0.5f);
    const int la = clus.assign(eA);
    const int lb = clus.assign(eB);
    expect(la != lb, "the two speakers get different labels");
    expect(clus.speakerCount() == 2, "clusterer found two speakers");

    std::printf(g_failures == 0 ? "\nSpeaker embedder: ALL PASS\n"
                                : "\nSpeaker embedder: %d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
