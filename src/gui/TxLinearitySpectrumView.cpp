#include "TxLinearitySpectrumView.h"

#include "core/ThemeManager.h"

#include <QFontMetricsF>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>

#include <algorithm>
#include <cmath>

namespace AetherSDR {
namespace {

constexpr int kLeftGutter = 52;    // dB axis labels
constexpr int kBottomGutter = 26;  // frequency axis labels
constexpr int kTopMargin = 26;     // product labels need headroom
constexpr int kRightMargin = 12;

QString dbcText(double dbc)
{
    return QString::number(dbc, 'f', 1) + QStringLiteral(" dBc");
}

}  // namespace

TxLinearitySpectrumView::TxLinearitySpectrumView(QWidget* parent) : QWidget(parent)
{
    theme::setContainer(this, QStringLiteral("dialog/txLinearity/spectrum"));
    setMinimumSize(420, 240);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setAccessibleName(tr("TX linearity spectrum"));
    setAccessibleDescription(tr("No measurement yet."));
}

void TxLinearitySpectrumView::setDbRange(double topDb, double bottomDb)
{
    if (bottomDb >= topDb) {
        return;
    }
    m_topDb = topDb;
    m_bottomDb = bottomDb;
    update();
}

void TxLinearitySpectrumView::clear()
{
    m_hasResult = false;
    m_result = txlin::LinearityResult{};
    updateAccessibleSummary();
    update();
}

void TxLinearitySpectrumView::clearReferenceResult()
{
    m_hasReference = false;
    m_reference = txlin::LinearityResult{};
    update();
}

void TxLinearitySpectrumView::setReferenceResult(const txlin::LinearityResult& result)
{
    m_reference = result;
    m_hasReference = result.valid;
    update();
}

void TxLinearitySpectrumView::setResult(const txlin::LinearityResult& result)
{
    m_result = result;
    m_hasResult = result.valid;

    if (m_hasResult) {
        m_centreHz = 0.5 * (result.tone1.freqHz + result.tone2.freqHz);
        // Fit the widest product with a margin of one more tone spacing, so an
        // order-7 measurement is never cropped and the operator can see that
        // there is nothing beyond it.
        double reach = result.toneSpacingHz * 1.5;
        for (const txlin::ImdProduct& p : result.products) {
            reach = std::max(reach, std::abs(p.freqHz - m_centreHz) + result.toneSpacingHz);
        }
        m_spanHz = std::min(reach, result.sampleRateHz / 2.0);

        // Autoscale the vertical span to the measurement: top just above the
        // tones, bottom a little below the noise floor. A fixed range either
        // wastes half the plot or crops the products, and the products are the
        // point.
        const double top = std::max(result.tone1.powerDb, result.tone2.powerDb) + 8.0;
        const double bottom = std::min(result.noiseFloorDb - 12.0, top - 40.0);
        m_topDb = top;
        m_bottomDb = bottom;
    }
    updateAccessibleSummary();
    update();
}

void TxLinearitySpectrumView::updateAccessibleSummary()
{
    // Custom-painted content is opaque to a screen reader, so the plot carries a
    // spoken summary of what it draws. Every number here is also in the readout
    // table — this is the plot's own description, not a substitute for it.
    if (!m_hasResult) {
        setAccessibleDescription(m_result.invalidReason.empty()
                                     ? tr("No measurement yet.")
                                     : QString::fromStdString(m_result.invalidReason));
        return;
    }
    QString text = tr("Two tones %1 hertz apart. ")
                       .arg(QString::number(m_result.toneSpacingHz, 'f', 0));
    for (const txlin::ImdProduct& p : m_result.products) {
        if (p.order != 3) {
            continue;
        }
        text += p.limitedByNoiseFloor
                    ? tr("%1 IMD3 below the noise floor. ")
                          .arg(p.upper ? tr("Upper") : tr("Lower"))
                    : tr("%1 IMD3 %2 dB below one tone. ")
                          .arg(p.upper ? tr("Upper") : tr("Lower"))
                          .arg(QString::number(-p.dbcPerTone, 'f', 1));
    }
    text += tr("Instrument noise floor %1 dBFS.")
                .arg(QString::number(m_result.noiseFloorDb, 'f', 1));
    setAccessibleDescription(text);
}

double TxLinearitySpectrumView::freqToX(double hz, const QRectF& plot) const
{
    const double lo = m_centreHz - m_spanHz;
    const double hi = m_centreHz + m_spanHz;
    if (hi <= lo) {
        return plot.left();
    }
    return plot.left() + plot.width() * (hz - lo) / (hi - lo);
}

double TxLinearitySpectrumView::dbToY(double db, const QRectF& plot) const
{
    const double clamped = std::clamp(db, m_bottomDb, m_topDb);
    return plot.top() + plot.height() * (m_topDb - clamped) / (m_topDb - m_bottomDb);
}

void TxLinearitySpectrumView::drawGrid(QPainter& painter, const QRectF& plot) const
{
    ThemeManager& theme = ThemeManager::instance();
    const QColor gridColor = theme.color(this, QStringLiteral("color.background.2"));
    const QColor textColor = theme.color(this, QStringLiteral("color.text.secondary"));

    painter.setPen(QPen(gridColor, 1.0));
    painter.drawRect(plot);

    QFont small = font();
    small.setPointSizeF(std::max(7.0, small.pointSizeF() - 2.0));
    painter.setFont(small);
    const QFontMetricsF metrics(small);

    // dB gridlines every 10 dB, labelled every 20 so the axis stays readable on
    // a short plot.
    const double firstLine = std::ceil(m_bottomDb / 10.0) * 10.0;
    for (double db = firstLine; db <= m_topDb; db += 10.0) {
        const double y = dbToY(db, plot);
        painter.setPen(QPen(gridColor, 1.0, Qt::DotLine));
        painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
        if (std::fmod(std::abs(db), 20.0) < 0.01) {
            painter.setPen(textColor);
            const QString label = QString::number(db, 'f', 0);
            painter.drawText(QPointF(plot.left() - metrics.horizontalAdvance(label) - 6.0,
                                     y + metrics.height() / 3.0),
                             label);
        }
    }

    // Frequency gridlines relative to the two-tone centre — the operator thinks
    // in offsets from the transmission, not in absolute baseband bins.
    const double step = (m_spanHz > 12000.0) ? 5000.0 : ((m_spanHz > 4000.0) ? 2000.0 : 1000.0);
    for (double off = -std::floor(m_spanHz / step) * step; off <= m_spanHz; off += step) {
        const double x = freqToX(m_centreHz + off, plot);
        painter.setPen(QPen(gridColor, 1.0, Qt::DotLine));
        painter.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
        painter.setPen(textColor);
        const QString label = QString::number(off / 1000.0, 'f', off == 0.0 ? 0 : 1);
        painter.drawText(QPointF(x - metrics.horizontalAdvance(label) / 2.0,
                                 plot.bottom() + metrics.height() + 2.0),
                         label);
    }
    painter.setPen(textColor);
    painter.drawText(QPointF(plot.left(), plot.bottom() + 2.0 * metrics.height() + 2.0),
                     tr("kHz from centre"));
}

void TxLinearitySpectrumView::drawTrace(QPainter& painter, const QRectF& plot,
                                        const txlin::LinearityResult& result,
                                        bool dimmed) const
{
    if (result.spectrumDb.empty() || result.binHz <= 0.0) {
        return;
    }
    ThemeManager& theme = ThemeManager::instance();
    QColor traceColor = dimmed ? theme.color(this, QStringLiteral("color.text.secondary"))
                               : theme.color(this, QStringLiteral("color.accent"));
    if (dimmed) {
        traceColor.setAlpha(140);
    }

    const int n = int(result.spectrumDb.size());
    const int centre = n / 2;
    const double loHz = m_centreHz - m_spanHz;
    const double hiHz = m_centreHz + m_spanHz;
    const int loBin = std::clamp(int(std::floor(loHz / result.binHz)) + centre, 0, n - 1);
    const int hiBin = std::clamp(int(std::ceil(hiHz / result.binHz)) + centre, 0, n - 1);

    // More bins than pixels: draw the per-pixel MAXIMUM rather than sampling.
    // Sampling would alias a narrow product straight out of the picture, which
    // for this instrument means the operator sees a clean signal that is not.
    const int columns = std::max(1, int(plot.width()));
    std::vector<double> peak(std::size_t(columns), -1e30);
    for (int k = loBin; k <= hiBin; ++k) {
        const double hz = double(k - centre) * result.binHz;
        const int col = std::clamp(int(freqToX(hz, plot) - plot.left()), 0, columns - 1);
        peak[std::size_t(col)] = std::max(peak[std::size_t(col)],
                                          double(result.spectrumDb[std::size_t(k)]));
    }

    QPolygonF poly;
    poly.reserve(columns);
    for (int c = 0; c < columns; ++c) {
        if (peak[std::size_t(c)] < -1e29) {
            continue;
        }
        poly << QPointF(plot.left() + c, dbToY(peak[std::size_t(c)], plot));
    }
    painter.setPen(QPen(traceColor, dimmed ? 1.0 : 1.4));
    painter.setBrush(Qt::NoBrush);
    painter.drawPolyline(poly);
}

void TxLinearitySpectrumView::drawBands(QPainter& painter, const QRectF& plot) const
{
    ThemeManager& theme = ThemeManager::instance();
    QColor shade = theme.color(this, QStringLiteral("color.text.secondary"));
    shade.setAlpha(38);

    const auto shadeBand = [&](const std::optional<txlin::BandPower>& band,
                               const QString& label) {
        if (!band.has_value()) {
            return;
        }
        const double x0 = freqToX(band->lowHz, plot);
        const double x1 = freqToX(band->highHz, plot);
        const QRectF rect(QPointF(x0, plot.top()), QPointF(x1, plot.bottom()));
        painter.fillRect(rect.intersected(plot), shade);
        painter.setPen(theme.color(this, QStringLiteral("color.text.secondary")));
        painter.drawText(QPointF(x0 + 2.0, plot.bottom() - 4.0), label);
    };

    shadeBand(m_result.acprLow, tr("ACPR"));
    shadeBand(m_result.acprHigh, tr("ACPR"));
    shadeBand(m_result.shoulderLow, tr("Shoulder"));
    shadeBand(m_result.shoulderHigh, tr("Shoulder"));
}

void TxLinearitySpectrumView::drawMarkers(QPainter& painter, const QRectF& plot) const
{
    ThemeManager& theme = ThemeManager::instance();
    const QColor toneColor = theme.color(this, QStringLiteral("color.accent"));
    const QColor productColor = theme.color(this, QStringLiteral("color.text.primary"));
    const QColor mutedColor = theme.color(this, QStringLiteral("color.text.secondary"));
    const QColor floorColor = theme.color(this, QStringLiteral("color.accent.warning"));

    QFont small = font();
    small.setPointSizeF(std::max(7.0, small.pointSizeF() - 2.0));
    painter.setFont(small);
    const QFontMetricsF metrics(small);

    // Instrument noise floor. Drawn across the whole plot because it is the
    // line every product has to clear to mean anything — the operator should
    // be able to see at a glance which products are above it.
    const double floorY = dbToY(m_result.noiseFloorDb, plot);
    painter.setPen(QPen(floorColor, 1.0, Qt::DashLine));
    painter.drawLine(QPointF(plot.left(), floorY), QPointF(plot.right(), floorY));
    painter.setPen(floorColor);
    painter.drawText(QPointF(plot.left() + 4.0, floorY - 3.0), tr("noise floor"));

    const auto markTone = [&](const txlin::TonePeak& tone, const QString& label) {
        if (!tone.valid) {
            return;
        }
        const double x = freqToX(tone.freqHz, plot);
        const double y = dbToY(tone.powerDb, plot);
        painter.setPen(QPen(toneColor, 1.0, Qt::DashLine));
        painter.drawLine(QPointF(x, y), QPointF(x, plot.top()));
        painter.setPen(toneColor);
        painter.drawText(QPointF(x - metrics.horizontalAdvance(label) / 2.0,
                                 plot.top() - 4.0),
                         label);
    };
    markTone(m_result.tone1, QStringLiteral("f1"));
    markTone(m_result.tone2, QStringLiteral("f2"));

    for (const txlin::ImdProduct& product : m_result.products) {
        const double x = freqToX(product.freqHz, plot);
        if (x < plot.left() || x > plot.right()) {
            continue;
        }
        const double y = dbToY(product.powerDb, plot);
        // A noise-limited product is drawn muted and labelled with its order
        // only. Printing "-31.4 dBc" beside a peak that is actually the
        // instrument's own noise is the specific mistake this whole feature
        // exists to stop an operator making.
        const bool trusted = !product.limitedByNoiseFloor;
        painter.setPen(QPen(trusted ? productColor : mutedColor, 1.0));
        painter.drawLine(QPointF(x, y), QPointF(x, y - 10.0));

        const QString label =
            trusted ? QStringLiteral("IMD%1  %2").arg(product.order).arg(dbcText(product.dbcPerTone))
                    : tr("IMD%1  (noise)").arg(product.order);
        const double tx = std::clamp(x - metrics.horizontalAdvance(label) / 2.0,
                                     plot.left(), plot.right() - metrics.horizontalAdvance(label));
        painter.drawText(QPointF(tx, y - 13.0), label);
    }
}

void TxLinearitySpectrumView::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    ThemeManager& theme = ThemeManager::instance();
    painter.fillRect(rect(), theme.color(this, QStringLiteral("color.background.0")));

    const QRectF plot(kLeftGutter, kTopMargin,
                      std::max(10, width() - kLeftGutter - kRightMargin),
                      std::max(10, height() - kTopMargin - kBottomGutter));

    if (!m_hasResult) {
        painter.setPen(theme.color(this, QStringLiteral("color.text.secondary")));
        const QString message = m_result.invalidReason.empty()
                                    ? tr("Run a measurement to see the TX spectrum.")
                                    : QString::fromStdString(m_result.invalidReason);
        painter.drawText(rect(), Qt::AlignCenter, message);
        return;
    }

    drawGrid(painter, plot);
    drawBands(painter, plot);
    if (m_hasReference) {
        drawTrace(painter, plot, m_reference, true);
    }
    drawTrace(painter, plot, m_result, false);
    drawMarkers(painter, plot);
}

}  // namespace AetherSDR
