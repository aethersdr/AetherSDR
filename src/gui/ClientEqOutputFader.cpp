#include "ClientEqOutputFader.h"

#include <QEvent>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLinearGradient>
#include <QLocale>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include "core/ThemeManager.h"

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

    m_endLabel = new QLabel("OUT");
    m_endLabel->setAlignment(Qt::AlignCenter);
    AetherSDR::ThemeManager::instance().applyStyleSheet(m_endLabel, "QLabel { color: {{color.text.secondary}}; font-size: 10px; font-weight: bold;"
        " background: transparent; border: none; }");

    // Inline-editable value at the bottom of the fader.  Click to focus,
    // type a dB value, Enter or focus-out to commit (clamped to range).
    // Looks identical to a label until focused; subtle inset + cyan
    // border on focus indicates edit mode (matches ClientCompKnob).
    m_valueEdit = new QLineEdit;
    m_valueEdit->setAlignment(Qt::AlignCenter);
    m_valueEdit->setFrame(false);
    AetherSDR::ThemeManager::instance().applyStyleSheet(m_valueEdit, "QLineEdit { color: {{color.text.primary}}; font-size: 10px; font-weight: bold;"
        " background: transparent; border: 1px solid transparent;"
        " border-radius: 2px; padding: 0;"
        " selection-background-color: {{color.background.2}}; }"
        "QLineEdit:focus { background: {{color.background.0}}; border: 1px solid {{color.accent}}; }");
    m_valueEdit->installEventFilter(this);

    rebuildLabelLayout();

    connect(m_valueEdit, &QLineEdit::returnPressed, this, [this] {
        commitValueEdit();
        m_valueEdit->clearFocus();
    });
    connect(m_valueEdit, &QLineEdit::editingFinished, this, [this] {
        commitValueEdit();
    });

    refreshValueLabel();
}

void ClientEqOutputFader::commitValueEdit()
{
    if (!m_valueEdit) return;
    static thread_local bool s_committing = false;
    if (s_committing) return;
    s_committing = true;
    const QString raw = m_valueEdit->text().trimmed();
    bool ok = false;
    double v = QLocale().toDouble(raw, &ok);
    if (!ok) {
        QString cleaned;
        cleaned.reserve(raw.size());
        for (QChar c : raw) {
            if (c.isDigit() || c == QChar('.') || c == QChar('-')
                || c == QChar('+') || c == QChar('e') || c == QChar('E'))
                cleaned.append(c);
        }
        v = cleaned.toDouble(&ok);
    }
    if (ok) {
        const float db = std::clamp(static_cast<float>(v),
                                    kGainMinDb, kGainMaxDb);
        m_gain = dbToLinear(db);
        refreshValueLabel();
        emit gainChanged(m_gain);
        update();
    } else {
        refreshValueLabel();
    }
    s_committing = false;
}

bool ClientEqOutputFader::eventFilter(QObject* obj, QEvent* ev)
{
    if (obj == m_valueEdit) {
        if (ev->type() == QEvent::Wheel) {
            wheelEvent(static_cast<QWheelEvent*>(ev));
            return true;
        }
        if (ev->type() == QEvent::KeyPress) {
            auto* ke = static_cast<QKeyEvent*>(ev);
            if (ke->key() == Qt::Key_Escape) {
                QSignalBlocker b(m_valueEdit);
                refreshValueLabel();
                m_valueEdit->clearFocus();
                return true;
            }
        }
        if (ev->type() == QEvent::FocusIn) {
            // Show bare number on focus so the user types just digits.
            QSignalBlocker b(m_valueEdit);
            const float db = linearToDb(m_gain);
            m_valueEdit->setText(QString::number(db, 'f', 1));
            m_valueEdit->selectAll();
        } else if (ev->type() == QEvent::FocusOut) {
            refreshValueLabel();
        }
    }
    return QWidget::eventFilter(obj, ev);
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
    if (!m_valueEdit || m_valueEdit->hasFocus()) return;
    const float db = linearToDb(m_gain);
    QSignalBlocker b(m_valueEdit);
    if (db <= kGainMinDb + 0.05f) {
        m_valueEdit->setText("-inf");
    } else {
        m_valueEdit->setText(QString::asprintf("%+.1f dB", db));
    }
}

