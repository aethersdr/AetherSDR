#pragma once

#include "DragValuePopup.h"
#include "MeterSmoother.h"

#include <QAccessible>
#include <QAccessibleWidget>
#include <QCursor>
#include <QEnterEvent>
#include <QEvent>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPoint>
#include <QStringList>
#include <QWidget>
#include <QWheelEvent>
#include <QTimer>
#include <QElapsedTimer>
#include <QVector>
#include <cmath>
#include <functional>
#include <limits>
#include <utility>

namespace AetherSDR {

// ── HGauge: reusable horizontal bar gauge ─────────────────────────────────────
//
// Draws a horizontal bar with:
//  - Dark background
//  - Filled portion (cyan below redStart, red above)
//  - Tick labels along the top
//  - Label text centred in the bar

class HGauge : public QWidget {
public:
    struct Tick { float value; QString label; };

    HGauge(float min, float max, float redStart,
           const QString& label, const QString& unit,
           const QVector<Tick>& ticks, QWidget* parent = nullptr,
           float yellowStart = std::numeric_limits<float>::quiet_NaN())
        : QWidget(parent)
        , m_min(min), m_max(max), m_redStart(redStart)
        , m_yellowStart(std::isnan(yellowStart) ? redStart : yellowStart)
        , m_label(label), m_unit(unit), m_ticks(ticks)
    {
        setFixedHeight(24);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

        m_smooth.setTarget(fractionFor(m_value));
        m_smooth.snapToTarget();
        m_animTimer.setTimerType(Qt::PreciseTimer);
        m_animTimer.setInterval(kMeterSmootherIntervalMs);
        connect(&m_animTimer, &QTimer::timeout, this, [this]() {
            if (!m_smooth.tick(m_animElapsed.restart()))
                m_animTimer.stop();
            // Republish so gaugeFraction tracks the bar through the sweep, not
            // just at the setValue/setRange call that started it — otherwise
            // the one property that reports DRAWN state would itself go stale
            // mid-animation, which is the defect class it exists to expose.
            // No-op unless AETHER_AUTOMATION is set.
            publishAutomationState();
            update();
        });

        // Recovery watchdog for a dropped leaveEvent — see syncHoverWatchdog().
        // Runs only while the pointer is physically over the bar, so it costs
        // nothing except during a real hover, and never fires for an injected
        // one.
        m_hoverWatchdog.setInterval(kHoverWatchdogMs);
        connect(&m_hoverWatchdog, &QTimer::timeout,
                this, [this]() { onHoverWatchdogTick(); });

        publishAutomationState();
    }

    ~HGauge() override {
        // Drop the app-wide "badge on screen" claim so it can't dangle at a
        // destroyed gauge (see activeHoverGauge()).
        if (activeHoverGauge() == this)
            activeHoverGauge() = nullptr;
    }

    void setLabel(const QString& label) {
        if (m_label == label) return;
        m_label = label;
        publishAutomationState();
        update();
    }

    // The normalised [0,1] fill fraction, after ballistics. Distinct from
    // value()/min()/max(): those are the INPUTS, this is the derived state that
    // gets drawn, and the two can disagree (see setRange). Exposed so that
    // divergence is assertable without pixel-reading.
    //
    // This is what paintEvent multiplies by the bar width for a normal gauge
    // and for setFillFromRight(). On a setReversed() gauge (PhoneCwApplet's
    // compression bar) the mapping is inverted at paint time — min means FULL —
    // so the painted width there is 1.0f - filledFraction(). Assert
    // accordingly; the fraction itself is always value-normalised.
    float filledFraction() const { return m_smooth.value(); }

    void setValue(float v) {
        if (qFuzzyCompare(m_value, v)) return;
        m_value = v;
        m_smooth.setTarget(fractionFor(v));
        if (!m_smooth.needsAnimation()) {
            if (m_animTimer.isActive()) m_animTimer.stop();
            update();
        } else if (!m_animTimer.isActive()) {
            m_animElapsed.restart();
            m_animTimer.start();
        }
        publishAutomationState();
        refreshHoverPopup();
    }

    void setValueImmediate(float v) {
        if (m_animTimer.isActive()) m_animTimer.stop();
        m_value = v;
        m_smooth.setTarget(fractionFor(v));
        m_smooth.snapToTarget();
        publishAutomationState();
        update();
        refreshHoverPopup();
    }

