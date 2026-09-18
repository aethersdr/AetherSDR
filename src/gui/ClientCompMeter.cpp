#include "ClientCompMeter.h"
#include "PanelTick.h"

#include <QAccessible>
#include <QAccessibleValueChangeEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPainterPath>
#include <QWheelEvent>
#include <QLinearGradient>
#include <algorithm>
#include <cmath>
#include "core/ThemeManager.h"

namespace AetherSDR {

namespace {

constexpr float kLevelMinDb = -60.0f;
constexpr float kLevelMaxDb =   0.0f;
constexpr float kGrMaxMag   =  40.0f;    // GR range 0..-40 dB
constexpr int   kPeakHoldMs =  700;
constexpr float kPeakDecayDbPer100Ms = 1.0f;

inline QColor kBarBg() { return AetherSDR::ThemeManager::instance().color("color.background.0"); }
inline QColor kGrColor() { return AetherSDR::ThemeManager::instance().color("color.accent.warning"); }
inline QColor kPeakLine() { return AetherSDR::ThemeManager::instance().color("color.text.primary"); }
inline QColor kCeilingLine() { return AetherSDR::ThemeManager::instance().color("color.accent.warning"); }  // bright amber — matches LIMIT button
const QColor kCeilingZone("#3a1810");     // dim red tint for the "no-go" zone
// Makeup fader furniture.  Amber, like the THRESH fader's handle, because
// it is the same kind of thing: a value the operator sets, riding a bar
// that reports a measurement.
inline QColor kMakeupHandle() { return AetherSDR::ThemeManager::instance().color("color.accent.warning"); }
inline QColor kMakeupStroke() { return AetherSDR::ThemeManager::instance().color("color.background.0"); }
constexpr int kMakeupTickColW   = 22;
constexpr int kMakeupHandleOver = 4;
constexpr int kMakeupHandleH    = 3;
constexpr int kMakeupCaretW     = 4;

inline QColor kLimGrTick() { return AetherSDR::ThemeManager::instance().color("color.accent.dim"); }  // cyan, distinct from the white peak line

} // namespace

ClientCompMeter::ClientCompMeter(QWidget* parent) : QWidget(parent)
{
    setMinimumWidth(18);
    setMinimumHeight(80);
    setAttribute(Qt::WA_OpaquePaintEvent, false);
    m_peakHoldTimer.start();

    m_animTimer.setTimerType(Qt::PreciseTimer);
    m_animTimer.setInterval(kPanelTickMs);
    connect(&m_animTimer, &QTimer::timeout, this, [this]() {
        const bool settled = !m_smooth.tick(m_animElapsed.restart());
        if (settled)
            m_animTimer.stop();
        update();
    });

    // Phase 5 PR 3b — repaint when the theme changes so live edits to
    // color.meter.bar.fillGradient (shared with ClientLevelMeter) show
    // up immediately.
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
            this, qOverload<>(&QWidget::update));
}

void ClientCompMeter::setMode(Mode m)
{
    if (m == m_mode) return;
    m_mode = m;
    m_currentDb = -120.0f;
    m_peakDb    = -120.0f;
    m_smooth.setTarget(0.0f);
    m_smooth.snapToTarget();
    update();
}

void ClientCompMeter::setLabel(const QString& label)
{
    m_label = label;
    update();
}

void ClientCompMeter::setTickSide(TickSide s)
{
    if (s == m_tickSide) return;
    m_tickSide = s;
    updateGeometry();
    update();
}

void ClientCompMeter::setShowValueLabel(bool on)
{
    if (on == m_showValueLabel) return;
    m_showValueLabel = on;
    updateGeometry();
    update();
}

void ClientCompMeter::setLimiterCeilingDb(float db)
{
    if (std::fabs(db - m_ceilingDb) < 0.01f) return;
    m_ceilingDb = db;
    update();
}

void ClientCompMeter::setLimiterGrDb(float db)
{
    if (std::fabs(db - m_limGrDb) < 0.01f) return;
    m_limGrDb = db;
    update();
}

