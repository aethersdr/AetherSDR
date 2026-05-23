#include "ClientCompThresholdFader.h"

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
#include <QPainterPath>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>

namespace MasterSDR {

namespace {

// Same ballistics as ClientEqOutputFader — fast rise, slow fall.
constexpr float kPeakAttack  = 0.6f;
constexpr float kPeakRelease = 0.08f;

const QColor kHandleFill   ("#e8a540");   // amber — same as threshold chevron
const QColor kHandleStroke ("#0a1a28");
const QColor kHandleCenter ("#3a2a10");

} // namespace

ClientCompThresholdFader::ClientCompThresholdFader(QWidget* parent)
    : QWidget(parent)
{
    setFixedWidth(kLabelColW + kGap + kBarW + kHandleOverhang * 2 + 2);
    setMinimumHeight(160);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    setFocusPolicy(Qt::ClickFocus);
    setCursor(Qt::ArrowCursor);
    setToolTip(
        "Threshold (dBFS). Drag to set, wheel for 0.5 dB step,\n"
        "double-click to reset to -18 dB.\n"
        "Linked to the threshold chevron on the curve.");

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 2, 0, 2);
    root->setSpacing(0);

    auto* top = new QLabel("THRESH");
    top->setAlignment(Qt::AlignCenter);
    top->setStyleSheet(
        "QLabel { color: #e8a540; font-size: 9px; font-weight: bold;"
        " background: transparent; border: none; }");
    root->addWidget(top);

    root->addStretch(1);

    // Inline-editable threshold value.  Click to focus, type a dB number,
    // Enter or focus-out to commit (clamped to [kThreshMinDb, kThreshMaxDb]).
    // Looks identical to a label until focused; matches ClientCompKnob /
    // ClientEqOutputFader's edit affordance.
    m_valueEdit = new QLineEdit;
    m_valueEdit->setAlignment(Qt::AlignCenter);
    m_valueEdit->setFrame(false);
    m_valueEdit->setStyleSheet(
        "QLineEdit { color: #e8e8e8; font-size: 10px; font-weight: bold;"
        " background: transparent; border: 1px solid transparent;"
        " border-radius: 2px; padding: 0;"
        " selection-background-color: #0070c0; }"
        "QLineEdit:focus { background: #0a0a18; border: 1px solid #00b4d8; }");
    m_valueEdit->installEventFilter(this);
    root->addWidget(m_valueEdit);

    connect(m_valueEdit, &QLineEdit::returnPressed, this, [this] {
        commitValueEdit();
        m_valueEdit->clearFocus();
    });
    connect(m_valueEdit, &QLineEdit::editingFinished, this, [this] {
        commitValueEdit();
    });

    refreshValueLabel();
}

void ClientCompThresholdFader::commitValueEdit()
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
        m_thresholdDb = std::clamp(static_cast<float>(v),
                                   kThreshMinDb, kThreshMaxDb);
        refreshValueLabel();
        emit thresholdChanged(m_thresholdDb);
        update();
    } else {
        refreshValueLabel();
    }
    s_committing = false;
}

bool ClientCompThresholdFader::eventFilter(QObject* obj, QEvent* ev)
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
            QSignalBlocker b(m_valueEdit);
            m_valueEdit->setText(QString::number(m_thresholdDb, 'f', 1));
            m_valueEdit->selectAll();
        } else if (ev->type() == QEvent::FocusOut) {
            refreshValueLabel();
        }
    }
    return QWidget::eventFilter(obj, ev);
}

void ClientCompThresholdFader::setThresholdDb(float db)
{
    const float clamped = std::clamp(db, kThreshMinDb, kThreshMaxDb);
    if (std::fabs(clamped - m_thresholdDb) < 0.01f) return;
    m_thresholdDb = clamped;
    refreshValueLabel();
    update();
}

void ClientCompThresholdFader::setInputPeakDb(float db)
{
    const float alpha = (db > m_smoothedPeakDb) ? kPeakAttack : kPeakRelease;
    m_smoothedPeakDb += alpha * (db - m_smoothedPeakDb);
    update();
}

void ClientCompThresholdFader::refreshValueLabel()
{
    if (!m_valueEdit || m_valueEdit->hasFocus()) return;
    QSignalBlocker b(m_valueEdit);
    m_valueEdit->setText(QString::asprintf("%+.1f dB", m_thresholdDb));
}

void ClientCompThresholdFader::setThresholdFromY(int y)
{
    // Map y to dBFS: top of strip = kThreshMaxDb (0), bottom = kThreshMinDb (-60).
    const float norm = 1.0f - std::clamp(
        static_cast<float>(y - m_stripTop) / std::max(1, m_stripH),
        0.0f, 1.0f);
    const float db = kThreshMinDb + norm * (kThreshMaxDb - kThreshMinDb);
    m_thresholdDb = std::clamp(db, kThreshMinDb, kThreshMaxDb);
    refreshValueLabel();
    emit thresholdChanged(m_thresholdDb);
    update();
}