    void setPeakValue(float v) {
        if (qFuzzyCompare(m_peakValue, v)) return;
        m_peakValue = v;
        m_peakEnabled = true;
        update();
    }

    void clearPeak() {
        if (!m_peakEnabled) return;
        m_peakEnabled = false;
        update();
    }

    void setReversed(bool rev) {
        if (m_reversed == rev) return;
        m_reversed = rev;

        // In reversed gauges, visible fill increases as the normalized target
        // decreases. Swap the current time constants so custom ballistics are
        // preserved while increasing fill still uses attack and falling fill
        // still uses release.
        MeterSmoother::Ballistics ballistics = m_smooth.ballistics();
        std::swap(ballistics.attackSeconds, ballistics.releaseSeconds);
        m_smooth.setBallistics(ballistics);
        update();
    }
    // Anchor the fill bar to the right edge instead of the left.  Unlike
    // setReversed (which also inverts the value mapping for compression-
    // style gauges), this just mirrors the fill direction — min still
    // means empty, max still means full, but the bar grows leftward from
    // the right edge.  Used for the ALC gauge so the bar tracks the scale
    // in the natural direction.
    void setFillFromRight(bool on) { m_fillFromRight = on; update(); }

    void setBallistics(const MeterSmoother::Ballistics& b) {
        m_smooth.setBallistics(b);
    }

    void setRange(float min, float max, float redStart,
                  const QVector<Tick>& ticks, float yellowStart = std::numeric_limits<float>::quiet_NaN()) {
        m_min = min; m_max = max; m_redStart = redStart;
        m_yellowStart = std::isnan(yellowStart) ? redStart : yellowStart;
        m_ticks = ticks;
        // Re-map the CURRENT value onto the new axis. Without this the fill
        // keeps the fraction computed under the old bounds, and setValue()'s
        // unchanged-value early-return means a steady reading never corrects
        // it — an amplifier holding a constant carrier across a range change
        // (ACOM auto-range, SPE LOW/MID/HIGH) shows the old fraction against
        // the new scale indefinitely, misreporting RF output by hundreds of
        // watts. Snapped, not animated: the axis moved, the signal did not,
        // so sweeping the needle would show a change that never happened.
        //
        // Precondition: m_value is already in the NEW axis's units. A caller
        // that changes units as well as bounds (MeterApplet's °C/°F toggle)
        // must still follow with setValueImmediate to convert it — this
        // re-map fixes the axis, it cannot know the value moved too. That
        // pairing is now belt-and-braces rather than load-bearing: both
        // update() calls coalesce into one repaint, so the intermediate
        // fraction never reaches the screen.
        m_smooth.setTarget(fractionFor(m_value));
        m_smooth.snapToTarget();
        // Belt-and-braces: the smoother is at target, so the animation
        // callback would stop the timer on its own next tick. Stopping here
        // just saves that tick and its repaint — no case depends on it.
        if (m_animTimer.isActive()) m_animTimer.stop();
        publishAutomationState();
        update();
    }

    // ── Hover value readout ───────────────────────────────────────────────
    // Opt-in floating popup that shows the gauge's current numeric value
    // while the pointer hovers over the bar, reusing the same DragValuePopup
    // badge the sliders flash on keyboard/drag adjustment.  Handy on the TX
    // meters (SWR / forward power / ALC) where the bar scale alone doesn't
    // give an exact reading.  The badge lingers briefly after the pointer
    // leaves so a quick glance-and-move still registers. (#3936)
    //
    // Only ONE badge is ever on screen app-wide: showHoverPopup() closes the
    // previously-showing gauge's badge before raising its own. Each gauge owns
    // its own DragValuePopup (a lifetime choice — a shared static QWidget would
    // outlive QApplication), so without that hand-off nothing would ever hide
    // gauge A's badge on entering gauge B, and the linger below would leave two
    // stacked on screen at once. The stacked meters in TxApplet/PhoneCwApplet
    // sit two pixels apart, so the two badges land nearly on top of each other.
    using HoverValueFormatter = std::function<QString(float)>;

    // How long a badge can outlive a dropped physical leaveEvent before the
    // watchdog notices the pointer is gone (see syncHoverWatchdog). Worst case
    // on screen is this plus kHoverLingerMs. Public because the regression test
    // has to wait it out, and a second copy of the number would drift.
    static constexpr int kHoverWatchdogMs = 250;

