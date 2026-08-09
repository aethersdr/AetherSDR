// Unit tests for fractional-octave smoothing on ClientEqCurveWidget.
//
// The smoothing function is exposed as a free static method, and the
// cache assertions render a real widget through Qt's offscreen platform.

#include "core/ClientEq.h"
#include "gui/ClientEqCurveWidget.h"

#include <QApplication>
#include <QImage>
#include <QPainter>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <vector>

using AetherSDR::ClientEqCurveWidget;
using AetherSDR::ClientEq;

namespace {

int g_failed = 0;

void report(const char* name, bool ok)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", name);
    if (!ok) ++g_failed;
}

bool nearlyEq(float a, float b, float tol = 0.01f)
{
    return std::fabs(a - b) < tol;
}

// 1025-bin (kFftSize/2 + 1 for kFftSize=2048) is the production size.
constexpr int kBins = 1025;
constexpr double kSampleRate = 24000.0;

class DerivedEqCanvas final : public ClientEqCurveWidget {
public:
    using ClientEqCurveWidget::ClientEqCurveWidget;
};

void testDerivedCanvasIdentifiesAsBase()
{
    DerivedEqCanvas canvas;
    report("derived EQ canvas inherits ClientEqCurveWidget",
           canvas.inherits("AetherSDR::ClientEqCurveWidget"));
}

void testOffReturnsInputUnchanged()
{
    std::vector<float> input(kBins);
    for (int i = 0; i < kBins; ++i)
        input[i] = -50.0f + (i % 10) * 1.5f;  // arbitrary spiky shape

    auto out = ClientEqCurveWidget::applyFractionalOctaveSmoothing(
        input, kSampleRate, 96);
    bool match = (out.size() == input.size());
    for (size_t i = 0; i < out.size() && match; ++i) {
        if (!nearlyEq(out[i], input[i], 0.001f)) match = false;
    }
    report("Off (1/96) returns input unchanged", match);

    // Out-of-range fraction also no-op (defensive)
    auto out2 = ClientEqCurveWidget::applyFractionalOctaveSmoothing(input, kSampleRate, 0);
    report("fraction=0 returns input unchanged",
           out2.size() == input.size() && nearlyEq(out2[100], input[100]));
}

void testFlatInputStaysFlat()
{
    std::vector<float> flat(kBins, -30.0f);
    for (int frac : {3, 6, 12, 24}) {
        auto out = ClientEqCurveWidget::applyFractionalOctaveSmoothing(
            flat, kSampleRate, frac);
        bool ok = (out.size() == flat.size());
        for (int i = 1; i < kBins && ok; ++i) {
            if (!nearlyEq(out[i], -30.0f, 0.05f)) ok = false;
        }
        char name[64];
        std::snprintf(name, sizeof(name), "flat input stays flat at 1/%d", frac);
        report(name, ok);
    }
}

void testImpulseSpreads()
{
    // Single-bin spike at 1 kHz (~bin 85 at fs=24k, fft=2048 → 11.7 Hz/bin)
    constexpr float floor = -80.0f;
    std::vector<float> impulse(kBins, floor);
    const int spikeBin = 85;
    impulse[spikeBin] = 0.0f;

    auto raw = ClientEqCurveWidget::applyFractionalOctaveSmoothing(
        impulse, kSampleRate, 96);
    auto smoothed = ClientEqCurveWidget::applyFractionalOctaveSmoothing(
        impulse, kSampleRate, 6);

    // Raw has the spike isolated.
    report("impulse: raw has spike at bin",
           raw[spikeBin] > -1.0f && raw[spikeBin - 5] < -70.0f);

    // Smoothed has spike spread to neighbors (raised them above the floor).
    bool spread = (smoothed[spikeBin - 5] > floor + 5.0f
                && smoothed[spikeBin + 5] > floor + 5.0f);
    report("impulse: 1/6 spreads spike to neighbors", spread);

    // Smoothed peak is lower than raw peak (energy distributed over window).
    report("impulse: smoothed peak < raw peak",
           smoothed[spikeBin] < raw[spikeBin]);
}

void testTighterFractionMeansLessSmoothing()
{
    // Construct stairs: every 10th bin is +20 dB, others -40 dB.
    constexpr float lo = -40.0f;
    constexpr float hi = -20.0f;
    std::vector<float> stairs(kBins, lo);
    for (int i = 50; i < kBins; i += 10) stairs[i] = hi;

    auto s3  = ClientEqCurveWidget::applyFractionalOctaveSmoothing(stairs, kSampleRate, 3);
    auto s12 = ClientEqCurveWidget::applyFractionalOctaveSmoothing(stairs, kSampleRate, 12);
    auto s24 = ClientEqCurveWidget::applyFractionalOctaveSmoothing(stairs, kSampleRate, 24);

    // Compute spread (max - min) at high freq where smoothing diff is biggest.
    auto spread = [](const std::vector<float>& v) {
        float mn = v[400], mx = v[400];
        for (int i = 400; i < 600; ++i) {
            mn = std::min(mn, v[i]);
            mx = std::max(mx, v[i]);
        }
        return mx - mn;
    };

    // Heavier smoothing (1/3) flattens the stairs more than light (1/24).
    report("1/3 spread < 1/12 spread (more smoothing)", spread(s3) < spread(s12));
    report("1/12 spread < 1/24 spread (more smoothing)", spread(s12) < spread(s24));
}

