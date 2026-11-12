// Manual smoke test for the sherpa-onnx live preview: feeds a 16 kHz mono
// float32 WAV through ILivePreview in 50 ms chunks (as fast as it decodes)
// twice, and checks the split logic on real speech. Needs a recording and a
// streaming transducer model, so it is not a CTest.
//
//   sherpa_live_preview_smoke <model-dir> <file.16k.wav> [seconds]
//
// Run A finalises at the feed position every 5 s. Run B finalises 5 s apart
// too, but 1.5 s behind the feed, the way segment edges reach the feeder while
// whisper is decoding, so every split falls inside audio already fed and the
// text after it has to be re-decoded. Checks:
//   - each run's finals tile the timeline: every final starts where the last
//     one ended, and no provisional result starts before that edge;
//   - B's finals hold the same number of words as A's, within 5%: a split
//     that kept words from after its edge would repeat them, one that lost the
//     re-fed audio would drop them.

#include "asr/LivePreview.h"

#include <QCoreApplication>
#include <QFile>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace AetherSDR;

namespace {

int g_failures = 0;

void expect(bool condition, const char* description)
{
    std::printf("%s %s\n", condition ? "[ OK ]" : "[FAIL]", description);
    if (!condition) {
        ++g_failures;
    }
}

struct Run {
    int finalWords = 0;
    bool tiled = true;
    bool provisionalInOrder = true;
    QString text;
};

Run run(const QString& modelDir, const std::vector<float>& pcm, qint64 lagSamples)
{
    Run out;
    qint64 edge = -1;
    LivePreviewCallbacks cb;
    cb.onResult = [&](const LivePreviewResult& r) {
        if (r.isFinal) {
            if (edge >= 0 && r.startSample != edge) {
                out.tiled = false;
            }
            edge = r.endSample;
            out.finalWords += int(r.text.split(QLatin1Char(' '), Qt::SkipEmptyParts).size());
            out.text += r.text + QLatin1Char(' ');
        } else if (edge >= 0 && r.startSample < edge) {
            out.provisionalInOrder = false;
        }
    };
    cb.onState = [](LivePreviewState s, const QString& m) {
        if (s == LivePreviewState::Failed) {
            std::fprintf(stderr, "failed: %s\n", qPrintable(m));
        }
    };
    std::unique_ptr<ILivePreview> preview = sherpaLivePreviewFactory(modelDir)(cb);
    if (!preview) {
        std::exit(1);
    }
    constexpr int kChunk = 800; // 50 ms
    constexpr qint64 kEvery = 5 * 16000;
    qint64 nextSplit = kEvery;
    for (size_t off = 0; off < pcm.size(); off += kChunk) {
        const int n = int(std::min<size_t>(kChunk, pcm.size() - off));
        preview->feed(pcm.data() + off, n, qint64(off));
        const qint64 fed = qint64(off) + n;
        if (fed >= nextSplit + lagSamples) {
            preview->finalizeThrough(nextSplit);
            nextSplit += kEvery;
        }
    }
    preview->finalizeThrough(qint64(pcm.size()));
    return out;
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <model-dir> <file.16k.wav> [seconds]\n", argv[0]);
        return 2;
    }
    if (!sherpaLivePreviewSupported()) {
        std::fprintf(stderr, "unsupported: %s\n", qPrintable(sherpaLivePreviewUnavailableReason()));
        return 77;
    }
    QFile f(QString::fromLocal8Bit(argv[2]));
    if (!f.open(QIODevice::ReadOnly)) {
        std::fprintf(stderr, "cannot open %s\n", argv[2]);
        return 1;
    }
    const QByteArray bytes = f.readAll();
    const int dataAt = bytes.indexOf("data");
    if (dataAt < 0) {
        std::fprintf(stderr, "no data chunk\n");
        return 1;
    }
    std::vector<float> pcm((bytes.size() - dataAt - 8) / 4);
    std::memcpy(pcm.data(), bytes.constData() + dataAt + 8, pcm.size() * 4);
    const double seconds = argc > 3 ? std::atof(argv[3]) : 120.0;
    pcm.resize(std::min(pcm.size(), size_t(seconds * 16000)));

    const QString model = QString::fromLocal8Bit(argv[1]);
    const Run a = run(model, pcm, 0);
    const Run b = run(model, pcm, 24000); // 1.5 s behind the feed
    std::printf("A (split at the feed): %d words\n  %s\n", a.finalWords, qPrintable(a.text));
    std::printf("B (split 1.5 s behind): %d words\n  %s\n", b.finalWords, qPrintable(b.text));

    expect(a.finalWords > 0, "the recording produced final text");
    expect(a.tiled && b.tiled, "finals tile the timeline in both runs");
    expect(a.provisionalInOrder && b.provisionalInOrder,
           "no provisional result starts before the last final's end");
    expect(std::abs(b.finalWords - a.finalWords) <= std::max(2, a.finalWords / 20),
           "splitting behind the feed neither repeats nor drops words (within 5%)");
    std::printf(g_failures == 0 ? "sherpa live preview smoke: ALL PASS\n"
                                : "sherpa live preview smoke: %d FAILURE(S)\n",
                g_failures);
    return g_failures == 0 ? 0 : 1;
}