void ClientCompMeter::recomputeTarget()
{
    float target = 0.0f;
    if (m_mode == Mode::Level) {
        target = std::clamp(
            (m_currentDb - kLevelMinDb) / (kLevelMaxDb - kLevelMinDb),
            0.0f, 1.0f);
    } else {
        const float mag = std::clamp(-m_currentDb, 0.0f, kGrMaxMag);
        target = mag / kGrMaxMag;
    }
    m_smooth.setTarget(target);
    if (!m_smooth.needsAnimation()) {
        if (m_animTimer.isActive()) m_animTimer.stop();
    } else if (!m_animTimer.isActive()) {
        m_animElapsed.restart();
        m_animTimer.start();
    }
}

void ClientCompMeter::setValueDb(float db)
{
    // Peak-hold tracks the loudest recent reading (or largest GR) and
    // decays after a 700 ms hold.  Jumps instantly to any new extreme
    // so transients don't get hidden.  Bar fill is smoothed separately
    // via MeterSmoother.
    if (m_mode == Mode::Level) {
        m_currentDb = db;
        if (db > m_peakDb) {
            m_peakDb = db;
            m_peakHoldTimer.restart();
        } else if (m_peakHoldTimer.elapsed() > kPeakHoldMs) {
            m_peakDb -= kPeakDecayDbPer100Ms / 10.0f;
            m_peakDb = std::max(m_peakDb, m_currentDb);
        }
    } else {
        m_currentDb = db;
        if (db < m_peakDb) {
            m_peakDb = db;
            m_peakHoldTimer.restart();
        } else if (m_peakHoldTimer.elapsed() > kPeakHoldMs) {
            m_peakDb += kPeakDecayDbPer100Ms / 10.0f;
            m_peakDb = std::min(m_peakDb, m_currentDb);
        }
    }
    recomputeTarget();
}

