// Manual smoke test for the Apple live-preview bridge (macOS 26+): feeds a
// 16 kHz mono float32 WAV through ILivePreview at real-time pace in 20 ms
// chunks and prints every result with the delay since its audio ended, and
// each final's confidence. Fails unless every finalizeThrough is acknowledged
// and no final arrives after the acknowledgement that covers it (the "None"
// model commits on that acknowledgement). Needs a real recording and the
// on-device model, so it is not a CTest.
//
//   apple_live_preview_smoke <file.16k.wav> [seconds] [language]

#include "asr/LivePreview.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QThread>

#include <cstdio>
#include <algorithm>
#include <cstring>
#include <mutex>
#include <vector>

using namespace AetherSDR;

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <file.16k.wav> [seconds] [language]\n", argv[0]);
        return 2;
    }
    if (!appleLivePreviewSupported()) {
        std::fprintf(stderr, "unsupported: %s\n",
                     qPrintable(appleLivePreviewUnavailableReason()));
        return 77;
    }
    QFile f(QString::fromLocal8Bit(argv[1]));
    if (!f.open(QIODevice::ReadOnly)) {
        std::fprintf(stderr, "cannot open %s\n", argv[1]);
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
    const double seconds = argc > 2 ? std::atof(argv[2]) : 30.0;
    pcm.resize(std::min(pcm.size(), static_cast<size_t>(seconds * 16000)));

    std::mutex mu;
    QElapsedTimer clock;
    bool ready = false;
    int results = 0;
    int acks = 0;
    int lateFinals = 0;  // finals that arrived after an acknowledgement covering them
    qint64 acked = -1;
    LivePreviewCallbacks cb;
    cb.onResult = [&](const LivePreviewResult& r) {
        const std::lock_guard<std::mutex> lock(mu);
        ++results;
        if (r.isFinal && !r.text.trimmed().isEmpty() && r.startSample < acked) {
            ++lateFinals;
        }
        std::printf("%7.2f s  %s [%6.2f-%6.2f] %s%s\n", clock.elapsed() / 1000.0,
                    r.isFinal ? "FINAL" : "prov ", r.startSample / 16000.0,
                    r.endSample / 16000.0, qPrintable(r.text),
                    r.isFinal ? qPrintable(QStringLiteral("  (confidence %1)")
                                               .arg(double(r.confidence), 0, 'f', 2))
                              : "");
        std::fflush(stdout);
    };
    cb.onFinalized = [&](qint64 sample) {
        const std::lock_guard<std::mutex> lock(mu);
        ++acks;
        acked = std::max(acked, sample);
        std::printf("%7.2f s  finalized through %.2f\n", clock.elapsed() / 1000.0,
                    sample / 16000.0);
        std::fflush(stdout);
    };
    cb.onState = [&](LivePreviewState s, const QString& m) {
        const std::lock_guard<std::mutex> lock(mu);
        ready = ready || s == LivePreviewState::Ready;
        std::printf("state %d %s\n", static_cast<int>(s), qPrintable(m));
        std::fflush(stdout);
    };
    std::unique_ptr<ILivePreview> preview =
        appleLivePreviewFactory(argc > 3 ? QString::fromLocal8Bit(argv[3]) : QStringLiteral("en"))(cb);
    if (!preview) {
        std::fprintf(stderr, "factory returned nothing\n");
        return 1;
    }
    for (int i = 0; i < 200; ++i) {
        {
            const std::lock_guard<std::mutex> lock(mu);
            if (ready) {
                break;
            }
        }
        QThread::msleep(25);
    }
    clock.start();
    constexpr int kChunk = 320; // 20 ms
    qint64 lastFinalize = 0;
    int requested = 0;
    for (size_t off = 0; off < pcm.size(); off += kChunk) {
        const int n = static_cast<int>(std::min<size_t>(kChunk, pcm.size() - off));
        const qint64 dueMs = static_cast<qint64>((off + n) / 16);
        while (clock.elapsed() < dueMs) {
            QThread::msleep(1);
        }
        preview->feed(pcm.data() + off, n, static_cast<qint64>(off));
        if (static_cast<qint64>(off + n) - lastFinalize >= 10 * 16000) {
            lastFinalize = static_cast<qint64>(off + n);
            preview->finalizeThrough(lastFinalize);
            ++requested;
        }
    }
    preview->finalizeThrough(static_cast<qint64>(pcm.size()));
    ++requested;
    // Apple finalises through a position only once audio past it arrives, as
    // it always does in the app: keep feeding silence for 2 s.
    const std::vector<float> quiet(kChunk, 0.0f);
    for (qint64 at = static_cast<qint64>(pcm.size()); at < qint64(pcm.size()) + 32000;
         at += kChunk) {
        while (clock.elapsed() < (at + kChunk) / 16) {
            QThread::msleep(1);
        }
        preview->feed(quiet.data(), kChunk, at);
    }
    QThread::msleep(500);
    preview.reset(); // no callback may run after this
    std::printf("done: %d results, %d/%d finalize requests acknowledged, %d finals after "
                "their acknowledgement\n",
                results, acks, requested, lateFinals);
    return results > 0 && acks == requested && lateFinals == 0 ? 0 : 1;
}
