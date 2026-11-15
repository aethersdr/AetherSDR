#include "ClientCompKnob.h"

#include <QDoubleValidator>
#include <QEvent>
#include <QFocusEvent>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QLineEdit>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <QPaintEvent>
#include <QPen>
#include <QPixmap>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include "core/ThemeManager.h"

namespace AetherSDR {

namespace {

constexpr float kDragPxPerFullRange = 200.0f;  // 200 px vertical = 0..1
constexpr float kWheelNormStep      = 0.01f;   // 1% per wheel notch
constexpr float kFineMultiplier     = 0.25f;   // Shift-drag scales ×0.25
constexpr float kArcStartDeg        = 225.0f;  // 7:30 clockwise to
constexpr float kArcSpanDeg         = -270.0f; // 4:30  = 270° sweep

// Knob components read from the dedicated color.knob.* namespace
// (carved out of color.background.1 / color.accent.dim /
// color.text.primary).  Each helper takes the owning widget so the
// widget-aware overload walks the container chain — that's how
// per-applet container overrides (e.g. applet/comp's amber knob
// foreground) actually reach the rendered output.  Label + value
// text below the knob stay on color.text.* — those are text, not
// knob componentry.
inline QColor kRingBg(const QWidget* w) { return AetherSDR::ThemeManager::instance().color(w, "color.knob.background"); }
inline QColor kRingArc(const QWidget* w) { return AetherSDR::ThemeManager::instance().color(w, "color.knob.foreground"); }
inline QColor kPointer(const QWidget* w) { return AetherSDR::ThemeManager::instance().color(w, "color.knob.handle"); }
inline QColor kLabelColor(const QWidget* w) { return AetherSDR::ThemeManager::instance().color(w, "color.text.secondary"); }
inline QColor kValueColor(const QWidget* w) { return AetherSDR::ThemeManager::instance().color(w, "color.text.primary"); }
} // namespace

ClientCompKnob::ClientCompKnob(QWidget* parent) : QWidget(parent)
{
    setMinimumSize(58, 64);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_OpaquePaintEvent, false);

    // Inline value editor — child QLineEdit overlay positioned over the
    // value text the knob would otherwise paint.  Click to focus, type
    // a number, Enter or focus-out to commit.  Clamped to [min, max]
    // via setValue().  Wheel events bubbling up while focused are
    // forwarded to the knob so the existing wheel-adjust UX survives.
    m_valueEdit = new QLineEdit(this);
    m_valueEdit->setAlignment(Qt::AlignHCenter);
    m_valueEdit->setFrame(false);
    m_valueEdit->setStyleSheet(
        // No frame normally — looks identical to a painted label.  On
        // focus, a subtle dark inset + cyan border indicates edit mode.
        "QLineEdit { background: transparent; color: #e8e8e8;"
        " border: 1px solid transparent; border-radius: 2px;"
        " padding: 0; selection-background-color: #0070c0; }"
        "QLineEdit:focus { background: #0a0a18;"
        " border: 1px solid #00b4d8; }");
    m_valueEdit->installEventFilter(this);  // wheel forward + Escape cancel
    refreshValueEditDisplay();

    connect(m_valueEdit, &QLineEdit::returnPressed, this, [this]() {
        commitValueEdit();
        // Drop focus so the editor reverts to its "looks like a label" look.
        clearFocus();
    });
    connect(m_valueEdit, &QLineEdit::editingFinished, this, [this]() {
        // editingFinished fires on focus-out too; commit there as well so
        // clicking elsewhere applies the value (matches the Phone applet's
        // commit-on-blur expectation from the issue body).
        commitValueEdit();
    });
}

void ClientCompKnob::setLabel(const QString& text)
{
    m_label = text;
    update();
}

void ClientCompKnob::setRange(float minPhysical, float maxPhysical)
{
    m_minPhys = minPhysical;
    m_maxPhys = maxPhysical;
    setValue(m_physical);
}

void ClientCompKnob::setDefault(float physical)
{
    m_defaultPhys = physical;
}

void ClientCompKnob::setValueFromNorm(ValueMap fromNorm)
{
    m_fromNorm = std::move(fromNorm);
    setValue(m_physical);  // re-evaluate with new mapping
}