void ClientEqOutputFader::setGainFromPos(QPoint pos)
{
    // Vertical counts up from the bottom, horizontal along from the left.
    const int along = (m_orientation == Qt::Horizontal) ? pos.x() : pos.y();
    float norm = std::clamp(
        static_cast<float>(along - m_stripOrigin) / std::max(1, m_stripLength),
        0.0f, 1.0f);
    if (m_orientation == Qt::Vertical) {
        norm = 1.0f - norm;
    }
    const float db = kGainMinDb + norm * (kGainMaxDb - kGainMinDb);
    m_gain = dbToLinear(db);
    refreshValueLabel();
    emit gainChanged(m_gain);
    update();
}

void ClientEqOutputFader::setOrientation(Qt::Orientation orientation)
{
    if (m_orientation == orientation) return;
    m_orientation = orientation;

    if (m_orientation == Qt::Horizontal) {
        // Bar, plus the scale figures printed beneath it, plus a little air.
        setMinimumWidth(240);
        setMaximumWidth(QWIDGETSIZE_MAX);
        setFixedHeight(kBarW + 18);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    } else {
        setMinimumHeight(160);
        setMaximumHeight(QWIDGETSIZE_MAX);
        setFixedWidth(kLabelColW + kGap + kBarW + kHandleOverhang * 2 + 2);
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    }
    rebuildLabelLayout();
    update();
}

// The two caps -- "OUT" and the editable dB value -- sit at the ends of the
// strip, so which end depends on the orientation. Rebuilt rather than
// re-parented: a QLayout cannot change direction after the fact.
void ClientEqOutputFader::rebuildLabelLayout()
{
    delete layout();

    if (m_orientation == Qt::Horizontal) {
        auto* row = new QHBoxLayout(this);
        row->setContentsMargins(4, 0, 4, 0);
        row->setSpacing(6);
        row->addWidget(m_endLabel, 0, Qt::AlignVCenter);
        row->addStretch(1);
        m_valueEdit->setFixedWidth(46);
        row->addWidget(m_valueEdit, 0, Qt::AlignVCenter);
    } else {
        auto* col = new QVBoxLayout(this);
        col->setContentsMargins(0, 2, 0, 2);
        col->setSpacing(0);
        col->addWidget(m_endLabel);
        col->addStretch(1);
        m_valueEdit->setMinimumWidth(0);
        m_valueEdit->setMaximumWidth(QWIDGETSIZE_MAX);
        col->addWidget(m_valueEdit);
    }
}

void ClientEqOutputFader::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);
    p.setRenderHint(QPainter::TextAntialiasing, true);
    if (m_orientation == Qt::Horizontal) {
        paintHorizontal(p);
    } else {
        paintVertical(p);
    }
}

// Left to right under the graph: the strip runs between the two caps the
// layout has already placed, the level fills from the left, the scale prints
// beneath it and the handle is a vertical bar standing across the meter.
void ClientEqOutputFader::paintHorizontal(QPainter& p)
{
    const int gap = 8;
    const int left = (m_endLabel ? m_endLabel->geometry().right() : 0) + gap;
    const int right = (m_valueEdit ? m_valueEdit->geometry().left() : width())
                    - gap;
    m_stripOrigin = left;
    m_stripLength = std::max(1, right - left);

    const int scaleH = 10;
    const int barTop = (height() - scaleH - kBarW) / 2;
    const QRect barR(left, barTop, m_stripLength, kBarW);

    p.fillRect(barR, QColor("#06111c"));

    const float peakNorm = std::clamp(
        (m_smoothedPeak - kMeterMinDb) / (kMeterMaxDb - kMeterMinDb),
        0.0f, 1.0f);
    const int fillW = static_cast<int>(peakNorm * m_stripLength);
    if (fillW > 0) {
        QLinearGradient grad(barR.left(), 0, barR.right(), 0);
        grad.setColorAt(0.0, QColor("#2f9e6a"));   // green at the quiet end
        grad.setColorAt(0.55, QColor("#6cc56a"));
        grad.setColorAt(0.80, QColor("#e8b94c"));
        grad.setColorAt(0.95, QColor("#e8553c"));
        grad.setColorAt(1.0, QColor("#f2362a"));
        p.fillRect(QRect(barR.x(), barR.y(), fillW, kBarW), grad);
    }

    p.setPen(QPen(QColor("#243a4e"), 1));
    p.setBrush(Qt::NoBrush);
    p.drawRect(barR.adjusted(0, 0, -1, -1));

    QFont f = p.font();
    f.setPixelSize(8);
    p.setFont(f);
    const QFontMetrics fm(f);

    struct Tick { float db; const char* label; };
    static constexpr Tick kTicks[] = {
        { -40.0f, "-40" }, { -20.0f, "-20" }, { -12.0f, "-12" },
        {  -6.0f,  "-6" }, {   0.0f,   "0" },
    };
    for (const auto& t : kTicks) {
        const float norm = (t.db - kMeterMinDb) / (kMeterMaxDb - kMeterMinDb);
        const int x = left + static_cast<int>(norm * m_stripLength);
        p.setPen(QColor("#405060"));
        p.drawLine(x, barR.bottom() + 1, x, barR.bottom() + 3);

        p.setPen(QColor("#7f93a5"));
        const QString label = QString::fromLatin1(t.label);
        const int tw = fm.horizontalAdvance(label);
        // Clamped so the end figures stay inside the strip rather than
        // hanging over the caps on either side.
        const int tx = std::clamp(x - tw / 2, left, right - tw);
        p.drawText(tx, barR.bottom() + 3 + fm.ascent(), label);
    }

    const float gainDb = std::clamp(linearToDb(m_gain),
                                    kGainMinDb, kGainMaxDb);
    const float gainNorm = (gainDb - kGainMinDb) / (kGainMaxDb - kGainMinDb);
    const int handleX = left + static_cast<int>(gainNorm * m_stripLength);
    const QRect handleR(handleX - kHandleH / 2,
                        barR.top() - kHandleOverhang,
                        kHandleH,
                        kBarW + kHandleOverhang * 2);
    p.setPen(QPen(QColor("#0a1a28"), 1));
    p.setBrush(QColor("#d7e7f2"));
    p.drawRect(handleR);
    p.setPen(QColor("#1a2a3a"));
    p.drawLine(handleX, handleR.top() + 1, handleX, handleR.bottom() - 1);
}

