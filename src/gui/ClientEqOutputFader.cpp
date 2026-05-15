#include "ClientEqOutputFader.h"

#include <QContextMenuEvent>
#include <QInputDialog>
#include <QLabel>
#include <QLinearGradient>
#include <QMenu>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>

namespace AetherSDR {

namespace {

float linearToDb(float linear)
{
    if (linear <= 1e-6f) return -120.0f;
    return 20.0f * std::log10(linear);
}

float dbToLinear(float db)
{
    return std::pow(10.0f, db / 20.0f);
}

// Attack / release for the displayed peak — fast rise, slow fall so the
// bar tracks transients but doesn't flicker.
constexpr float kPeakAttack  = 0.6f;
constexpr float kPeakRelease = 0.08f;

} // namespace

ClientEqOutputFader::ClientEqOutputFader(QWidget* parent) : QWidget(parent)
{
    // Total width = label column + gap + bar + overhang each side.
    setFixedWidth(kLabelColW + kGap + kBarW + kHandleOverhang * 2 + 2);
    setMinimumHeight(160);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    setFocusPolicy(Qt::ClickFocus);
    setCursor(Qt::ArrowCursor);
    setMouseTracking(false);
    setToolTip(
        "Output gain (dB). Drag to set, wheel for fine step,\n"
        "double-click to reset to 0 dB.");

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 2, 0, 2);
    root->setSpacing(0);

    auto* top = new QLabel("OUT");
    top->setAlignment(Qt::AlignCenter);
    top->setStyleSheet(
        "QLabel { color: #8aa8c0; font-size: 10px; font-weight: bold;"
        " background: transparent; border: none; }");
    root->addWidget(top);

    root->addStretch(1);

    m_valueLabel = new QLabel;
    m_valueLabel->setAlignment(Qt::AlignCenter);
    m_valueLabel->setStyleSheet(
        "QLabel { color: #d7e7f2; font-size: 10px; font-weight: bold;"
        " background: transparent; border: none; }");
    root->addWidget(m_valueLabel);

    refreshValueLabel();
}

void ClientEqOutputFader::setGainLinear(float linear)
{
    m_gain = std::clamp(linear, 0.0f, 4.0f);
    refreshValueLabel();
    update();
}

void ClientEqOutputFader::setPeakLinear(float peakLinear)
{
    const float peakDb = linearToDb(std::max(peakLinear, 1e-6f));
    const float alpha = (peakDb > m_smoothedPeak) ? kPeakAttack : kPeakRelease;
    m_smoothedPeak += alpha * (peakDb - m_smoothedPeak);
    update();
}

void ClientEqOutputFader::refreshValueLabel()
{
    const float db = linearToDb(m_gain);
    if (db <= kGainMinDb + 0.05f) {
        m_valueLabel->setText("-inf");
    } else {
        m_valueLabel->setText(QString::asprintf("%+.1f dB", db));
    }
}

void ClientEqOutputFader::setGainFromY(int y)
{
    const float norm = 1.0f - std::clamp(
        static_cast<float>(y - m_stripTop) / std::max(1, m_stripH), 0.0f, 1.0f);
    const float db = kGainMinDb + norm * (kGainMaxDb - kGainMinDb);
    m_gain = dbToLinear(db);
    refreshValueLabel();
    emit gainChanged(m_gain);
    update();
}