void ClientCompKnob::setNormFromValue(ValueMap toNorm)
{
    m_toNorm = std::move(toNorm);
    setValue(m_physical);
}

void ClientCompKnob::setLabelFormat(LabelFormat fmt)
{
    m_fmt = std::move(fmt);
    refreshValueEditDisplay();
    update();
}

void ClientCompKnob::setCenterLabelMode(bool on)
{
    if (m_centerLabel == on) return;
    m_centerLabel = on;
    update();
}

void ClientCompKnob::setInlineEditEnabled(bool on)
{
    if (m_inlineEdit == on) return;
    m_inlineEdit = on;
    if (m_valueEdit) m_valueEdit->setVisible(on);
    update();
}

void ClientCompKnob::setValue(float physical)
{
    m_physical = std::clamp(physical, m_minPhys, m_maxPhys);
    if (m_toNorm) {
        m_norm = std::clamp(m_toNorm(m_physical), 0.0f, 1.0f);
    } else {
        const float span = std::max(1e-9f, m_maxPhys - m_minPhys);
        m_norm = std::clamp((m_physical - m_minPhys) / span, 0.0f, 1.0f);
    }
    refreshValueEditDisplay();
    update();
}

void ClientCompKnob::applyNorm(float norm)
{
    m_norm = std::clamp(norm, 0.0f, 1.0f);
    if (m_fromNorm) {
        m_physical = std::clamp(m_fromNorm(m_norm), m_minPhys, m_maxPhys);
    } else {
        m_physical = m_minPhys + m_norm * (m_maxPhys - m_minPhys);
    }
    emit valueChanged(m_physical);
    refreshValueEditDisplay();
    update();
}

QString ClientCompKnob::formatValue() const
{
    if (m_fmt) return m_fmt(m_physical);
    return QString::number(m_physical, 'f', 2);
}

QRect ClientCompKnob::valueRect() const
{
    const int w = width();
    const int h = height();
    const int topRow    = m_centerLabel ?  0 : 12;
    const int bottomRow = 4;
    const int diameter  = std::min(w - 4, h - topRow - bottomRow);
    const int valueY = topRow + static_cast<int>(diameter * 0.82f);
    const int valueH = std::max(13, h - valueY);
    return QRect(0, valueY, w, valueH);
}

void ClientCompKnob::layoutValueEditor()
{
    if (!m_valueEdit) return;
    const QRect r = valueRect();
    // Slight horizontal inset so the editor isn't full-width — keeps
    // the visual match with the painted text the old paint path drew.
    const int inset = std::max(2, r.width() / 8);
    m_valueEdit->setGeometry(r.adjusted(inset, 0, -inset, 0));
    QFont f = m_valueEdit->font();
    f.setPixelSize(11);
    m_valueEdit->setFont(f);
}

void ClientCompKnob::refreshValueEditDisplay()
{
    if (!m_valueEdit || m_valueEdit->hasFocus()) return;  // don't fight active edit
    QSignalBlocker b(m_valueEdit);
    m_valueEdit->setText(formatValue());
}

void ClientCompKnob::commitValueEdit()
{
    if (!m_valueEdit) return;
    // Already locked out by hasFocus guard in refreshValueEditDisplay,
    // but be doubly defensive against re-entry from editingFinished +
    // returnPressed firing back to back.
    static thread_local bool s_committing = false;
    if (s_committing) return;
    s_committing = true;

    const QString raw = m_valueEdit->text().trimmed();
    bool ok = false;
    // Locale-aware parse so "12,5" works in comma-decimal locales.
    double v = QLocale().toDouble(raw, &ok);
    if (!ok) {
        // Fallback: strip everything that isn't digit / sign / dot, then
        // try C-locale parse so "12.5 ms" or "−6 dB" still work.
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
        setValue(static_cast<float>(v));   // setValue() clamps to [min, max]
        emit valueChanged(m_physical);
    } else {
        // Bad input — silently revert.
        refreshValueEditDisplay();
    }
    s_committing = false;
}