    void setHoverValueFormatter(HoverValueFormatter formatter) {
        m_hoverFormatter = std::move(formatter);
    }

    void setHoverValuePopupEnabled(bool enabled) {
        m_hoverPopupEnabled = enabled;
        setMouseTracking(enabled);
        if (!enabled) {
            m_hovered = false;
            m_hoverWatchdog.stop();
            hideHoverPopupNow();
        }
    }

protected:
    void enterEvent(QEnterEvent* ev) override {
        QWidget::enterEvent(ev);
        m_hovered = true;
        m_lastHoverGlobal = ev->globalPosition().toPoint();
        syncHoverWatchdog();
        showHoverPopup(m_lastHoverGlobal);
    }

    void mouseMoveEvent(QMouseEvent* ev) override {
        QWidget::mouseMoveEvent(ev);
        m_lastHoverGlobal = ev->globalPosition().toPoint();
        // A no-button move that lands inside the bar means the pointer is over
        // it, whether or not the enterEvent arrived — so a dropped enter can't
        // suppress the readout for as long as the pointer sits there. The rect
        // test keeps this true even if a future subclass grabs the mouse (a
        // grab delivers moves from outside the widget too).
        if (ev->buttons() == Qt::NoButton
            && rect().contains(ev->position().toPoint()))
            m_hovered = true;
        syncHoverWatchdog();
        if (m_hovered)
            showHoverPopup(m_lastHoverGlobal);
    }

    void leaveEvent(QEvent* ev) override {
        QWidget::leaveEvent(ev);
        m_hovered = false;
        m_hoverWatchdog.stop();
        beginHoverLinger();
    }

    void hideEvent(QHideEvent* ev) override {
        QWidget::hideEvent(ev);
        // Qt does not guarantee a leaveEvent when the gauge is hidden or
        // reparented while the pointer is still over it (tab switch, applet
        // float/dock, window minimize). The popup is a top-level Qt::ToolTip
        // window, so without this it could linger orphaned on screen — close
        // it immediately and clear the hover state so it can't reappear stale
        // when the gauge is shown again.
        m_hovered = false;
        m_hoverWatchdog.stop();
        hideHoverPopupNow();
    }

    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        const int w = width();
        const int h = height();
        const int barY = 12;
        const int barH = h - barY - 2;
        const int barX = 0;
        const int barW = w;

        // Background
        p.fillRect(barX, barY, barW, barH, QColor(0x0a, 0x0a, 0x18));
        p.setPen(QColor(0x20, 0x30, 0x40));
        p.drawRect(barX, barY, barW - 1, barH - 1);

        // Filled portion (animated)
        int fillW = static_cast<int>(m_smooth.value() * barW);

        if (m_reversed) {
            // Reversed: bar fills from right to left, single color.
            // frac=1 (max) means empty, frac=0 (min) means full bar.
            int revFillW = barW - fillW;
            if (revFillW > 0)
                p.fillRect(barX + fillW + 1, barY + 1, revFillW - 2, barH - 2, QColor(0xff, 0x44, 0x44));
        } else if (m_fillFromRight) {
            // Mirrored fill — same min=empty/max=full mapping as the
            // normal mode, but the bar grows from the right edge inward.
            // Single color (red); zone-based tinting doesn't translate
            // cleanly to this orientation.
            if (fillW > 0)
                p.fillRect(barX + barW - fillW + 1, barY + 1, fillW - 2, barH - 2,
                           QColor(0xcc, 0x33, 0x33));
        } else {
            // Normal: three zones cyan → yellow → red
            int yellowX = static_cast<int>(((m_yellowStart - m_min) / (m_max - m_min)) * barW);
            int redX = static_cast<int>(((m_redStart - m_min) / (m_max - m_min)) * barW);

            if (fillW > 0) {
                // Green portion (below yellow zone)
                int greenW = qMin(fillW, yellowX);
                if (greenW > 0)
                    p.fillRect(barX + 1, barY + 1, greenW, barH - 2, QColor(0x1a, 0x90, 0x30));

                // Dirty yellow portion (between yellow and red zones)
                if (fillW > yellowX && yellowX < redX) {
                    int yw = qMin(fillW, redX) - yellowX;
                    if (yw > 0)
                        p.fillRect(barX + yellowX + 1, barY + 1, yw, barH - 2, QColor(0x99, 0x88, 0x00));
                }

                // Red portion (above red zone)
                if (fillW > redX) {
                    int rw = fillW - redX;
                    p.fillRect(barX + redX + 1, barY + 1, rw, barH - 2, QColor(0xcc, 0x33, 0x33));
                }
            }
        }