void ClientCompMeter::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);
    p.setRenderHint(QPainter::TextAntialiasing, true);

    const int w = width();
    const int h = height();
    const int labelH = m_label.isEmpty() ? 0 : 12;
    // Carve out room for an optional bottom value readout (mirrors the
    // THRESH fader's "-16.3 dB" footer).
    const int valueH = m_showValueLabel ? 14 : 0;
    // The makeup fader gets its own readout line under the level one. The
    // knob it replaced showed its value in words; a handle position alone
    // does not, and "how much makeup am I running" is a number operators
    // quote to each other.
    const int makeupH = (m_makeupControl && m_mode == Mode::Level) ? 13 : 0;

    // Tick column reservation (mirrors the THRESH fader). The bar
    // shifts toward the opposite side to leave room for tick labels
    // + the short tick-mark lines.
    constexpr int kTickColW = 22;
    constexpr int kTickGap  = 2;
    int leftPad  = (m_tickSide == TickSide::Left)  ? kTickColW + kTickGap : 2;
    const int rightPad = (m_tickSide == TickSide::Right) ? kTickColW + kTickGap : 2;
    // The makeup scale claims the left gutter: its own ticks plus room for
    // the handle to overhang the bar without colliding with them.
    // Tick labels, then a gap, then the room the handle's caret and
    // overhang need. Without the caret's width in this sum the "0" label
    // and the caret land on the same pixels at the detent.
    if (m_makeupControl)
        leftPad = std::max(leftPad, kMakeupTickColW + kTickGap
                                        + kMakeupHandleOver + kMakeupCaretW);
    const QRectF bar(leftPad,
                     labelH + 2.0,
                     std::max(1, w - leftPad - rightPad),
                     std::max(1, h - labelH - 4 - valueH - makeupH));

    // The hit-test maps clicks against the strip the operator sees, so the
    // makeup handle cannot land somewhere the bar is not.
    m_barTop = static_cast<int>(bar.top());
    m_barH   = std::max(1, static_cast<int>(bar.height()));

    if (!m_label.isEmpty()) {
        QFont f = p.font();
        f.setPixelSize(9);
        f.setBold(true);
        p.setFont(f);
        // Amber matches the THRESH fader's label colour so the paired
        // meters read as one consistent strip header.
        p.setPen(AetherSDR::ThemeManager::instance().color("color.accent.warning"));
        p.drawText(QRectF(0, 0, w, labelH), Qt::AlignCenter, m_label);
    }

    p.fillRect(bar, kBarBg());
    // Border matches the THRESH fader so the paired meters read as
    // one visual idiom across the comp editor.
    p.setPen(QPen(AetherSDR::ThemeManager::instance().color("color.background.1"), 1));
    p.setBrush(Qt::NoBrush);
    p.drawRect(QRectF(bar.left(), bar.top(),
                      bar.width() - 1, bar.height() - 1));

    if (m_mode == Mode::Level) {
        const float fillH = m_smooth.value() * bar.height();
        QRectF fill(bar.left(), bar.bottom() - fillH, bar.width(), fillH);

        // Shared meter-bar gradient — same token ClientLevelMeter uses,
        // so themes can recolour every level meter from one place via the
        // gradient editor.  Map against the FULL bar rect so the visible
        // portion always shows the stop colour the bar's bottom would
        // have; the bar grows upward through the gradient as compression
        // increases.
        const QBrush brush =
            AetherSDR::ThemeManager::instance()
                .brush("color.meter.bar.fillGradient", bar.toRect());
        p.fillRect(fill, brush);

        // Limiter overlay — draw ceiling zone + line if a ceiling has
        // been set.  m_ceilingDb > 0 is our sentinel for "no overlay"
        // (ceilings are always ≤ 0 dBFS in practice).
        if (m_ceilingDb <= kLevelMaxDb + 0.0001f) {
            const float tc = std::clamp(
                (m_ceilingDb - kLevelMinDb) / (kLevelMaxDb - kLevelMinDb),
                0.0f, 1.0f);
            const float cy = bar.bottom() - tc * bar.height();

            // Red-zone shading above the ceiling — "do not enter."
            const QRectF zone(bar.left(), bar.top(),
                              bar.width(), cy - bar.top());
            if (zone.height() > 0) {
                QColor zoneColor = kCeilingZone;
                zoneColor.setAlpha(140);
                p.fillRect(zone, zoneColor);
            }

            // Ceiling line — bright amber, slightly thicker than the
            // peak line so it reads as a structural limit, not a meter
            // value.
            p.setPen(QPen(kCeilingLine(), 1.5));
            p.drawLine(QPointF(bar.left() - 2.0, cy),
                       QPointF(bar.right() + 2.0, cy));

            // Limiter GR tick — cyan stub hanging from the ceiling
            // line into the bar whenever the limiter is clamping.
            // Length ∝ |m_limGrDb|, capped at 12 dB so big spikes
            // don't blow past the bar.
            if (m_limGrDb < -0.05f) {
                const float grSpanDb = std::min(-m_limGrDb, 12.0f);
                const float tickH =
                    (grSpanDb / (kLevelMaxDb - kLevelMinDb)) * bar.height();
                p.setPen(QPen(kLimGrTick(), 2.0));
                p.drawLine(QPointF(bar.center().x(), cy),
                           QPointF(bar.center().x(), cy + tickH));
            }
        }

        if (m_peakDb > kLevelMinDb) {
            const float tp = std::clamp(
                (m_peakDb - kLevelMinDb) / (kLevelMaxDb - kLevelMinDb),
                0.0f, 1.0f);
            const float y = bar.bottom() - tp * bar.height();
            p.setPen(QPen(kPeakLine(), 1.0));
            p.drawLine(QPointF(bar.left(), y), QPointF(bar.right(), y));
        }
    } else {
        const float fillH = m_smooth.value() * bar.height();
        QRectF fill(bar.left(), bar.top(), bar.width(), fillH);
        p.fillRect(fill, kGrColor());

        const float peakMag = std::clamp(-m_peakDb, 0.0f, kGrMaxMag);
        if (peakMag > 0.0f) {
            const float y = bar.top() + (peakMag / kGrMaxMag) * bar.height();
            p.setPen(QPen(kPeakLine(), 1.0));
            p.drawLine(QPointF(bar.left(), y), QPointF(bar.right(), y));
        }
    }

    // Tick column (#2887, matching THRESH fader). Labels + short
    // horizontal stub lines into the bar so the eye links the number
    // to the position on the scale.
    if (m_tickSide != TickSide::None) {
        QFont f = p.font();
        f.setPixelSize(9);
        // The header label above set bold=true on the painter font;
        // explicitly clear it here so the tick labels render in the
        // same regular weight as the THRESH fader's tick column.
        f.setBold(false);
        p.setFont(f);
        const QFontMetrics fm(f);

        struct Tick { float db; const char* label; };
        // Level: 0 / -12 / -24 / -36 / -48 (matches THRESH fader).
        // GR: 0 / -10 / -20 / -30 / -40 (matches GR display range).
        static constexpr Tick kLevelTicks[] = {
            {   0.0f,  "0" }, { -12.0f, "-12" }, { -24.0f, "-24" },
            { -36.0f, "-36" }, { -48.0f, "-48" }
        };
        static constexpr Tick kGrTicks[] = {
            {   0.0f,  "0"  }, { -10.0f, "-10" }, { -20.0f, "-20" },
            { -30.0f, "-30" }, { -40.0f, "-40" }
        };
        const Tick* ticks = (m_mode == Mode::Level) ? kLevelTicks : kGrTicks;
        const int   nTicks = 5;
        const float minDb = (m_mode == Mode::Level) ? kLevelMinDb : -kGrMaxMag;
        const float maxDb = (m_mode == Mode::Level) ? kLevelMaxDb :        0.0f;

        const int textRight = (m_tickSide == TickSide::Left)
            ? leftPad - kTickGap - 1
            : w - 2;
        const bool tickLeft = (m_tickSide == TickSide::Left);

        for (int i = 0; i < nTicks; ++i) {
            const float norm = (ticks[i].db - minDb) / (maxDb - minDb);
            const int y = static_cast<int>(bar.bottom() - norm * bar.height());

            p.setPen(AetherSDR::ThemeManager::instance().color("color.text.secondary"));
            const QString s = QString::fromLatin1(ticks[i].label);
            const int tw = fm.horizontalAdvance(s);
            const int ty = std::clamp(y + fm.ascent() / 2 - 1,
                                      static_cast<int>(bar.top()) + fm.ascent() - 1,
                                      static_cast<int>(bar.bottom()) - 1);
            if (tickLeft) {
                p.drawText(textRight - tw, ty, s);
                p.setPen(AetherSDR::ThemeManager::instance().color("color.meter.bar.fill"));
                p.drawLine(textRight, y,
                           static_cast<int>(bar.left()) - 1, y);
            } else {
                // Right side — labels grow from the bar outward.
                const int textLeft = static_cast<int>(bar.right()) + kTickGap + 1;
                p.drawText(textLeft, ty, s);
                p.setPen(AetherSDR::ThemeManager::instance().color("color.meter.bar.fill"));
                p.drawLine(static_cast<int>(bar.right()), y,
                           textLeft - 1, y);
            }
        }
    }

    // Numeric value footer (#2887, matching THRESH fader's "-16.3 dB"
    // styling: 10 px bold, light text). Right-anchored with a fixed
    // pad so the digits don't wander left/right as the value's digit
    // count changes — the " dB" suffix stays pinned at the right edge.
    // The text itself is re-formatted only every ~200 ms so the
    // numbers are readable while the bar above continues to animate
    // at the full 125 Hz smoother rate.
    if (m_showValueLabel) {
        if (!m_valueLabelClock.isValid()
            || m_valueLabelClock.elapsed() >= kMeterReadoutUpdateMs
            || m_cachedValueText.isEmpty()) {
            m_cachedValueText = (m_currentDb <= -100.0f)
                ? QStringLiteral("-∞ dB")
                : QString::asprintf("%+.1f dB", m_currentDb);
            m_valueLabelClock.restart();
        }
        QFont f = p.font();
        f.setPixelSize(10);
        f.setBold(true);
        p.setFont(f);
        p.setPen(AetherSDR::ThemeManager::instance().color("color.text.primary"));
        constexpr int kFooterRightPad = 3;
        const QRectF footer(0, h - valueH - makeupH,
                            w - kFooterRightPad, valueH);
        p.drawText(footer, Qt::AlignRight | Qt::AlignVCenter,
                   m_cachedValueText);
    }

    // ── Makeup fader ────────────────────────────────────────────────
    // Drawn last so the handle sits over the fill, the limiter overlay and
    // the peak line rather than under them.
    if (m_makeupControl && m_mode == Mode::Level) {
        QFont f = p.font();
        f.setPixelSize(8);
        f.setBold(false);
        p.setFont(f);
        const QFontMetrics fm(f);

        // Makeup ticks, left gutter.  Deliberately a different colour and
        // side from the level ticks on the right: one strip now carries two
        // scales, and nothing but placement tells them apart.
        struct MakeupTick { float db; const char* label; };
        static constexpr MakeupTick kTicks[] = {
            { 24.0f, "+24" }, { 12.0f, "+12" }, { 0.0f, "0" }, { -12.0f, "-12" }
        };
        const int textRight = leftPad - kTickGap - kMakeupHandleOver - kMakeupCaretW;
        for (const auto& t : kTicks) {
            const float norm = (t.db - kMakeupMinDb) / (kMakeupMaxDb - kMakeupMinDb);
            const int   y    = static_cast<int>(bar.bottom() - norm * bar.height());
            const QString label = QString::fromLatin1(t.label);
            const int tw = fm.horizontalAdvance(label);
            const int ty = std::clamp(y + fm.ascent() / 2 - 1,
                                      static_cast<int>(bar.top()) + fm.ascent() - 1,
                                      static_cast<int>(bar.bottom()) - 1);
            QColor tick = kMakeupHandle();
            // 0 dB is the detent — unity makeup, the value the operator
            // returns to — so it is the one tick drawn at full strength,
            // and the only one carried across the bar.
            const bool unity = (t.db == 0.0f);
            tick.setAlpha(unity ? 220 : 130);
            p.setPen(tick);
            p.drawText(textRight - tw, ty, label);
            p.drawLine(textRight + 1, y, static_cast<int>(bar.left()) - 1, y);
            if (unity) {
                QColor across = kMakeupHandle();
                across.setAlpha(70);
                p.setPen(QPen(across, 1, Qt::DashLine));
                p.drawLine(static_cast<int>(bar.left()) + 1, y,
                           static_cast<int>(bar.right()) - 1, y);
            }
        }

        // Makeup readout, amber to tie it to the handle and to separate it
        // from the white level figure directly above.
        {
            QFont vf = p.font();
            vf.setPixelSize(10);
            vf.setBold(true);
            p.setFont(vf);
            p.setPen(kMakeupHandle());
            p.drawText(QRectF(0, h - makeupH, w - 3, makeupH),
                       Qt::AlignRight | Qt::AlignVCenter,
                       QString::asprintf("%+.1f dB", m_makeupDb));
            p.setFont(f);
        }

        // Handle — overhangs the bar on the left only. The right-hand side
        // is the level tick gutter, and a handle reaching into it would read
        // as a marker on that scale.
        const int hy = static_cast<int>(bar.bottom() - makeupNorm() * bar.height());
        const QRect handleR(static_cast<int>(bar.left()) - kMakeupHandleOver,
                            hy - kMakeupHandleH / 2,
                            static_cast<int>(bar.width()) + kMakeupHandleOver,
                            kMakeupHandleH);
        p.setPen(QPen(kMakeupStroke(), 1));
        p.setBrush(kMakeupHandle());
        p.drawRect(handleR);

        QPainterPath caret;
        const int cx = handleR.left();
        caret.moveTo(cx - 4, hy);
        caret.lineTo(cx,     hy - 3);
        caret.lineTo(cx,     hy + 3);
        caret.closeSubpath();
        p.setPen(Qt::NoPen);
        p.setBrush(kMakeupHandle());
        p.drawPath(caret);

        // Focus ring, so keyboard operators can see where they are.
        if (hasFocus()) {
            QColor ring = kMakeupHandle();
            ring.setAlpha(160);
            p.setPen(QPen(ring, 1, Qt::DotLine));
            p.setBrush(Qt::NoBrush);
            p.drawRect(QRectF(bar.left() - kMakeupHandleOver - 1, bar.top() - 1,
                              bar.width() + kMakeupHandleOver + 1, bar.height() + 1));
        }
    }
}

