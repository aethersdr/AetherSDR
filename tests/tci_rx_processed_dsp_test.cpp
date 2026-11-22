// RFC #6864: the per-slice processed-RX tap in AudioEngine.
//
// Socket-free. Drives AudioEngine::processTciSliceAudio() with injected PCM
// and reads tciSliceProcessedAudioReady. Covers: one tap per slice, nothing
// during TX, nothing for misaligned input, and release per slice and for all.
// The raw/processed source switch and the 500 ms fallback are covered at the
// TCI route level in tci_rx_audio_test.
#include "core/AudioEngine.h"

#include <QCoreApplication>
#include <QList>
#include <QPair>
#include <cmath>
#include <cstdio>

using namespace AetherSDR;

namespace {

int failures = 0;

void check(bool condition, const char* claim)
{
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", claim);
    failures += !condition;
}

constexpr int kFrames = 480; // 20 ms at 24 kHz

QByteArray toneBlock(int block)
{
    QByteArray pcm(kFrames * 2 * static_cast<int>(sizeof(float)), Qt::Uninitialized);
    auto* s = reinterpret_cast<float*>(pcm.data());
    for (int i = 0; i < kFrames; ++i) {
        const double t = static_cast<double>(block * kFrames + i) / 24000.0;
        const float v = 0.25f * static_cast<float>(std::sin(2.0 * 3.14159265358979323846 * 700.0 * t));
        s[2 * i] = v;
        s[2 * i + 1] = v;
    }
    return pcm;
}

void perSliceTap()
{
    AudioEngine audio;
    QList<QPair<int, int>> emitted; // slice, bytes
    QObject::connect(&audio, &AudioEngine::tciSliceProcessedAudioReady,
                     [&emitted](int slice, const QByteArray& pcm) { emitted.append({slice, int(pcm.size())}); });

    check(audio.tciSliceDspCount() == 0, "no slice tap exists before any audio");
    audio.processTciSliceAudio(3, toneBlock(0));
    audio.processTciSliceAudio(5, toneBlock(0));
    check(audio.tciSliceDspCount() == 2, "each slice gets its own processing state");
    check(emitted.size() == 2 && emitted[0].first == 3 && emitted[1].first == 5,
          "processed audio comes back tagged with its own slice");
    check(!emitted.isEmpty() && emitted[0].second % (2 * int(sizeof(float))) == 0,
          "processed audio is whole 24 kHz stereo frames");
    check(audio.tciSliceDspBlocks() == 2, "the block counter follows the emitted blocks");

    const int before = emitted.size();
    QByteArray odd = toneBlock(1);
    odd.chop(4);
    audio.processTciSliceAudio(3, odd);
    audio.processTciSliceAudio(3, QByteArray());
    audio.processTciSliceAudio(-1, toneBlock(1));
    check(emitted.size() == before, "misaligned, empty or slice-less input emits nothing");

    audio.setRadioTransmitting(true, true);
    audio.processTciSliceAudio(3, toneBlock(2));
    check(emitted.size() == before, "nothing is emitted during TX, so raw takes over");
    audio.setRadioTransmitting(false, false);
    audio.processTciSliceAudio(3, toneBlock(3));
    check(emitted.size() == before + 1, "processing resumes after TX");

    audio.releaseTciSliceDsp(3);
    check(audio.tciSliceDspCount() == 1, "releasing one slice keeps the others");
    audio.releaseTciSliceDsp(-1);
    check(audio.tciSliceDspCount() == 0, "releasing all drops every slice tap");
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    perSliceTap();
    return failures ? 1 : 0;
}