        // Peak-hold marker (thin white vertical line)
        if (m_peakEnabled) {
            float peakFrac = qBound(0.0f, (m_peakValue - m_min) / (m_max - m_min), 1.0f);
            int peakX = barX + static_cast<int>(peakFrac * barW);
            if (m_reversed) {
                // In reversed mode, peak is the lowest value (most compression)
                if (peakX > barX && peakX < barX + barW - 1) {
                    p.setPen(QColor(0xff, 0xff, 0xff));
                    p.drawLine(peakX, barY + 1, peakX, barY + barH - 2);
                }
            } else {
                if (peakX > barX && peakX < barX + barW - 1) {
                    p.setPen(QColor(0xff, 0xff, 0xff));
                    p.drawLine(peakX, barY + 1, peakX, barY + barH - 2);
                }
            }
        }

        // Tick labels along the top
        QFont tickFont = font();
        tickFont.setPixelSize(9);
        p.setFont(tickFont);

        for (const auto& tick : m_ticks) {
            float tf = (tick.value - m_min) / (m_max - m_min);
            int tx = barX + static_cast<int>(tf * barW);
            QColor tickColor = (tick.value >= m_redStart) ? QColor(0xcc, 0x33, 0x33)
                             : (tick.value >= m_yellowStart) ? QColor(0x99, 0x88, 0x00)
                             : QColor(0xc8, 0xd8, 0xe8);
            p.setPen(tickColor);
            const QFontMetrics fm(tickFont);
            int tw = fm.horizontalAdvance(tick.label);
            // Center label on tick position, clamp to widget bounds
            // Leave a small right margin so the last tick isn't flush to the edge
            int lx = qBound(2, tx - tw / 2, w - tw - 4);
            p.drawText(lx, 10, tick.label);
        }

        // Label in center of bar
        QFont lblFont = font();
        lblFont.setPixelSize(10);
        lblFont.setBold(true);
        p.setFont(lblFont);
        p.setPen(QColor(0xff, 0xff, 0xff));
        const QFontMetrics lfm(lblFont);
        int labelW = lfm.horizontalAdvance(m_label);
        p.drawText((w - labelW) / 2, barY + barH / 2 + lfm.ascent() / 2 - 1, m_label);
    }

private:
    // Map a physical value onto the normalised [0,1] axis fraction the
    // smoother and paintEvent work in. Every site that moves the fill must
    // agree on this — the constructor, setValue, setValueImmediate and
    // setRange. setRange's re-map exists because for a long time it was the
    // one site that didn't, so keep the arithmetic in exactly one place.
    float fractionFor(float v) const {
        return qBound(0.0f, (v - m_min) / (m_max - m_min), 1.0f);
    }

    // ── Automation-bridge introspection ───────────────────────────────────
    // HGauge is custom-painted and has no Q_OBJECT, so its label/value/scale
    // are invisible to the automation bridge's dumpTree. Mirror the state into
    // dynamic properties whenever it changes; the bridge reads them generically
    // via the meta-object (the same decoupled pattern SpectrumWidget uses for
    // noiseFloorDbm), so the °C/°F toggle and live overlay values become
    // numerically assertable without pixel-reading. Gated on AETHER_AUTOMATION
    // so production paints carry zero extra cost. (#3886 test support)
    void publishAutomationState() {
        static const bool kAutomation = qEnvironmentVariableIsSet("AETHER_AUTOMATION");
        if (!kAutomation) return;
        setProperty("gaugeLabel", m_label);
        setProperty("gaugeValue", m_value);
        // The DERIVED state — see filledFraction(). Every property above and
        // below reads correct while the bar paints something else, so without
        // this a bridge assertion reports a healthy gauge at the exact moment
        // it is misreading by hundreds of watts. (#3845)
        setProperty("gaugeFraction", m_smooth.value());
        setProperty("gaugeMin", m_min);
        setProperty("gaugeMax", m_max);
        setProperty("gaugeRedStart", m_redStart);
        setProperty("gaugeYellowStart", m_yellowStart);
        QStringList tickLabels;
        tickLabels.reserve(m_ticks.size());
        for (const auto& t : m_ticks)
            tickLabels << t.label;
        setProperty("gaugeTicks", tickLabels.join(QLatin1Char(',')));
    }