void ClientCompMeter::setMakeupControlEnabled(bool on)
{
    if (m_makeupControl == on) return;
    m_makeupControl = on;
    if (on) {
        // Focusable and operable from the keyboard: this is a control now,
        // not a readout, and it is the only way to reach makeup gain from
        // this panel.
        setFocusPolicy(Qt::StrongFocus);
        setCursor(Qt::ArrowCursor);
        setAccessibleName(tr("Makeup gain"));
        setAccessibleDescription(
            tr("Compressor makeup gain, -12 to +24 dB, on the output meter. "
               "Up and down arrows adjust by 0.5 dB, Page Up and Page Down "
               "by 3 dB, Home returns to 0 dB."));
        setToolTip(tr(
            "Out level, and makeup gain (amber handle).\n"
            "Drag or wheel to set makeup, double-click for 0 dB.\n"
            "Left ticks are makeup; right ticks are output level."));
    } else {
        // Put back everything the enable path set, not just the focus policy:
        // a meter left advertising itself as "Makeup gain" to a screen reader
        // would be worse than one that says nothing.
        setFocusPolicy(Qt::NoFocus);
        setAccessibleName(QString());
        setAccessibleDescription(QString());
        setToolTip(QString());
        unsetCursor();
        m_dragging = false;
    }
    update();
}