void ClientCompKnob::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const int w = width();
    const int h = height();
    // Center-label mode reclaims the 12-px label row above the ring.
    // The ring uses almost all remaining height — the 270° arc sweep
    // (225° → -45°) leaves a 90° gap at the bottom (4:30 → 7:30),
    // which is where the value readout lives.  Reserving a separate
    // "row" below the ring creates a visible gap; overlapping the
    // empty arc sector keeps the readout snug against the knob.
    const int topRow    = m_centerLabel ?  0 : 12;
    const int bottomRow = 4;
    const int diameter  = std::min(w - 4, h - topRow - bottomRow);
    const QRectF ring((w - diameter) * 0.5f,
                      static_cast<float>(topRow),
                      diameter, diameter);

    // Label — above the ring in classic mode, centred inside the
    // ring in center-label mode.  Centred text is rendered AFTER
    // the arc + pointer so it reads on top cleanly.
    QFont labelFont = p.font();
    labelFont.setPixelSize(9);
    labelFont.setBold(true);
    if (!m_centerLabel) {
        // Shrink-to-fit: longer labels ("Threshold", "Release") on
        // narrow knobs would otherwise truncate with "..." — scale
        // the font down in 1-px steps until the rendered width fits
        // the widget, floor at 7 px.
        int pixelSize = 9;
        QFontMetrics fm(labelFont);
        const float maxWidth = std::max(8.0f, static_cast<float>(w) - 2.0f);
        while (fm.horizontalAdvance(m_label) > maxWidth && pixelSize > 7) {
            --pixelSize;
            labelFont.setPixelSize(pixelSize);
            fm = QFontMetrics(labelFont);
        }
        p.setFont(labelFont);
        p.setPen(kLabelColor(this));
        p.drawText(QRectF(0, 0, w, 12), Qt::AlignCenter, m_label);
    }

    // Background ring.
    const qreal thick = std::max(2.0, diameter * 0.10);
    QPen bgPen(kRingBg(this), thick);
    bgPen.setCapStyle(Qt::FlatCap);
    p.setPen(bgPen);
    p.drawArc(ring.adjusted(thick * 0.5, thick * 0.5,
                            -thick * 0.5, -thick * 0.5),
              static_cast<int>(kArcStartDeg * 16.0f),
              static_cast<int>(kArcSpanDeg * 16.0f));

    // Value arc.
    QPen arcPen(kRingArc(this), thick);
    arcPen.setCapStyle(Qt::FlatCap);
    p.setPen(arcPen);
    p.drawArc(ring.adjusted(thick * 0.5, thick * 0.5,
                            -thick * 0.5, -thick * 0.5),
              static_cast<int>(kArcStartDeg * 16.0f),
              static_cast<int>((kArcSpanDeg * m_norm) * 16.0f));

    // Pointer tick line from the inner edge of the arc to the inner
    // tick radius — visible indicator of exact knob position.
    const float angle = (kArcStartDeg + kArcSpanDeg * m_norm) * (M_PI / 180.0);
    const QPointF c = ring.center();
    const float rOut = diameter * 0.5f - thick * 0.5f;
    const float rIn  = diameter * 0.5f - thick * 1.6f;
    const QPointF pOut(c.x() + rOut * std::cos(angle),
                       c.y() - rOut * std::sin(angle));
    const QPointF pIn (c.x() + rIn  * std::cos(angle),
                       c.y() - rIn  * std::sin(angle));
    QPen pointerPen(kPointer(this), thick * 0.6);
    pointerPen.setCapStyle(Qt::RoundCap);
    p.setPen(pointerPen);
    p.drawLine(pIn, pOut);

    // Center-label mode: draw the parameter name centred inside the
    // ring.  The cap is sized so a 6-char label like "Thresh" /
    // "Release" fits inside the ring WITHOUT shrinking — every label
    // then renders at the same size, whether it's "Hold" or "Return".
    // Without the cap the shrink-to-fit fires only on long labels and
    // short labels bloat, visually inconsistent across the row.
    if (m_centerLabel && !m_label.isEmpty()) {
        QFont centerFont = p.font();
        centerFont.setBold(true);
        int pixelSize = std::max(8, diameter / 6);
        centerFont.setPixelSize(pixelSize);

        // Max text width is the knob interior, less a ring-thickness
        // pad on each side so text never kisses the arc.
        const float maxTextWidth = std::max(
            8.0f,
            static_cast<float>(diameter) - 4.0f * static_cast<float>(thick));
        QFontMetrics fm(centerFont);
        while (fm.horizontalAdvance(m_label) > maxTextWidth
               && pixelSize > 7) {
            --pixelSize;
            centerFont.setPixelSize(pixelSize);
            fm = QFontMetrics(centerFont);
        }

        p.setFont(centerFont);
        p.setPen(kLabelColor(this));
        p.drawText(ring, Qt::AlignCenter, m_label);
    }

    // Value text — when inline editing is on (channel strip / editor),
    // the child QLineEdit handles painting.  When off (applet tiles,
    // where the QLineEdit would clip), paint the formatted value here.
    if (!m_inlineEdit) {
        QFont valueFont = p.font();
        valueFont.setBold(true);
        valueFont.setPixelSize(11);
        p.setFont(valueFont);
        p.setPen(kValueColor(this));
        p.drawText(valueRect(), Qt::AlignCenter, formatValue());
    }
}