    QString hoverValueText() const {
        if (m_hoverFormatter)
            return m_hoverFormatter(m_value);
        QString text = QString::number(m_value, 'f', 1);
        if (!m_unit.isEmpty())
            text += QLatin1Char(' ') + m_unit;
        return text;
    }

    // The one gauge whose badge is currently claimed to be on screen, app-wide.
    // A raw non-owning pointer rather than a shared popup widget: DragValuePopup
    // is a top-level QWidget, and a function-local static one would be destroyed
    // after QApplication. Cleared by hideHoverPopupNow() and by ~HGauge.
    static HGauge*& activeHoverGauge() {
        static HGauge* gauge = nullptr;
        return gauge;
    }

    // Close this gauge's badge immediately and release the app-wide claim.
    // Reached cross-instance from another gauge's showHoverPopup() (same class,
    // so the private access holds) as well as from this gauge's own teardown
    // paths.
    void hideHoverPopupNow() {
        if (m_hoverPopup)
            m_hoverPopup->hideNow();
        if (activeHoverGauge() == this)
            activeHoverGauge() = nullptr;
    }

    void beginHoverLinger() {
        if (m_hoverPopup)
            m_hoverPopup->linger(kHoverLingerMs);
        // The claim is deliberately NOT released here. The badge is still on
        // screen for kHoverLingerMs, so it must stay findable — that is exactly
        // the window in which the next gauge needs to close it.
    }

    // Re-drive the badge from a value change: while hovered, the readout has to
    // track the meter. Recovery from a dropped leaveEvent is NOT done here —
    // see syncHoverWatchdog().
    void refreshHoverPopup() {
        if (m_hovered)
            showHoverPopup(m_lastHoverGlobal);
    }

    // Is the physical pointer over this bar right now? Independent of the
    // enter/leave stream, which is exactly what makes it useful as a recovery
    // check — and exactly what makes it wrong as a gate on the hover itself
    // (see syncHoverWatchdog).
    bool pointerPhysicallyInside() const {
        return isVisible() && rect().contains(mapFromGlobal(QCursor::pos()));
    }

    // Arm the recovery watchdog only while the pointer is REALLY over the bar.
    //
    // Qt does not guarantee a leaveEvent (the hideEvent override above exists
    // for the same reason), and a stuck m_hovered is self-sustaining: every
    // showValue() cancels the pending hide timer. So a dropped leave used to
    // pin the badge on screen indefinitely, frozen at the anchor the pointer
    // left it at.
    //
    // The recovery is on a timer rather than on setValue(), because setValue()
    // early-returns on an unchanged reading — a meter that settles (or stops
    // reporting entirely, as the TX gauges do on unkey) would never reach it,
    // and a quiescent meter is the more likely way to end up here than a busy
    // one.
    //
    // Gating on the physical cursor is what keeps the automation bridge's
    // synthetic hover working. `hover <target>` injects a QEnterEvent and a
    // no-button QMouseMove at the widget centre WITHOUT moving the real cursor
    // (AutomationServer::doHover), so an injected hover never arms the
    // watchdog and holds the badge until the driver sends an explicit
    // `hover <target> leave`. A validation run on every frame instead would
    // have torn the badge down under the driver on the first changing value.
    // The cost is that recovery covers physical hovers only — which is the
    // only case that can drop a leave, since the injected ones are delivered
    // by hand.
    void syncHoverWatchdog() {
        // Already armed and still hovered: the next tick re-validates within
        // kHoverWatchdogMs, so asking again in between learns nothing. Worth
        // short-circuiting because pointerPhysicallyInside() goes through
        // QCursor::pos(), which on XCB is a synchronous round-trip to the
        // display server — and mouseMoveEvent lands here on every single move
        // while the pointer is over the bar.
        if (m_hovered && m_hoverWatchdog.isActive())
            return;
        if (m_hovered && pointerPhysicallyInside()) {
            m_hoverWatchdog.start();
        } else if (!m_hovered) {
            m_hoverWatchdog.stop();
        }
        // m_hovered && !inside: an injected hover — deliberately left alone.
    }