void ClientEqOutputFader::paintVertical(QPainter& p)
{
    // The strip lives between the OUT label at the top and the value
    // label at the bottom — paintEvent receives the full widget rect and
    // we carve out a fixed vertical band in the middle.
    const int topLabelH = 16;
    const int botLabelH = 14;
    const int stripTop  = topLabelH + kStripTopPad;
    const int stripBot  = height() - botLabelH - kStripBottomPad;
    m_stripOrigin = stripTop;
    m_stripLength = std::max(1, stripBot - stripTop);

    const int barLeft = kLabelColW + kGap + kHandleOverhang;
    const QRect barR(barLeft, stripTop, kBarW, m_stripLength);

    // Background for the bar — dark inset.
    p.fillRect(barR, QColor("#06111c"));

    // Level fill — gradient bottom green → top red, clipped to the level
    // height derived from the smoothed peak.
    const float peakNorm = std::clamp(
        (m_smoothedPeak - kMeterMinDb) / (kMeterMaxDb - kMeterMinDb),
        0.0f, 1.0f);
    const int fillH = static_cast<int>(peakNorm * m_stripLength);
    if (fillH > 0) {
        const QRect fill(barR.x(), barR.y() + m_stripLength - fillH,
                         kBarW, fillH);
        QLinearGradient grad(0, barR.y() + m_stripLength, 0, barR.y());
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
        const int y = stripTop + static_cast<int>((1.0f - norm) * m_stripLength);

        p.setPen(QColor("#7f93a5"));
        const QString s = QString::fromLatin1(t.label);
        const int tw = fm.horizontalAdvance(s);
        const int ty = std::clamp(y + fm.ascent() / 2 - 1,
                                  stripTop + fm.ascent() - 1,
                                  stripTop + m_stripLength - 1);
        p.drawText(textRight - tw, ty, s);

        p.setPen(QColor("#405060"));
        p.drawLine(textRight, y, barLeft - 1, y);
    }

    // Fader handle — horizontal bar that overhangs the meter on both
    // sides so it's easy to grab without covering the level colour.
    const float gainDb = std::clamp(linearToDb(m_gain),
                                    kGainMinDb, kGainMaxDb);
    const float gainNorm = (kGainMaxDb - gainDb) / (kGainMaxDb - kGainMinDb);
    const int handleY = stripTop + static_cast<int>(gainNorm * m_stripLength);
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
        setGainFromPos(ev->pos());
        ev->accept();
        return;
    }
    QWidget::mousePressEvent(ev);
}

void ClientEqOutputFader::mouseMoveEvent(QMouseEvent* ev)
{
    if (m_dragging) {
        setGainFromPos(ev->pos());
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

} // namespace AetherSDR