void ClientCompKnob::mousePressEvent(QMouseEvent* ev)
{
    if (ev->button() != Qt::LeftButton) return;
    m_dragging = true;
    m_dragStartY = ev->position().y();
    m_dragStartNorm = m_norm;
    setCursor(Qt::SizeVerCursor);
    ev->accept();
}

void ClientCompKnob::mouseMoveEvent(QMouseEvent* ev)
{
    if (!m_dragging) return;
    const float dy = static_cast<float>(m_dragStartY - ev->position().y());
    const float scale = (ev->modifiers() & Qt::ShiftModifier)
                          ? kFineMultiplier : 1.0f;
    applyNorm(m_dragStartNorm + scale * dy / kDragPxPerFullRange);
    ev->accept();
}

void ClientCompKnob::mouseReleaseEvent(QMouseEvent* ev)
{
    if (!m_dragging) return;
    m_dragging = false;
    setCursor(Qt::ArrowCursor);
    ev->accept();
}

void ClientCompKnob::mouseDoubleClickEvent(QMouseEvent* ev)
{
    if (ev->button() != Qt::LeftButton) return;
    setValue(m_defaultPhys);
    emit valueChanged(m_physical);
    ev->accept();
}

void ClientCompKnob::wheelEvent(QWheelEvent* ev)
{
    const int ticks = ev->angleDelta().y() / 120;
    const float scale = (ev->modifiers() & Qt::ShiftModifier)
                          ? kFineMultiplier : 1.0f;
    applyNorm(m_norm + ticks * kWheelNormStep * scale);
    ev->accept();
}

void ClientCompKnob::resizeEvent(QResizeEvent* ev)
{
    QWidget::resizeEvent(ev);
    layoutValueEditor();
}

bool ClientCompKnob::eventFilter(QObject* obj, QEvent* ev)
{
    if (obj == m_valueEdit) {
        // Forward wheel events on the value editor to the parent knob
        // so the existing wheel-adjust UX (#1026 fine step with Shift)
        // keeps working when the cursor happens to be over the value
        // text instead of the ring.
        if (ev->type() == QEvent::Wheel) {
            wheelEvent(static_cast<QWheelEvent*>(ev));
            return true;
        }
        // Escape cancels the edit without committing — revert text from
        // the current physical value and drop focus.
        if (ev->type() == QEvent::KeyPress) {
            auto* ke = static_cast<QKeyEvent*>(ev);
            if (ke->key() == Qt::Key_Escape) {
                QSignalBlocker b(m_valueEdit);   // suppress editingFinished
                refreshValueEditDisplay();
                m_valueEdit->clearFocus();
                return true;
            }
        }
        // Show the formatted text-with-units on blur, raw number on focus,
        // so the user types just digits (no need to type units back).
        if (ev->type() == QEvent::FocusIn) {
            QSignalBlocker b(m_valueEdit);
            m_valueEdit->setText(QString::number(m_physical, 'f', 3));
            m_valueEdit->selectAll();
        } else if (ev->type() == QEvent::FocusOut) {
            refreshValueEditDisplay();
        }
    }
    return QWidget::eventFilter(obj, ev);
}

} // namespace AetherSDR
