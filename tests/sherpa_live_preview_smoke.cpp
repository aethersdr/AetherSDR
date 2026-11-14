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
// text after it has to be re-decoded. Run C never splits, as the reference
// for what the recogniser hears. Run D splits at the feed every 10 s, like the
// feeder's forcing, and a second later reports an edge 2.5 s before each
// split, as a segment edge arrives late while whisper decodes. Checks:
//   - each run's finals tile the timeline once a final that re-uses an
//     earlier one's start has replaced it, and no provisional result starts
//     before the furthest final's end;
//   - B's finals hold the same number of words as A's, within 5%: a split
//     that kept words from after its edge would repeat them, one that lost the
//     re-fed audio would drop them;
//   - A's and B's words match C's, within 5% word error: a split that cut a
//     word in two ("backgr" + "ound") keeps the count but not the words;
//   - D re-split at least one final and its words match C's within 5%: the
//     words after a late edge move to the next final rather than vanish;
//   - both runs share one factory, and B's session starts in a fifth of the
//     time A's took: the model loads once, not per session.

#include "asr/LivePreview.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
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
    int reissued = 0;   // finals that replaced an earlier one (a late edge split it)
    QString text;
    qint64 startMs = 0; // creating the session (the first one loads the model)
};

// Word-level edit distance between two texts, ignoring case and punctuation.
int wordErrors(const QString& hyp, const QString& ref)
{
    const auto words = [](QString t) {
        t = t.toLower();
        t.remove(QRegularExpression(QStringLiteral("[^a-z0-9' ]")));
        return t.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    };
    const QStringList h = words(hyp);
    const QStringList r = words(ref);
    std::vector<int> row(size_t(h.size()) + 1);
    for (int j = 0; j <= h.size(); ++j) {
        row[size_t(j)] = j;
    }
    for (int i = 1; i <= r.size(); ++i) {
        int diag = row[0];
        row[0] = i;
        for (int j = 1; j <= h.size(); ++j) {
            const int up = row[size_t(j)];
            row[size_t(j)] = std::min({up + 1, row[size_t(j - 1)] + 1,
                                       diag + (r[i - 1] == h[j - 1] ? 0 : 1)});
            diag = up;
        }
    }
    return row[size_t(h.size())];
}

// Splits every `every` samples, lagSamples behind the feed; never when
// lagSamples < 0. With lateEdge > 0, each split is followed, once a second
// more has been fed, by another lateEdge before it: a segment edge reported
// after the 10 s forcing had already split past it.
Run run(const LivePreviewFactory& factory, const std::vector<float>& pcm, qint64 lagSamples,
        qint64 every = 5 * 16000, qint64 lateEdge = 0)
{
    Run out;
    std::map<qint64, std::pair<qint64, QString>> finals; // start -> (end, text)
    qint64 finalEnd = -1;
    LivePreviewCallbacks cb;
    cb.onResult = [&](const LivePreviewResult& r) {
        if (r.isFinal) {
            // A final with the start of an earlier one replaces it; any other
            // starts a new span. Tiling is checked once all have arrived.
            if (finals.count(r.startSample) != 0) {
                ++out.reissued;
            }
            finals[r.startSample] = {r.endSample, r.text};
            finalEnd = std::max(finalEnd, r.endSample);
        } else if (finalEnd >= 0 && r.startSample < finalEnd) {
            out.provisionalInOrder = false;
        }
    };
    cb.onState = [](LivePreviewState s, const QString& m) {
        if (s == LivePreviewState::Failed) {
            std::fprintf(stderr, "failed: %s\n", qPrintable(m));
        }
    };
    QElapsedTimer started;
    started.start();
    std::unique_ptr<ILivePreview> preview = factory(cb);
    out.startMs = started.elapsed();
    if (!preview) {
        std::exit(1);
    }
    constexpr int kChunk = 800; // 50 ms
    qint64 nextSplit = every;
    qint64 lateAt = -1;
    for (size_t off = 0; off < pcm.size(); off += kChunk) {
        const int n = int(std::min<size_t>(kChunk, pcm.size() - off));
        preview->feed(pcm.data() + off, n, qint64(off));
        const qint64 fed = qint64(off) + n;
        if (lagSamples >= 0 && fed >= nextSplit + lagSamples) {
            preview->finalizeThrough(nextSplit);
            if (lateEdge > 0) {
                lateAt = nextSplit;
            }
            nextSplit += every;
        }
        if (lateAt >= 0 && fed >= lateAt + 16000) {
            preview->finalizeThrough(lateAt - lateEdge);
            lateAt = -1;
        }
    }
    preview->finalizeThrough(qint64(pcm.size()));
    // A split waits for a little audio past it, as it would get in the app.
    const std::vector<float> after(16000, 0.0f);
    preview->feed(after.data(), int(after.size()), qint64(pcm.size()));

    qint64 edge = 0;
    for (const auto& [start, f] : finals) {
        out.tiled = out.tiled && start == edge;
        edge = f.first;
        out.finalWords += int(f.second.split(QLatin1Char(' '), Qt::SkipEmptyParts).size());
        out.text += f.second + QLatin1Char(' ');
    }
    out.tiled = out.tiled && edge == qint64(pcm.size());
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

    // One factory for both runs, as the engine keeps one across sessions.
    const LivePreviewFactory factory = sherpaLivePreviewFactory(QString::fromLocal8Bit(argv[1]));
    const Run a = run(factory, pcm, 0);
    const Run b = run(factory, pcm, 24000); // 1.5 s behind the feed
    const Run c = run(factory, pcm, -1);    // never split
    const Run d = run(factory, pcm, 0, 10 * 16000, 40000); // late edges 2.5 s back
    std::printf("session start: A %lld ms (loads the model), B %lld ms\n",
                static_cast<long long>(a.startMs), static_cast<long long>(b.startMs));
    std::printf("A (split at the feed): %d words\n  %s\n", a.finalWords, qPrintable(a.text));
    std::printf("B (split 1.5 s behind): %d words\n  %s\n", b.finalWords, qPrintable(b.text));
    std::printf("C (never split): %d words\n  %s\n", c.finalWords, qPrintable(c.text));
    const int errA = wordErrors(a.text, c.text);
    const int errB = wordErrors(b.text, c.text);
    const int errD = wordErrors(d.text, c.text);
    std::printf("D (late edges): %d words, %d finals re-split\n", d.finalWords, d.reissued);
    std::printf("word errors against C: A %d, B %d, D %d\n", errA, errB, errD);

    expect(a.finalWords > 0, "the recording produced final text");
    expect(a.tiled && b.tiled && c.tiled && d.tiled, "finals tile the timeline in every run");
    expect(a.provisionalInOrder && b.provisionalInOrder && d.provisionalInOrder,
           "no provisional result starts before the last final's end");
    expect(std::abs(b.finalWords - a.finalWords) <= std::max(2, a.finalWords / 20),
           "splitting behind the feed neither repeats nor drops words (within 5%)");
    expect(errA * 20 <= c.finalWords && errB * 20 <= c.finalWords,
           "splitting keeps words whole: A and B match the unsplit run (within 5%)");
    expect(d.reissued > 0 && errD * 20 <= c.finalWords,
           "a late edge re-splits the last final and loses no words (D within 5% of C)");
    expect(b.startMs * 5 < a.startMs,
           "a second session from the same factory reuses the loaded model");
    std::printf(g_failures == 0 ? "sherpa live preview smoke: ALL PASS\n"
                                : "sherpa live preview smoke: %d FAILURE(S)\n",
                g_failures);
    return g_failures == 0 ? 0 : 1;
}
