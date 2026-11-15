// Real-inference test for WhisperAsrBackend (RFC #4333, Phase 3).
//
// Weights are download-on-demand, so CI has no model and this test SKIPS
// (exit 0) unless both env vars are set:
//   AETHER_ASR_TEST_MODEL = path to a ggml whisper model (e.g. ggml-base.bin)
//   AETHER_ASR_TEST_PCM   = path to raw 32-bit float, mono, 16 kHz PCM of speech
// When set, it loads the model, transcribes the clip, and asserts non-empty
// text — the end-to-end "fixture clip -> text" check. Produce a PCM fixture with:
//   ffmpeg -i clip.wav -f f32le -ac 1 -ar 16000 clip.pcm

#include "asr/WhisperAsrBackend.h"

#include <QByteArray>
#include <QFile>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace AetherSDR;

int main()
{
    const char* modelPath = std::getenv("AETHER_ASR_TEST_MODEL");
    const char* pcmPath = std::getenv("AETHER_ASR_TEST_PCM");
    if (modelPath == nullptr || pcmPath == nullptr || modelPath[0] == '\0'
        || pcmPath[0] == '\0') {
        std::printf("[SKIP] set AETHER_ASR_TEST_MODEL and AETHER_ASR_TEST_PCM "
                    "(raw f32 mono 16k) to run real whisper inference\n");
        return 0;
    }

    WhisperAsrBackend backend;
    QString error;
    if (!backend.load(QString::fromLocal8Bit(modelPath), &error)) {
        std::fprintf(stderr, "[FAIL] load: %s\n", qPrintable(error));
        return 1;
    }

    QFile pcm(QString::fromLocal8Bit(pcmPath));
    if (!pcm.open(QIODevice::ReadOnly)) {
        std::fprintf(stderr, "[FAIL] cannot open PCM: %s\n", qPrintable(pcm.errorString()));
        return 1;
    }
    const QByteArray raw = pcm.readAll();
    std::vector<float> samples(raw.size() / sizeof(float));
    std::memcpy(samples.data(), raw.constData(), samples.size() * sizeof(float));

    const AsrTranscript result = backend.transcribe(samples, &error);
    if (!error.isEmpty()) {
        std::fprintf(stderr, "[FAIL] transcribe: %s\n", qPrintable(error));
        return 1;
    }
    if (result.text.trimmed().isEmpty()) {
        std::fprintf(stderr, "[FAIL] empty transcription for %zu samples\n", samples.size());
        return 1;
    }

    std::printf("[ OK ] transcribed %zu samples (confidence %.2f) -> \"%s\"\n",
                samples.size(), result.confidence, qPrintable(result.text));
    return 0;
}