void testWindowScalesWithFrequency()
{
    // At higher frequencies a 1/N-octave window covers more bins than at
    // lower frequencies (because bin width is constant but octave width
    // grows linearly with frequency).  Probe with a single-bin spike at
    // two different frequencies and check the smoothed peak is more
    // spread (lower / wider) at the higher frequency.
    constexpr float floor = -80.0f;
    std::vector<float> low(kBins, floor);
    std::vector<float> high(kBins, floor);
    low[20]  = 0.0f;   // ~234 Hz
    high[400] = 0.0f;  // ~4.7 kHz

    auto loSmooth  = ClientEqCurveWidget::applyFractionalOctaveSmoothing(low,  kSampleRate, 6);
    auto hiSmooth  = ClientEqCurveWidget::applyFractionalOctaveSmoothing(high, kSampleRate, 6);

    // Smoothed peak at higher frequency should be lower (more bins to
    // average over → energy spread thinner).
    report("smoothing window scales with freq (high peak < low peak)",
           hiSmooth[400] < loSmooth[20]);
}

void testEmptyInputHandled()
{
    std::vector<float> empty;
    auto out = ClientEqCurveWidget::applyFractionalOctaveSmoothing(
        empty, kSampleRate, 6);
    report("empty input doesn't crash", out.empty());
}

void testIdempotentForOff()
{
    std::vector<float> input(kBins);
    for (int i = 0; i < kBins; ++i)
        input[i] = -40.0f + 0.05f * static_cast<float>(i);

    auto pass1 = ClientEqCurveWidget::applyFractionalOctaveSmoothing(
        input, kSampleRate, 96);
    auto pass2 = ClientEqCurveWidget::applyFractionalOctaveSmoothing(
        pass1, kSampleRate, 96);

    bool same = (pass1.size() == pass2.size());
    for (size_t i = 0; i < pass1.size() && same; ++i) {
        if (!nearlyEq(pass1[i], pass2[i], 0.001f)) same = false;
    }
    report("Off is idempotent", same);
}

void testDbFloorPreserved()
{
    // Silent input at the floor stays close to the floor.
    constexpr float floor = -100.0f;
    std::vector<float> silent(kBins, floor);
    auto out = ClientEqCurveWidget::applyFractionalOctaveSmoothing(
        silent, kSampleRate, 6);

    bool ok = (out.size() == silent.size());
    for (int i = 1; i < kBins && ok; ++i) {
        // Smoothed output mustn't drop to -inf (algorithm has an
        // epsilon floor) or wander significantly above the input floor.
        if (!std::isfinite(out[i]) || out[i] > floor + 0.5f) ok = false;
    }
    report("dB floor preserved on silent input", ok);
}

QImage renderWidget(ClientEqCurveWidget& widget)
{
    const QSize pixelSize(qRound(widget.width() * widget.devicePixelRatioF()),
                          qRound(widget.height() * widget.devicePixelRatioF()));
    QImage image(pixelSize,
                 QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(widget.devicePixelRatioF());
    image.fill(Qt::transparent);
    QPainter painter(&image);
    widget.render(&painter);
    return image;
}

bool imageIsNonblank(const QImage& image)
{
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (image.pixelColor(x, y).alpha() != 0) {
                return true;
            }
        }
    }
    return false;
}

bool imagesMatch(const QImage& first, const QImage& second)
{
    return first.size() == second.size()
        && first.format() == second.format()
        && first.sizeInBytes() == second.sizeInBytes()
        && std::memcmp(first.constBits(), second.constBits(),
                       static_cast<size_t>(first.sizeInBytes())) == 0;
}

qulonglong counter(const QVariantMap& stats, const char* key)
{
    return stats.value(QString::fromLatin1(key)).toULongLong();
}

