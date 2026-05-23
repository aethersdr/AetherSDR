#include "ClientEqCurveWidget.h"
#include "core/ClientEq.h"

#include <QPainter>
#include <QPaintEvent>
#include <QPainterPath>
#include <QPen>
#include <QFont>
#include <QColor>
#include <array>
#include <cmath>

namespace MasterSDR {

namespace {

// Gridlines at standard audio decades + halves. 20k is the right-hand bound.
constexpr float kMinHz   = 20.0f;
constexpr float kMaxHz   = 20000.0f;
constexpr float kDbRange = 18.0f;   // ±18 dB vertical extent
// Bottom strip showing band-plan-style audio modulation regions
// (E-SSB / SSB / AM-FM).  Reserved at the bottom of the drawing
// rect; freq labels move above it; analyzer + curves clip to
// (h - kAudioBandStripH).  Mirrors ClientEqCurveWidget::kAudioBandStripPx
// for the derived editor canvas's hit-test logic.
constexpr int   kAudioBandStripH = ClientEqCurveWidget::kAudioBandStripPx;

const float kGridFreqs[] = {
    20.0f, 50.0f, 100.0f, 200.0f, 500.0f,
    1000.0f, 2000.0f, 5000.0f, 10000.0f, 20000.0f
};

QString freqLabel(float hz)
{
    if (hz >= 1000.0f) {
        const float k = hz / 1000.0f;
        if (std::fabs(k - std::round(k)) < 0.01f) {
            return QString::number(static_cast<int>(std::round(k))) + "k";
        }
        return QString::number(k, 'f', 1) + "k";
    }
    return QString::number(static_cast<int>(std::round(hz)));
}

// Logic-Pro-style palette across the audio spectrum. Gray at the extremes
// reserves those slots visually for HP / LP slopes; the middle rainbow is
// for peaks and shelves. Interpolated beyond 8 slots (up to kMaxBands=16).
const std::array<QColor, 8> kPalette = {
    QColor("#9aa4ad"),  // gray
    QColor("#e8a35a"),  // amber
    QColor("#e8d65a"),  // yellow
    QColor("#66d19e"),  // green
    QColor("#66c8d1"),  // teal
    QColor("#6b92d6"),  // blue
    QColor("#a888d6"),  // purple
    QColor("#9aa4ad"),  // gray
};

} // namespace

QColor ClientEqCurveWidget::bandColor(int bandIdx)
{
    if (bandIdx < 0) bandIdx = 0;
    // Wrap by modulo so 8..15 reuse 0..7 rather than clamp to gray.
    return kPalette[static_cast<size_t>(bandIdx) % kPalette.size()];
}

ClientEqCurveWidget::ClientEqCurveWidget(QWidget* parent) : QWidget(parent)
{
    setMinimumHeight(80);
    setAttribute(Qt::WA_OpaquePaintEvent, false);
}

void ClientEqCurveWidget::setEq(ClientEq* eq)
{
    m_eq = eq;
    update();
}

void ClientEqCurveWidget::setSelectedBand(int idx)
{
    if (idx == m_selectedBand) return;
    m_selectedBand = idx;
    emit selectedBandChanged(idx);
    update();
}

void ClientEqCurveWidget::setShowFilledRegions(bool on)
{
    if (on == m_showFilled) return;
    m_showFilled = on;
    update();
}

void ClientEqCurveWidget::setFftBinsDb(const std::vector<float>& binsDb,
                                       double sampleRate)
{
    m_fftBinsDb = binsDb;
    m_fftSampleRate = sampleRate > 0.0 ? sampleRate : 24000.0;

    // Peak-hold trail: per-bin running max, decaying ~10 dB/sec at 25 Hz
    // updates so recent resonances stay visible without permanent clutter.
    // Frozen mode skips decay so the trace sticks at the max.  Operates on
    // raw bins so peak-detection is sample-accurate; visual smoothing of
    // the peak trace happens in applySmoothing() below.
    constexpr float kPeakDecayDb = 0.5f;
    constexpr float kPeakFloorDb = -100.0f;
    if (m_peakHoldDb.size() != m_fftBinsDb.size()) {
        m_peakHoldDb.assign(m_fftBinsDb.size(), kPeakFloorDb);
    }
    const float decayStep = m_peakHoldFrozen ? 0.0f : kPeakDecayDb;
    for (size_t i = 0; i < m_fftBinsDb.size(); ++i) {
        const float decayed = m_peakHoldDb[i] - decayStep;
        m_peakHoldDb[i] = std::max(decayed, m_fftBinsDb[i]);
    }

    // Smoothing runs AFTER peak-hold update so both buffers reflect the
    // current frame.  Generates m_fftBinsDbSmoothed and m_peakHoldDbSmoothed.
    applySmoothing();

    update();
}

void ClientEqCurveWidget::setPeakHoldFrozen(bool frozen)
{
    m_peakHoldFrozen = frozen;
}

void ClientEqCurveWidget::setSmoothingOctaveFraction(int n)
{
    if (m_smoothingFraction == n) return;
    m_smoothingFraction = n;
    applySmoothing();
    update();
}

void ClientEqCurveWidget::setFilterCutoffs(int lowHz, int highHz)
{
    if (m_filterLowCutHz == lowHz && m_filterHighCutHz == highHz) return;
    m_filterLowCutHz = lowHz;
    m_filterHighCutHz = highHz;
    update();
}

std::vector<float> ClientEqCurveWidget::applyFractionalOctaveSmoothing(
    const std::vector<float>& binsDb, double sampleRate, int octaveFraction)
{
    const int N = static_cast<int>(binsDb.size());
    if (N < 2 || octaveFraction <= 0 || octaveFraction >= 96)
        return binsDb;

    // Window half-width in octaves: ±1/(2N).
    const double halfOct = 1.0 / (2.0 * static_cast<double>(octaveFraction));
    const double mulHi   = std::exp2( halfOct);
    const double mulLo   = std::exp2(-halfOct);
    // bin i frequency = i * sampleRate / fftSize, where fftSize = 2*(N-1)
    const double binHz   = sampleRate / static_cast<double>((N - 1) * 2);

    // Precompute ln(10)/10 so the inner-loop dB→linear conversion uses
    // std::exp instead of std::pow(10, ...) — typically 3-4× faster.
    // Equivalent: pow(10, x/10) == exp(x * ln(10) / 10).
    constexpr double kLn10Over10 = 0.23025850929940457;

    std::vector<float> out(N, 0.0f);
    out[0] = binsDb[0];
    for (int i = 1; i < N; ++i) {
        const double fc = i * binHz;
        const int jLo = std::max(0,
            static_cast<int>(std::floor(fc * mulLo / binHz)));
        const int jHi = std::min(N - 1,
            static_cast<int>(std::ceil (fc * mulHi / binHz)));

        // Linear-power average → back to dB.  Matches FabFilter Pro-Q
        // / Voxengo SPAN convention.
        double sumLin = 0.0;
        const int span = jHi - jLo + 1;
        for (int j = jLo; j <= jHi; ++j) {
            sumLin += std::exp(static_cast<double>(binsDb[j]) * kLn10Over10);
        }
        const double meanLin = sumLin / static_cast<double>(span);
        out[i] = static_cast<float>(10.0 * std::log10(meanLin + 1e-12));
    }
    return out;
}

void ClientEqCurveWidget::applySmoothing()
{
    if (m_smoothingFraction >= 96 || m_fftBinsDb.size() < 2) {
        m_fftBinsDbSmoothed = m_fftBinsDb;
        m_peakHoldDbSmoothed = m_peakHoldDb;
        return;
    }
    m_fftBinsDbSmoothed = applyFractionalOctaveSmoothing(
        m_fftBinsDb, m_fftSampleRate, m_smoothingFraction);
    // Smooth peak-hold for display too — peak-hold logic still operates
    // on raw bins for max tracking, but the visible trace gets the same
    // smoothing as the live FFT so the user sees a consistent picture.
    m_peakHoldDbSmoothed = applyFractionalOctaveSmoothing(
        m_peakHoldDb, m_fftSampleRate, m_smoothingFraction);
}

float ClientEqCurveWidget::freqToX(float hz) const
{
    const float logMin = std::log10(kMinHz);
    const float logMax = std::log10(kMaxHz);
    const float norm   = (std::log10(std::max(hz, 0.1f)) - logMin) / (logMax - logMin);
    return norm * static_cast<float>(width());
}

float ClientEqCurveWidget::xToFreq(float x) const
{
    const float logMin = std::log10(kMinHz);
    const float logMax = std::log10(kMaxHz);
    const float norm   = std::clamp(x / static_cast<float>(width()), 0.0f, 1.0f);
    return std::pow(10.0f, logMin + norm * (logMax - logMin));
}

float ClientEqCurveWidget::dbToY(float db) const
{
    // Reserve the bottom strip for the audio band-plan band — curves
    // and handles clip above it.
    const float h = static_cast<float>(height() - kAudioBandStripH);
    const float norm = (kDbRange - db) / (2.0f * kDbRange);  // +db = top
    return std::clamp(norm * h, 0.0f, h);
}

float ClientEqCurveWidget::yToDb(float y) const
{
    const float h = static_cast<float>(height() - kAudioBandStripH);
    const float norm = std::clamp(y / h, 0.0f, 1.0f);
    return kDbRange - norm * (2.0f * kDbRange);
}

void ClientEqCurveWidget::paintEvent(QPaintEvent* /*ev*/)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);

    const QRect r = rect();

    // Background — deep navy matching our dark theme.
    p.fillRect(r, QColor("#0a0a18"));

    // Minor grid — dB lines at ±6, ±12 dB.
    {
        QPen pen(QColor("#1a2a38"));
        pen.setWidth(1);
        p.setPen(pen);
        for (float db : { -12.0f, -6.0f, 0.0f, 6.0f, 12.0f }) {
            const float y = dbToY(db);
            p.drawLine(0, static_cast<int>(y), r.width(), static_cast<int>(y));
        }
    }

    // Main freq gridlines.
    {
        QPen pen(QColor("#203040"));
        pen.setWidth(1);
        p.setPen(pen);
        for (float hz : kGridFreqs) {
            const float x = freqToX(hz);
            p.drawLine(static_cast<int>(x), 0, static_cast<int>(x), r.height());
        }
    }

    // 0 dB reference line — slightly brighter.
    {
        QPen pen(QColor("#304050"));
        pen.setWidth(1);
        p.setPen(pen);
        const float y = dbToY(0.0f);
        p.drawLine(0, static_cast<int>(y), r.width(), static_cast<int>(y));
    }

    // TX filter cutoff guides — faint dashed yellow vertical lines at
    // the radio's current Phone low-cut and high-cut values, so the user
    // can see where their EQ shape lands relative to what's actually
    // passed to the radio.  Drawn behind the EQ curves and analyzer.
    // Cutoffs of 0 mean "not set / RX path" — skip drawing.
    if (m_filterLowCutHz > 0 || m_filterHighCutHz > 0) {
        QPen pen(QColor(220, 200, 80, 110));
        pen.setWidth(1);
        pen.setStyle(Qt::DashLine);
        p.setPen(pen);
        if (m_filterLowCutHz > 0) {
            const float x = freqToX(static_cast<float>(m_filterLowCutHz));
            p.drawLine(static_cast<int>(x), 0, static_cast<int>(x), r.height());
        }
        if (m_filterHighCutHz > 0) {
            const float x = freqToX(static_cast<float>(m_filterHighCutHz));
            p.drawLine(static_cast<int>(x), 0, static_cast<int>(x), r.height());
        }
    }

    // Freq labels along the bottom, tiny.
    {
        QFont f = p.font();
        f.setPointSizeF(7.0);
        p.setFont(f);
        p.setPen(QColor("#506070"));
        const int fh = p.fontMetrics().height();
        for (float hz : kGridFreqs) {
            const QString lbl = freqLabel(hz);
            const int w = p.fontMetrics().horizontalAdvance(lbl);
            int x = static_cast<int>(freqToX(hz)) - w / 2;
            x = std::clamp(x, 2, r.width() - w - 2);
            p.drawText(x, r.height() - kAudioBandStripH - 2, lbl);
            (void)fh;
        }
    }

    // Live FFT analyzer — filled gradient showing what's actually flowing
    // through the audio path post-EQ.  Drawn early so every EQ-visual
    // layer sits on top.  Scale: -70 dB → bottom, 0 dB → top.
    // Filled region uses fractional-octave-smoothed bins (m_fftBinsDbSmoothed)
    // so the visual matches the user's smoothing selection.  Peak-hold trace
    // below stays on raw bins so transient peaks aren't masked.
    if (!m_fftBinsDb.empty()) {
        const int bins = static_cast<int>(m_fftBinsDb.size());
        const std::vector<float>& drawBins = (m_fftBinsDbSmoothed.size() == m_fftBinsDb.size())
            ? m_fftBinsDbSmoothed : m_fftBinsDb;
        const float minDb = -70.0f;
        const float maxDb =   0.0f;
        // Clip the analyzer to above the audio band-plan strip.
        const float h = static_cast<float>(r.height() - kAudioBandStripH);
        auto dbfsToY = [&](float db) {
            const float n = (db - minDb) / (maxDb - minDb);
            return (1.0f - std::clamp(n, 0.0f, 1.0f)) * h;
        };

        QPainterPath fftPath;
        fftPath.moveTo(0, h);
        bool  started = false;
        float lastX   = 0.0f;
        for (int i = 1; i < bins; ++i) {
            // bins.size() == fftSize/2 + 1; bin i maps to i * fs / fftSize.
            const float f = static_cast<float>(i) *
                            static_cast<float>(m_fftSampleRate) /
                            static_cast<float>((bins - 1) * 2);
            const float x = freqToX(f);
            if (x < 0 || x > r.width()) continue;
            const float y = dbfsToY(drawBins[i]);
            if (!started) { fftPath.lineTo(x, h); started = true; }
            fftPath.lineTo(x, y);
            lastX = x;
        }
        // Close the filled region at the last valid bin's x, not at the
        // canvas right edge.  Above Nyquist the FFT has no bins; drawing
        // out to r.width() produced a misleading near-horizontal "shelf"
        // connecting the last bin's level to the bottom-right corner.
        if (started) {
            fftPath.lineTo(lastX, h);
        }
        fftPath.closeSubpath();

        QLinearGradient grad(0, 0, 0, h);
        grad.setColorAt(0.0, QColor(88, 200, 232, 140));   // cyan top
        grad.setColorAt(0.6, QColor(30, 110, 170,  70));
        grad.setColorAt(1.0, QColor(12,  40,  70,   0));   // fades to clear
        p.setPen(Qt::NoPen);
        p.setBrush(grad);
        p.drawPath(fftPath);

        // Peak-hold line — same dBFS scale as the live spectrum.  Drawn
        // on top so resonances and harsh peaks stand out as the user
        // tunes.  Soft off-white reads cleanly against the cool-cyan
        // analyzer.  Reads the smoothed peak-hold buffer so changing
        // the Smoothing combo visibly affects the dominant trace.
        const std::vector<float>& peakBins =
            (m_peakHoldDbSmoothed.size() == m_peakHoldDb.size())
                ? m_peakHoldDbSmoothed : m_peakHoldDb;
        if (!peakBins.empty() && peakBins.size() == m_fftBinsDb.size()) {
            QPainterPath peakPath;
            bool peakStarted = false;
            for (int i = 1; i < bins; ++i) {
                const float f = static_cast<float>(i) *
                                static_cast<float>(m_fftSampleRate) /
                                static_cast<float>((bins - 1) * 2);
                const float x = freqToX(f);
                if (x < 0 || x > r.width()) continue;
                const float y = dbfsToY(peakBins[i]);
                if (!peakStarted) { peakPath.moveTo(x, y); peakStarted = true; }
                else              peakPath.lineTo(x, y);
            }
            QPen peakPen(QColor(220, 222, 230, 210), 1.4);
            peakPen.setJoinStyle(Qt::RoundJoin);
            peakPen.setCapStyle(Qt::RoundCap);
            p.setPen(peakPen);
            p.setBrush(Qt::NoBrush);
            p.drawPath(peakPath);
        }
    }

    if (!m_eq || m_eq->activeBandCount() == 0) {
        p.setPen(QColor("#405060"));
        QFont f = p.font();
        f.setPointSizeF(8.0);
        p.setFont(f);
        p.drawText(r, Qt::AlignCenter,
                   m_eq ? QString("(no bands — add one in the editor)")
                        : QString("(no EQ connected)"));
        return;
    }

    const int   bandCount = m_eq->activeBandCount();
    const double fs       = m_eq->sampleRate();
    const int   W         = r.width();
    const bool  eqOn      = m_eq->isEnabled();

    // bandMagnitudeDb evaluates analog-prototype transfer functions in
    // double precision, so the drawn response is ideal across the full
    // 20 Hz - 20 kHz canvas — no aliasing, no Nyquist artefacts, no low-
    // end precision loss. The audio path still uses the real-rate digital
    // biquads; this is the analog reference the biquad approximates.
    // HP/LP bands cascade internally based on slopeDbPerOct and the
    // globally-selected FilterFamily on the bound ClientEq.
    const ClientEq::FilterFamily family = m_eq->filterFamily();
    QVector<float> summed(W, 0.0f);
    QVector<QVector<float>> perBand(bandCount, QVector<float>(W, 0.0f));
    for (int x = 0; x < W; ++x) {
        const float probe = xToFreq(static_cast<float>(x));
        float acc = 0.0f;
        for (int i = 0; i < bandCount; ++i) {
            const auto bp = m_eq->band(i);
            const float dB = ClientEq::bandMagnitudeDb(bp, probe, fs, family);
            perBand[i][x] = dB;
            acc += dB;
        }
        summed[x] = acc;
    }

    // Selected-band highlight bar — vertical translucent stripe that ties
    // the icon row, canvas, and param-row column together.  Drawn before
    // the filled regions so the filled region colour still shows through.
    if (m_selectedBand >= 0 && m_selectedBand < bandCount) {
        const auto bp = m_eq->band(m_selectedBand);
        const float cx = freqToX(bp.freqHz);
        const float stripeWidth = 18.0f;
        QColor stripe = bandColor(m_selectedBand);
        stripe.setAlphaF(0.16f);
        p.fillRect(QRectF(cx - stripeWidth * 0.5f, 0.0f,
                          stripeWidth, static_cast<float>(r.height())),
                   stripe);
    }

    // Filled per-band regions — semi-transparent area between the 0 dB
    // line and each band's response.  Renders the Logic-Pro-style "see
    // what each band is doing" look.  Drawn first so the per-band strokes
    // and summed curve on top stay readable.
    if (m_showFilled) {
        const float yZero = dbToY(0.0f);
        for (int i = 0; i < bandCount; ++i) {
            const auto bp = m_eq->band(i);
            if (!bp.enabled) continue;
            QColor fill = bandColor(i);
            fill.setAlphaF(eqOn ? 0.22f : 0.07f);
            p.setPen(Qt::NoPen);
            p.setBrush(fill);
            QPainterPath path;
            path.moveTo(0, yZero);
            for (int x = 0; x < W; ++x) {
                path.lineTo(x, dbToY(perBand[i][x]));
            }
            path.lineTo(W - 1, yZero);
            path.closeSubpath();
            p.drawPath(path);
        }
    }

    // Per-band curves — thin stroke in the band's palette colour.
    for (int i = 0; i < bandCount; ++i) {
        QColor c = bandColor(i);
        c.setAlphaF(eqOn ? 0.55f : 0.18f);
        QPen pen(c);
        pen.setWidthF(1.0);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        QPainterPath path;
        for (int x = 0; x < W; ++x) {
            const float y = dbToY(perBand[i][x]);
            if (x == 0) path.moveTo(x, y);
            else        path.lineTo(x, y);
        }
        p.drawPath(path);
    }

    // Summed curve — bolder stroke in slightly saturated cyan when enabled,
    // dimmed when bypassed.
    {
        QColor c = eqOn ? QColor("#00b4d8") : QColor("#506070");
        QPen pen(c);
        pen.setWidthF(1.6);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        p.setPen(pen);
        QPainterPath path;
        for (int x = 0; x < W; ++x) {
            const float y = dbToY(summed[x]);
            if (x == 0) path.moveTo(x, y);
            else        path.lineTo(x, y);
        }
        p.drawPath(path);
    }

    // Band handles — small filled circles at each band's (freq, gain).
    // For Peak / shelf types the handle sits on the (freq, gain) point.
    // For HP / LP (which have no user gain), we anchor at 0 dB.
    // Selected band renders a halo ring to match the icon-row highlight.
    p.setRenderHint(QPainter::Antialiasing, true);
    for (int i = 0; i < bandCount; ++i) {
        const auto bp = m_eq->band(i);
        const bool isSlope = (bp.type == ClientEq::FilterType::LowPass
                           || bp.type == ClientEq::FilterType::HighPass);
        const float handleDb = isSlope ? 0.0f : bp.gainDb;
        const QPointF center(freqToX(bp.freqHz), dbToY(handleDb));
        QColor c = bandColor(i);
        if (!bp.enabled || !eqOn) c.setAlphaF(0.35f);

        if (i == m_selectedBand) {
            QColor halo = c; halo.setAlphaF(0.35f);
            p.setBrush(halo);
            p.setPen(Qt::NoPen);
            p.drawEllipse(center, 8.0, 8.0);
        }
        p.setBrush(c);
        p.setPen(QPen(QColor("#08121d"), 1.5));
        p.drawEllipse(center, 4.0, 4.0);
    }

    // Audio band-plan strip — fixed segments along the bottom showing
    // common modulation regions.  Colors and license blend match the
    // panadapter band plan (CW=#3060ff, Phone=#ff8000, Data=#c03030;
    // 0.40 = E,G class blend, 0.20 = E-only blend).  Drawn last so it
    // covers any analyzer / curve content underneath.
    {
        struct Seg {
            float lowHz, highHz;
            QColor color;
            float blend;
            const char* label;
        };
        static const Seg segs[] = {
            {   20.0f,    99.0f, QColor(0x30, 0x60, 0xff), 0.40f, "E-SSB"   },
            {  100.0f,  3000.0f, QColor(0xff, 0x80, 0x00), 0.40f, "SSB"     },
            { 3000.0f,  6000.0f, QColor(0x30, 0x60, 0xff), 0.40f, "E-SSB"   },
            { 6000.0f, 20000.0f, QColor(0xc0, 0x30, 0x30), 0.40f, "AM / FM" },
        };
        const QColor bg(0x0a, 0x0a, 0x14);
        const int stripY = r.height() - kAudioBandStripH;

        QFont stripF = p.font();
        stripF.setPointSize(7);
        stripF.setBold(true);
        p.setFont(stripF);

        for (const auto& seg : segs) {
            const int x1 = static_cast<int>(freqToX(seg.lowHz));
            const int x2 = static_cast<int>(freqToX(seg.highHz));
            if (x2 <= x1) continue;
            QColor fill(
                static_cast<int>(seg.color.red()   * seg.blend + bg.red()   * (1.0f - seg.blend)),
                static_cast<int>(seg.color.green() * seg.blend + bg.green() * (1.0f - seg.blend)),
                static_cast<int>(seg.color.blue()  * seg.blend + bg.blue()  * (1.0f - seg.blend)),
                255);
            p.fillRect(x1, stripY, x2 - x1, kAudioBandStripH, fill);
            p.setPen(QColor(0x0f, 0x0f, 0x1a, 200));
            p.drawLine(x1, stripY, x1, stripY + kAudioBandStripH);
            if (x2 - x1 > 24) {
                p.setPen(Qt::white);
                p.drawText(QRect(x1, stripY, x2 - x1, kAudioBandStripH),
                           Qt::AlignCenter, QString::fromLatin1(seg.label));
            }
        }
    }
}

} // namespace MasterSDR