void ClientCompThresholdFader::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);
    p.setRenderHint(QPainter::TextAntialiasing, true);

    // Strip vertical bounds — carve between the THRESH label and the
    // numeric value label.
    const int topLabelH = 16;
    const int botLabelH = 14;
    const int stripTop  = topLabelH + kStripTopPad;
    const int stripBot  = height() - botLabelH - kStripBottomPad;
    m_stripTop = stripTop;
    m_stripH   = std::max(1, stripBot - stripTop);

    const int barLeft = kLabelColW + kGap + kHandleOverhang;
    const QRect barR(barLeft, stripTop, kBarW, m_stripH);

    p.fillRect(barR, QColor("#06111c"));

    // Level fill — same green→amber→red gradient as the output fader so
    // the metering vocabulary is consistent across the app.
    const float peakNorm = std::clamp(
        (m_smoothedPeakDb - kMeterMinDb) / (kMeterMaxDb - kMeterMinDb),
        0.0f, 1.0f);
    const int fillH = static_cast<int>(peakNorm * m_stripH);
    if (fillH > 0) {
        const QRect fill(barR.x(), barR.y() + m_stripH - fillH,
                         kBarW, fillH);
        QLinearGradient grad(0, barR.y() + m_stripH, 0, barR.y());
        grad.setColorAt(0.0, QColor("#2f9e6a"));
        grad.setColorAt(0.55, QColor("#6cc56a"));
        grad.setColorAt(0.80, QColor("#e8b94c"));
        grad.setColorAt(0.95, QColor("#e8553c"));
        grad.setColorAt(1.0, QColor("#f2362a"));
        p.fillRect(fill, grad);
    }

    p.setPen(QPen(QColor("#243a4e"), 1));
    p.setBrush(Qt::NoBrush);
    p.drawRect(barR.adjusted(0, 0, -1, -1));

    // dB scale on the left.
    QFont f = p.font();
    f.setPixelSize(8);
    p.setFont(f);
    const QFontMetrics fm(f);
    const int textRight = kLabelColW - 2;

    struct Tick { float db; const char* label; };
    static constexpr Tick kTicks[] = {
        {   0.0f,  "0" },
        { -12.0f,  "-12" },
        { -24.0f,  "-24" },
        { -36.0f,  "-36" },
        { -48.0f,  "-48" },
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

    // Threshold handle — amber bar overhanging both sides of the meter,
    // matching the chevron's colour.  The value label already shows the
    // numeric value, so the handle just marks the position visually.
    const float threshNorm =
        (kThreshMaxDb - m_thresholdDb) / (kThreshMaxDb - kThreshMinDb);
    const int handleY = stripTop + static_cast<int>(threshNorm * m_stripH);
    const QRect handleR(barLeft - kHandleOverhang,
                        handleY - kHandleH / 2,
                        kBarW + kHandleOverhang * 2,
                        kHandleH);
    p.setPen(QPen(kHandleStroke, 1));
    p.setBrush(kHandleFill);
    p.drawRect(handleR);
    p.setPen(kHandleCenter);
    p.drawLine(handleR.left() + 1, handleY,
               handleR.right() - 1, handleY);

    // Tiny caret on the left edge of the handle so the control reads
    // as "grabbable" even when stationary.
    QPainterPath caret;
    const int cy = handleY;
    caret.moveTo(barLeft - kHandleOverhang - 4, cy);
    caret.lineTo(barLeft - kHandleOverhang,     cy - 3);
    caret.lineTo(barLeft - kHandleOverhang,     cy + 3);
    caret.closeSubpath();
    p.setBrush(kHandleFill);
    p.setPen(Qt::NoPen);
    p.drawPath(caret);
}

void ClientCompThresholdFader::mousePressEvent(QMouseEvent* ev)
{
    if (ev->button() == Qt::LeftButton) {
        m_dragging = true;
        setCursor(Qt::ClosedHandCursor);
        setThresholdFromY(ev->pos().y());
        ev->accept();
        return;
    }
    QWidget::mousePressEvent(ev);
}

void ClientCompThresholdFader::mouseMoveEvent(QMouseEvent* ev)
{
    if (m_dragging) {
        setThresholdFromY(ev->pos().y());
        ev->accept();
        return;
    }
    QWidget::mouseMoveEvent(ev);
}

void ClientCompThresholdFader::mouseReleaseEvent(QMouseEvent* ev)
{
    if (m_dragging && ev->button() == Qt::LeftButton) {
        m_dragging = false;
        setCursor(Qt::ArrowCursor);
        ev->accept();
        return;
    }
    QWidget::mouseReleaseEvent(ev);
}

void ClientCompThresholdFader::mouseDoubleClickEvent(QMouseEvent* ev)
{
    if (ev->button() == Qt::LeftButton) {
        m_thresholdDb = kThreshDefaultDb;
        refreshValueLabel();
        emit thresholdChanged(m_thresholdDb);
        update();
        ev->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(ev);
}

void ClientCompThresholdFader::wheelEvent(QWheelEvent* ev)
{
    const int notches = ev->angleDelta().y() / 120;
    if (notches == 0) { QWidget::wheelEvent(ev); return; }
    m_thresholdDb = std::clamp(m_thresholdDb + 0.5f * notches,
                               kThreshMinDb, kThreshMaxDb);
    refreshValueLabel();
    emit thresholdChanged(m_thresholdDb);
    update();
    ev->accept();
}

} // namespace MasterSDR