    void onHoverWatchdogTick() {
        if (pointerPhysicallyInside())
            return;   // still there; keep watching
        m_hoverWatchdog.stop();
        m_hovered = false;
        beginHoverLinger();
    }

    void showHoverPopup(const QPoint& globalAnchor) {
        if (!m_hoverPopupEnabled)
            return;
        // Hand the badge over: close whichever gauge is currently showing one
        // before raising ours, so a traverse across adjacent meters can never
        // leave the previous gauge's badge lingering alongside this one.
        HGauge*& active = activeHoverGauge();
        if (active && active != this)
            active->hideHoverPopupNow();
        active = this;
        if (!m_hoverPopup)
            m_hoverPopup = new AetherSDR::DragValuePopup(this);
        // setValue() fires at ballistics-animation rate while hovered, but the
        // badge text and anchor are usually identical frame-to-frame. showValue()
        // re-runs adjustSize()/resize()/move()/raise() every call, so skip it
        // when nothing changed and the popup is already up. (isVisible() gates
        // the re-enter case, where the cache may still match but the popup was
        // hidden by leaveEvent/hideEvent and must be shown again.)
        const QString text = hoverValueText();
        if (m_hoverPopup->isVisible()
            && text == m_lastPopupText
            && globalAnchor == m_lastPopupAnchor)
            return;
        m_lastPopupText = text;
        m_lastPopupAnchor = globalAnchor;
        m_hoverPopup->showValue(globalAnchor, text);
    }

    float m_min, m_max, m_redStart, m_yellowStart;
    float m_value{0.0f};
    float m_peakValue{0.0f};
    bool  m_peakEnabled{false};
    bool  m_reversed{false};
    bool  m_fillFromRight{false};
    QString m_label, m_unit;
    QVector<Tick> m_ticks;

    // Hover value readout state (see setHoverValuePopupEnabled).
    //
    // The badge fades on the same schedule as every other DragValuePopup in the
    // app. #3936 shipped a bespoke 1000 ms here, but PR #2944 had already tested
    // that dimension and settled on 450 ms — 250 ms read as too fleeting, 750 ms
    // as sluggish. At 1000 ms a leave-and-move-on left the readout sitting over
    // the next control long after the pointer had gone.
    static constexpr int kHoverLingerMs = AetherSDR::DragValuePopup::kDefaultLingerMs;

    HoverValueFormatter m_hoverFormatter;
    AetherSDR::DragValuePopup* m_hoverPopup{nullptr};
    bool m_hoverPopupEnabled{false};
    bool m_hovered{false};
    QTimer m_hoverWatchdog;
    QPoint m_lastHoverGlobal;
    QString m_lastPopupText;    // last badge text/anchor pushed to the popup —
    QPoint m_lastPopupAnchor;   // used to skip redundant per-frame re-layouts

    // Shared meter ballistics — see MeterSmoother.h.
    MeterSmoother m_smooth;
    QTimer        m_animTimer;
    QElapsedTimer m_animElapsed;
};

// ── RelayBar: horizontal bar for relay position (0–255) ───────────────────────
// Supports mousewheel scrolling for manual relay adjustment (#469).

class RelayBar : public QWidget {
    Q_OBJECT

public:
    RelayBar(const QString& label, QWidget* parent = nullptr)
        : QWidget(parent), m_label(label)
    {
        setFixedHeight(18);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setFocusPolicy(Qt::TabFocus);
        setToolTip(tr("Scroll or use Up/Down keys to adjust relay position"));

        // Direct hardware state pushes (e.g. TGXL relay updates during an
        // ATU sweep) arrive unthrottled from the device — debounce the
        // accessibility announcement so a rapid sweep doesn't turn into an
        // updateAccessibility() storm. Same pattern as SMeterWidget/VfoWidget
        // (see docs/a11y.md "Throttle high-rate updaters").
        m_accessibilityTimer.setSingleShot(true);
        m_accessibilityTimer.setInterval(kAccessibilityAnnouncementIntervalMs);
        connect(&m_accessibilityTimer, &QTimer::timeout, this, [this]() {
            if (!hasFocus() || !QAccessible::isActive()) return;
            if (m_value == m_lastAccessibleValue) return;
            m_lastAccessibleValue = m_value;
            QAccessibleValueChangeEvent event(this, QVariant(m_value));
            QAccessible::updateAccessibility(&event);
        });
    }

