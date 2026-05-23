#include "ClientLevelMeter.h"

#include <QFontMetrics>
#include <QLabel>
#include <QLinearGradient>
#include <QPaintEvent>
#include <QPainter>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

namespace MasterSDR {

namespace {

float linearToDb(float linear)
{
    if (linear <= 1e-6f) return -120.0f;
    return 20.0f * std::log10(linear);
}

// Same fast-attack / slow-release ballistics as ClientEqOutputFader so
// the meter feels visually consistent across applets.
constexpr float kPeakAttack  = 0.6f;
constexpr float kPeakRelease = 0.08f;

} // namespace

ClientLevelMeter::ClientLevelMeter(QWidget* parent) : QWidget(parent)
{
    setFixedWidth(kLabelColW + kGap + kBarW + 2);
    setMinimumHeight(160);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    setAttribute(Qt::WA_OpaquePaintEvent, false);
}

void ClientLevelMeter::setHeaderText(const QString& text)
{
    if (m_headerText == text) return;
    m_headerText = text;
    update();
}

void ClientLevelMeter::setPeakDb(float peakDb)
{
    const float alpha = (peakDb > m_smoothedPeak) ? kPeakAttack : kPeakRelease;
    m_smoothedPeak += alpha * (peakDb - m_smoothedPeak);
    update();
}

void ClientLevelMeter::setPeakLinear(float peakLinear)
{
    setPeakDb(linearToDb(std::max(peakLinear, 1e-6f)));
}

void ClientLevelMeter::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);
    p.setRenderHint(QPainter::TextAntialiasing, true);

    const int topLabelH = m_headerText.isEmpty() ? 0 : 16;
    const int botLabelH = 14;
    const int stripTop  = topLabelH + kStripTopPad;
    const int stripBot  = height() - botLabelH - kStripBottomPad;
    const int stripH    = std::max(1, stripBot - stripTop);

    const int barLeft = kLabelColW + kGap;
    const QRect barR(barLeft, stripTop, kBarW, stripH);

    // Header text.
    if (!m_headerText.isEmpty()) {
        QFont f = p.font();
        f.setPixelSize(10);
        f.setBold(true);
        p.setFont(f);
        p.setPen(QColor("#8aa8c0"));
        p.drawText(QRect(0, 0, width(), topLabelH),
                   Qt::AlignCenter, m_headerText);
    }

    // Bar background.
    p.fillRect(barR, QColor("#06111c"));

    // Level fill — green→amber→red gradient, height proportional to
    // the smoothed peak in dB.
    const float peakNorm = std::clamp(
        (m_smoothedPeak - kMeterMinDb) / (kMeterMaxDb - kMeterMinDb),
        0.0f, 1.0f);
    const int fillH = static_cast<int>(peakNorm * stripH);
    if (fillH > 0) {
        const QRect fill(barR.x(), barR.y() + stripH - fillH,
                         kBarW, fillH);
        QLinearGradient grad(0, barR.y() + stripH, 0, barR.y());
        grad.setColorAt(0.0,  QColor("#2f9e6a"));   // green bottom
        grad.setColorAt(0.55, QColor("#6cc56a"));   // lime
        grad.setColorAt(0.80, QColor("#e8b94c"));   // amber
        grad.setColorAt(0.95, QColor("#e8553c"));   // red top
        grad.setColorAt(1.0,  QColor("#f2362a"));
        p.fillRect(fill, grad);
    }

    // Bar outline.
    p.setPen(QPen(QColor("#243a4e"), 1));
    p.setBrush(Qt::NoBrush);
    p.drawRect(barR.adjusted(0, 0, -1, -1));

    // dB scale + tick marks on the left.
    QFont tf = p.font();
    tf.setPixelSize(8);
    tf.setBold(false);
    p.setFont(tf);
    const QFontMetrics fm(tf);
    const int textRight = kLabelColW - 2;

    struct Tick { float db; const char* label; };
    static constexpr Tick kTicks[] = {
        {   0.0f,  "0" },
        {  -6.0f,  "-6" },
        { -12.0f,  "-12" },
        { -20.0f,  "-20" },
        { -40.0f,  "-40" },
    };
    for (const auto& t : kTicks) {
        const float norm = (t.db - kMeterMinDb) / (kMeterMaxDb - kMeterMinDb);
        const int y = stripTop + static_cast<int>((1.0f - norm) * stripH);

        p.setPen(QColor("#7f93a5"));
        const QString s = QString::fromLatin1(t.label);
        const int tw = fm.horizontalAdvance(s);
        const int ty = std::clamp(y + fm.ascent() / 2 - 1,
                                  stripTop + fm.ascent() - 1,
                                  stripTop + stripH - 1);
        p.drawText(textRight - tw, ty, s);

        p.setPen(QColor("#405060"));
        p.drawLine(textRight, y, barLeft - 1, y);
    }

    // Numeric readout below the strip.
    QFont vf = p.font();
    vf.setPixelSize(10);
    vf.setBold(true);
    p.setFont(vf);
    p.setPen(QColor("#d7e7f2"));
    const QString readout = (m_smoothedPeak <= kMeterMinDb + 0.5f)
        ? QStringLiteral("-inf")
        : QString::asprintf("%+.1f dB", m_smoothedPeak);
    p.drawText(QRect(0, height() - botLabelH, width(), botLabelH),
               Qt::AlignCenter, readout);
}

} // namespace MasterSDR
