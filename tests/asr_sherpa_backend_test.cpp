// End-to-end test for SherpaOnnxBackend. Env-gated like the other model tests
// (needs a sherpa-onnx offline model directory + a 16 kHz mono WAV, neither
// in-repo):
//   AETHER_SHERPA_TEST_DIR=/path/to/sherpa-onnx-model-dir
//   AETHER_SHERPA_TEST_WAV=/path/speech16k.wav
// Unset → prints SKIP and passes. Only built when sherpa-onnx is available.
#include "asr/SherpaOnnxBackend.h"

#include <QString>

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
    f.seekg(44);
    std::vector<float> out;
    int16_t s;
    while (f.read(reinterpret_cast<char*>(&s), sizeof(s))) out.push_back(s / 32768.0f);
    return out;
}
} // namespace

int main()
{
    const char* dir = std::getenv("AETHER_SHERPA_TEST_DIR");
    const char* wav = std::getenv("AETHER_SHERPA_TEST_WAV");
    if (dir == nullptr || wav == nullptr) {
        std::printf("SKIP: set AETHER_SHERPA_TEST_DIR and AETHER_SHERPA_TEST_WAV to run\n");
        return 0;
    }

    expect(sherpaOnnxAvailable(), "sherpa-onnx compiled in");

    SherpaOnnxBackend backend(2);
    QString err;
    expect(backend.load(QString::fromUtf8(dir), &err), "model directory loads");
    if (!backend.isLoaded()) {
        std::printf("       load error: %s\n", err.toUtf8().constData());
        return 1;
    }

    const std::vector<float> pcm = readWav16(wav);
    const AsrTranscript r = backend.transcribe(pcm, &err);
    std::printf("       text: \"%s\"\n", r.text.toUtf8().constData());
    expect(!r.text.isEmpty(), "transcription is non-empty");
    expect(r.confidence > 0.0f, "confidence is set");

    std::printf(g_failures == 0 ? "\nsherpa-onnx backend: ALL PASS\n"
                                : "\nsherpa-onnx backend: %d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