void ClientEqOutputFader::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);
    p.setRenderHint(QPainter::TextAntialiasing, true);

    // The strip lives between the OUT label at the top and the value
    // label at the bottom — paintEvent receives the full widget rect and
    // we carve out a fixed vertical band in the middle.
    const int topLabelH = 16;
    const int botLabelH = 14;
    const int stripTop  = topLabelH + kStripTopPad;
    const int stripBot  = height() - botLabelH - kStripBottomPad;
    m_stripTop = stripTop;
    m_stripH   = std::max(1, stripBot - stripTop);

    const int barLeft = kLabelColW + kGap + kHandleOverhang;
    const QRect barR(barLeft, stripTop, kBarW, m_stripH);

    // Background for the bar — dark inset.
    p.fillRect(barR, QColor("#06111c"));

    // Level fill — gradient bottom green → top red, clipped to the level
    // height derived from the smoothed peak.
    const float peakNorm = std::clamp(
        (m_smoothedPeak - kMeterMinDb) / (kMeterMaxDb - kMeterMinDb),
        0.0f, 1.0f);
    const int fillH = static_cast<int>(peakNorm * m_stripH);
    if (fillH > 0) {
        const QRect fill(barR.x(), barR.y() + m_stripH - fillH,
                         kBarW, fillH);
        QLinearGradient grad(0, barR.y() + m_stripH, 0, barR.y());
        grad.setColorAt(0.0, QColor("#2f9e6a"));   // green bottom
        grad.setColorAt(0.55, QColor("#6cc56a"));  // lime
        grad.setColorAt(0.80, QColor("#e8b94c"));  // amber
        grad.setColorAt(0.95, QColor("#e8553c"));  // red top
        grad.setColorAt(1.0, QColor("#f2362a"));
        p.fillRect(fill, grad);
    }

    // Bar outline.
    p.setPen(QPen(QColor("#243a4e"), 1));
    p.setBrush(Qt::NoBrush);
    p.drawRect(barR.adjusted(0, 0, -1, -1));

    // dB scale labels + tick marks on the left side.
    QFont f = p.font();
    f.setPixelSize(8);
    p.setFont(f);
    const QFontMetrics fm(f);
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
        const int y = stripTop + static_cast<int>((1.0f - norm) * m_stripH);

        p.setPen(QColor("#7f93a5"));
        const QString s = QString::fromLatin1(t.label);
        const int tw = fm.horizontalAdvance(s);
        const int ty = std::clamp(y + fm.ascent() / 2 - 1,
                                  stripTop + fm.ascent() - 1,
                                  stripTop + m_stripH - 1);
        p.drawText(textRight - tw, ty, s);

        p.setPen(QColor("#405060"));
        p.drawLine(textRight, y, barLeft - 1, y);
    }

    // Fader handle — horizontal bar that overhangs the meter on both
    // sides so it's easy to grab without covering the level colour.
    const float gainDb = std::clamp(linearToDb(m_gain),
                                    kGainMinDb, kGainMaxDb);
    const float gainNorm = (kGainMaxDb - gainDb) / (kGainMaxDb - kGainMinDb);
    const int handleY = stripTop + static_cast<int>(gainNorm * m_stripH);
    const QRect handleR(barLeft - kHandleOverhang,
                        handleY - kHandleH / 2,
                        kBarW + kHandleOverhang * 2,
                        kHandleH);
    p.setPen(QPen(QColor("#0a1a28"), 1));
    p.setBrush(QColor("#d7e7f2"));   // cream / bright off-white
    p.drawRect(handleR);
    // Centre line — a single pixel on the handle so the exact gain level
    // reads clearly against the bar's colour.
    p.setPen(QColor("#1a2a3a"));
    p.drawLine(handleR.left() + 1, handleY,
               handleR.right() - 1, handleY);

    Q_UNUSED(barR);
}

void ClientEqOutputFader::mousePressEvent(QMouseEvent* ev)
{
    if (ev->button() == Qt::LeftButton) {
        m_dragging = true;
        setCursor(Qt::ClosedHandCursor);
        setGainFromY(ev->pos().y());
        ev->accept();
        return;
    }
    QWidget::mousePressEvent(ev);
}

void ClientEqOutputFader::mouseMoveEvent(QMouseEvent* ev)
{
    if (m_dragging) {
        setGainFromY(ev->pos().y());
        ev->accept();
        return;
    }
    QWidget::mouseMoveEvent(ev);
}

void ClientEqOutputFader::mouseReleaseEvent(QMouseEvent* ev)
{
    if (m_dragging && ev->button() == Qt::LeftButton) {
        m_dragging = false;
        setCursor(Qt::ArrowCursor);
        ev->accept();
        return;
    }
    QWidget::mouseReleaseEvent(ev);
}

void ClientEqOutputFader::mouseDoubleClickEvent(QMouseEvent* ev)
{
    if (ev->button() == Qt::LeftButton) {
        m_gain = 1.0f;  // 0 dB
        refreshValueLabel();
        emit gainChanged(m_gain);
        update();
        ev->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(ev);
}

void ClientEqOutputFader::wheelEvent(QWheelEvent* ev)
{
    // 0.5 dB per notch (12 notches for a full deg of the wheel).
    const int notches = ev->angleDelta().y() / 120;
    if (notches == 0) { QWidget::wheelEvent(ev); return; }
    const float db = std::clamp(linearToDb(m_gain) + 0.5f * notches,
                                kGainMinDb, kGainMaxDb);
    m_gain = dbToLinear(db);
    refreshValueLabel();
    emit gainChanged(m_gain);
    update();
    ev->accept();
}

void ClientEqOutputFader::contextMenuEvent(QContextMenuEvent* ev)
{
    // Right-click → numeric entry.  Drag / double-click reset / wheel
    // remain on the left button so the existing gesture set is intact.
    QMenu menu(this);
    QAction* enterAct = menu.addAction(tr("Enter output gain (dB)…"));
    QAction* resetAct = menu.addAction(tr("Reset to 0 dB"));
    QAction* chosen = menu.exec(ev->globalPos());
    if (chosen == enterAct) {
        bool ok = false;
        const double currentDb =
            std::clamp(linearToDb(m_gain), kGainMinDb, kGainMaxDb);
        const double val = QInputDialog::getDouble(
            this,
            tr("Output Gain"),
            tr("Gain (%1 to %2 dB):")
                .arg(kGainMinDb, 0, 'f', 0)
                .arg(kGainMaxDb, 0, 'f', 0),
            currentDb,
            static_cast<double>(kGainMinDb),
            static_cast<double>(kGainMaxDb),
            1, &ok);
        if (ok) {
            const float db = std::clamp(static_cast<float>(val),
                                        kGainMinDb, kGainMaxDb);
            m_gain = dbToLinear(db);
            refreshValueLabel();
            emit gainChanged(m_gain);
            update();
        }
    } else if (chosen == resetAct) {
        m_gain = 1.0f;
        refreshValueLabel();
        emit gainChanged(m_gain);
        update();
    }
    ev->accept();
}

} // namespace AetherSDR