    void setValue(int v) {
        if (m_value == v) return;
        m_value = v;
        update();
        if (hasFocus() && QAccessible::isActive() && !m_accessibilityTimer.isActive()) {
            m_accessibilityTimer.start();
        }
    }

    void setScrollEnabled(bool on) {
        m_scrollEnabled = on;
        setCursor(on ? Qt::SizeVerCursor : Qt::ArrowCursor);
    }

signals:
    void relayAdjusted(int direction);  // +1 scroll up, -1 scroll down

protected:
    void focusOutEvent(QFocusEvent* e) override {
        // setValue() is driven by hardware state pushes regardless of focus,
        // and both the schedule and publish gates require focus — so relay
        // positions can move unannounced while the bar is not focused. Forget
        // the last published value so a position that wandered away and back
        // is not mistaken for "unchanged" and swallowed by the dedup.
        m_accessibilityTimer.stop();
        m_lastAccessibleValue = std::numeric_limits<int>::min();
        QWidget::focusOutEvent(e);
    }

    void keyPressEvent(QKeyEvent* e) override {
        if (!m_scrollEnabled) { QWidget::keyPressEvent(e); return; }
        if (e->key() == Qt::Key_Up || e->key() == Qt::Key_Plus || e->key() == Qt::Key_Right) {
            emit relayAdjusted(+1);
            e->accept();
        } else if (e->key() == Qt::Key_Down || e->key() == Qt::Key_Minus || e->key() == Qt::Key_Left) {
            emit relayAdjusted(-1);
            e->accept();
        } else {
            QWidget::keyPressEvent(e);
        }
    }

    void wheelEvent(QWheelEvent* e) override {
        if (!m_scrollEnabled) {
            QWidget::wheelEvent(e);
            return;
        }
        // Clamp to ±1: KDE/Cinnamon send 960 per notch (#504)
        m_angleAccum += e->angleDelta().y();
        constexpr int step = 120;
        int emitted = 0;
        while (m_angleAccum >= step && emitted == 0)  { m_angleAccum -= step; emit relayAdjusted(+1); ++emitted; }
        while (m_angleAccum <= -step && emitted == 0) { m_angleAccum += step; emit relayAdjusted(-1); ++emitted; }
        if (emitted) m_angleAccum = 0;  // discard leftover inflation
        e->accept();
    }

    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        const int w = width();
        const int h = height();
        const int labelW = 24;
        const int valueW = 28;
        const int barX = labelW;
        const int barW = w - labelW - valueW - 4;
        const int barY = 3;
        const int barH = h - 6;

        // Label
        QFont f = font();
        f.setPixelSize(10);
        f.setBold(true);
        p.setFont(f);
        p.setPen(QColor(0xc8, 0xd8, 0xe8));
        p.drawText(0, barY + barH / 2 + QFontMetrics(f).ascent() / 2, m_label);

        // Bar background
        p.fillRect(barX, barY, barW, barH, QColor(0x0a, 0x0a, 0x18));
        p.setPen(QColor(0x20, 0x30, 0x40));
        p.drawRect(barX, barY, barW - 1, barH - 1);

        // Filled portion
        float frac = qBound(0.0f, m_value / 255.0f, 1.0f);
        int fillW = static_cast<int>(frac * (barW - 2));
        if (fillW > 0)
            p.fillRect(barX + 1, barY + 1, fillW, barH - 2, QColor(0x00, 0xb4, 0xd8));

        // Value text
        p.setPen(QColor(0xc8, 0xd8, 0xe8));
        const QString valText = QString::number(m_value);
        const QFontMetrics fm(f);
        p.drawText(w - valueW, barY + barH / 2 + fm.ascent() / 2, valText);
    }

private:
    static constexpr int kAccessibilityAnnouncementIntervalMs = 100;

    QString m_label;
    int m_value{0};
    bool m_scrollEnabled{false};
    int m_angleAccum{0};
    QTimer m_accessibilityTimer;
    int m_lastAccessibleValue{std::numeric_limits<int>::min()};
};

} // namespace AetherSDR