void ClientCompMeter::setMakeupDb(float db)
{
    const float clamped = std::clamp(db, kMakeupMinDb, kMakeupMaxDb);
    if (std::fabs(clamped - m_makeupDb) < 0.01f) return;
    m_makeupDb = clamped;
    update();
}

float ClientCompMeter::makeupNorm() const
{
    return (m_makeupDb - kMakeupMinDb) / (kMakeupMaxDb - kMakeupMinDb);
}

void ClientCompMeter::commitMakeup(float db)
{
    const float clamped = std::clamp(db, kMakeupMinDb, kMakeupMaxDb);
    if (std::fabs(clamped - m_makeupDb) < 0.01f) return;
    m_makeupDb = clamped;
    update();
#ifndef QT_NO_ACCESSIBILITY
    QAccessibleValueChangeEvent ev(this, QVariant(m_makeupDb));
    QAccessible::updateAccessibility(&ev);
#endif
    emit makeupChanged(m_makeupDb);
}

void ClientCompMeter::setMakeupFromY(int y)
{
    const float norm = 1.0f - std::clamp(
        static_cast<float>(y - m_barTop) / static_cast<float>(m_barH), 0.0f, 1.0f);
    commitMakeup(kMakeupMinDb + norm * (kMakeupMaxDb - kMakeupMinDb));
}