void testPaintCaches()
{
    ClientEq eq;
    eq.prepare(48000.0);
    eq.setEnabled(true);
    eq.setActiveBandCount(1);
    ClientEq::BandParams band;
    band.freqHz = 1200.0f;
    band.gainDb = 4.0f;
    band.q = 1.2f;
    eq.setBand(0, band);

    ClientEqCurveWidget widget;
    widget.resize(480, 180);
    widget.setEq(&eq);
    widget.setReferenceCurvePreset(QStringLiteral("Heil DX"));
    widget.setSelectedBand(0);
    std::vector<float> fft(kBins, -65.0f);
    fft[100] = -12.0f;
    widget.setFftBinsDb(fft, kSampleRate);
    const QImage firstImage = renderWidget(widget);
    const QImage identicalImage = renderWidget(widget);
    const QVariantMap first = widget.eqstatsSnapshot(false);
    const qulonglong firstBackground = counter(first, "backgroundCacheRebuildCount");
    const qulonglong firstResponse = counter(first, "responseCacheRebuildCount");
    report("first paint builds background cache", firstBackground == 1);
    report("first paint builds response cache", firstResponse == 1);
    report("cached widget render remains nonblank", imageIsNonblank(firstImage));
    report("identical cached composition is pixel-stable",
           imagesMatch(firstImage, identicalImage));
    const qreal dpr = first.value(QStringLiteral("dpr")).toReal();
    const qulonglong expectedCacheBytes = static_cast<qulonglong>(qRound(widget.width() * dpr))
        * static_cast<qulonglong>(qRound(widget.height() * dpr)) * 4ULL;
    report("high-DPI cache preserves device pixels",
           dpr >= 2.0 && counter(first, "backgroundCacheBytes") == expectedCacheBytes
           && counter(first, "responseCacheBytes") == expectedCacheBytes);

    for (int frame = 0; frame < 4; ++frame) {
        fft[100 + frame] = -12.0f;
        widget.setFftBinsDb(fft, kSampleRate);
        (void)renderWidget(widget);
    }
    const QVariantMap fftOnly = widget.eqstatsSnapshot(false);
    report("FFT-only paints reuse background cache",
           counter(fftOnly, "backgroundCacheRebuildCount") == firstBackground);
    report("FFT-only paints reuse response cache",
           counter(fftOnly, "responseCacheRebuildCount") == firstResponse);
    report("FFT-only cache hits are counted",
           counter(fftOnly, "backgroundCacheHits") >= 4
           && counter(fftOnly, "responseCacheHits") >= 4);

    widget.setFilterCutoffs(200, 2800);
    (void)renderWidget(widget);
    const QVariantMap cutoffs = widget.eqstatsSnapshot(false);
    report("filter cutoff rebuilds background only",
           counter(cutoffs, "backgroundCacheRebuildCount") == firstBackground + 1
           && counter(cutoffs, "responseCacheRebuildCount") == firstResponse);

    widget.setReferenceCurvePreset(QStringLiteral("AT&T 1959"));
    (void)renderWidget(widget);
    const QVariantMap reference = widget.eqstatsSnapshot(false);
    report("reference curve rebuilds response cache",
           counter(reference, "responseCacheRebuildCount")
           == counter(cutoffs, "responseCacheRebuildCount") + 1);

    widget.setSelectedBand(-1);
    (void)renderWidget(widget);
    const QVariantMap selection = widget.eqstatsSnapshot(false);
    report("band selection rebuilds response cache",
           counter(selection, "responseCacheRebuildCount")
           == counter(reference, "responseCacheRebuildCount") + 1);

    widget.setShowFilledRegions(false);
    (void)renderWidget(widget);
    const QVariantMap filled = widget.eqstatsSnapshot(false);
    report("filled-region toggle rebuilds response cache",
           counter(filled, "responseCacheRebuildCount")
           == counter(selection, "responseCacheRebuildCount") + 1);

    band.gainDb = -3.0f;
    eq.setBand(0, band);
    widget.update();  // ClientEq has no QObject signal; callers may do this.
    (void)renderWidget(widget);
    const QVariantMap changedBand = widget.eqstatsSnapshot(false);
    report("ClientEq band mutation rebuilds response cache",
           counter(changedBand, "responseCacheRebuildCount")
           == counter(filled, "responseCacheRebuildCount") + 1);

    QFont changedFont = widget.font();
    changedFont.setPointSize(13);
    widget.setFont(changedFont);
    (void)renderWidget(widget);
    const QVariantMap changedFontStats = widget.eqstatsSnapshot(false);
    report("font change rebuilds both text-bearing caches",
           counter(changedFontStats, "backgroundCacheRebuildCount")
           == counter(changedBand, "backgroundCacheRebuildCount") + 1
           && counter(changedFontStats, "responseCacheRebuildCount")
           == counter(changedBand, "responseCacheRebuildCount") + 1);

    widget.resize(640, 220);
    const QImage resizedImage = renderWidget(widget);
    const QVariantMap resized = widget.eqstatsSnapshot(false);
    report("resize rebuilds both caches",
           counter(resized, "backgroundCacheRebuildCount")
           == counter(changedFontStats, "backgroundCacheRebuildCount") + 1
           && counter(resized, "responseCacheRebuildCount")
           == counter(changedFontStats, "responseCacheRebuildCount") + 1);
    report("resized widget render remains nonblank", imageIsNonblank(resizedImage));
}

}  // namespace

int main(int argc, char* argv[])
{
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    qputenv("QT_SCALE_FACTOR", QByteArrayLiteral("2"));
    QApplication app(argc, argv);
    testDerivedCanvasIdentifiesAsBase();
    testOffReturnsInputUnchanged();
    testFlatInputStaysFlat();
    testImpulseSpreads();
    testTighterFractionMeansLessSmoothing();
    testWindowScalesWithFrequency();
    testEmptyInputHandled();
    testIdempotentForOff();
    testDbFloorPreserved();
    testPaintCaches();

    if (g_failed == 0) {
        std::printf("\nAll EQ smoothing tests passed.\n");
        return 0;
    }
    std::printf("\n%d test(s) FAILED.\n", g_failed);
    return 1;
}