void ClientCompMeter::mousePressEvent(QMouseEvent* ev)
{
    if (m_makeupControl && m_mode == Mode::Level && ev->button() == Qt::LeftButton) {
        m_dragging = true;
        setFocus(Qt::MouseFocusReason);
        setMakeupFromY(ev->position().toPoint().y());
        ev->accept();
        return;
    }
    QWidget::mousePressEvent(ev);
}

void ClientCompMeter::mouseMoveEvent(QMouseEvent* ev)
{
    if (m_dragging) {
        setMakeupFromY(ev->position().toPoint().y());
        ev->accept();
        return;
    }
    QWidget::mouseMoveEvent(ev);
}

void ClientCompMeter::mouseReleaseEvent(QMouseEvent* ev)
{
    if (m_dragging && ev->button() == Qt::LeftButton) {
        m_dragging = false;
        ev->accept();
        return;
    }
    QWidget::mouseReleaseEvent(ev);
}

void ClientCompMeter::mouseDoubleClickEvent(QMouseEvent* ev)
{
    if (m_makeupControl && m_mode == Mode::Level && ev->button() == Qt::LeftButton) {
        commitMakeup(kMakeupDefaultDb);
        ev->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(ev);
}

void ClientCompMeter::wheelEvent(QWheelEvent* ev)
{
    if (!m_makeupControl || m_mode != Mode::Level) { QWidget::wheelEvent(ev); return; }
    const int notches = ev->angleDelta().y() / 120;
    if (notches == 0) { QWidget::wheelEvent(ev); return; }
    commitMakeup(m_makeupDb + 0.5f * static_cast<float>(notches));
    ev->accept();
}

void ClientCompMeter::keyPressEvent(QKeyEvent* ev)
{
    if (!m_makeupControl || m_mode != Mode::Level) { QWidget::keyPressEvent(ev); return; }
    switch (ev->key()) {
    case Qt::Key_Up:    case Qt::Key_Right: commitMakeup(m_makeupDb + 0.5f); break;
    case Qt::Key_Down:  case Qt::Key_Left:  commitMakeup(m_makeupDb - 0.5f); break;
    case Qt::Key_PageUp:                    commitMakeup(m_makeupDb + 3.0f); break;
    case Qt::Key_PageDown:                  commitMakeup(m_makeupDb - 3.0f); break;
    case Qt::Key_Home:                      commitMakeup(kMakeupDefaultDb);  break;
    default: QWidget::keyPressEvent(ev); return;
    }
    ev->accept();
}

} // namespace AetherSDR
