#pragma once

#include <QAbstractButton>
#include <QList>
#include <QString>
#include <QVariantMap>
#include <QWidget>

class QHBoxLayout;
class QTimer;

namespace AetherSDR {

// Connection state of one radio, as shown in the title bar's radio tabs.
//
// The dot colour is a *redundant* encoding: every tab also spells the state out
// on its second line, because the operator community includes colour-blind and
// low-vision users and WCAG 1.4.1 forbids colour as the only carrier.
enum class RadioTabStatus {
    Available,   // discovered, idle — slate
    Connected,   // this client owns the session — green, slow pulse
    InUse        // another station has it — amber
};

QString radioTabStatusText(RadioTabStatus status);

// One entry in the title bar's radio strip.
struct RadioTabEntry {
    QString        id;         // stable key — serial, or family:address for HL2
    QString        name;       // "Hermes-Lite 2", "FLEX-6600"
    QString        detail;     // free text appended after the status, e.g. a callsign
    QString        transport;  // "SmartLink" | "192.168.1.21" — shown in the popover
    RadioTabStatus status{RadioTabStatus::Available};

    bool operator==(const RadioTabEntry& o) const
    {
        return id == o.id && name == o.name && detail == o.detail
            && transport == o.transport && status == o.status;
    }
};

// A single radio tab.  QAbstractButton (not a styled QWidget) so it is
// tab-focusable, space/enter-activatable, and reported to screen readers as a
// button with a name — all of which a bare paint-only widget would lose.
class RadioTab : public QAbstractButton {
    Q_OBJECT
    // Drives the connected dot's 2.4 s glow.  A property rather than a plain
    // member so QPropertyAnimation can own the easing.
    Q_PROPERTY(qreal pulse READ pulse WRITE setPulse)

public:
    explicit RadioTab(const RadioTabEntry& entry, QWidget* parent = nullptr);

    void setEntry(const RadioTabEntry& entry);
    const RadioTabEntry& entry() const { return m_entry; }

    qreal pulse() const { return m_pulse; }
    void  setPulse(qreal v);

    // ── Radio-link indicator ────────────────────────────────────────────────
    // The active tab's status dot doubles as the discovery/heartbeat light the
    // bar used to carry as a separate lamp.  One dot now answers both "which
    // radio is this" and "is its link alive", which is where an operator looks
    // anyway — and it removes an indicator whose meaning had to be learned.
    //
    // `overrideColor` invalid means "use the status colour"; a valid colour is
    // the link state speaking over it (amber while discovering, red on loss).
    void setLinkOverride(const QColor& overrideColor, bool alarm);
    void setAlarmVisible(bool on);          // driven by the bar's blink phase
    void setBeatColor(const QColor& color); // throttle tint; invalid = dot colour

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void paintEvent(QPaintEvent* ev) override;
    void enterEvent(QEnterEvent* ev) override;
    void leaveEvent(QEvent* ev) override;
    void focusInEvent(QFocusEvent* ev) override;
    void focusOutEvent(QFocusEvent* ev) override;

private:
    void refreshAccessibility();
    QString statusLine() const;

    QColor dotColor() const;

    RadioTabEntry m_entry;
    qreal         m_pulse{0.0};
    QColor        m_overrideColor;   // invalid = use the status colour
    QColor        m_beatColor;       // invalid = use the dot colour
    bool          m_alarm{false};    // link lost — red, and blinking if enabled
    bool          m_alarmVisible{true};
    bool          m_hovered{false};
    // Focus ring is drawn only for keyboard-delivered focus — see the paint
    // path.  Mouse and initial-window focus leave the tab unringed.
    bool          m_focusVisible{false};
};

// The radio strip: one tab per known radio, plus a "+" that opens the
// discovered-radios popover.
class RadioTabBar : public QWidget {
    Q_OBJECT

public:
    explicit RadioTabBar(QWidget* parent = nullptr);

    // Replace the tab set.  A no-op when `radios` matches what is already
    // shown, so discovery's steady 5 s re-announce doesn't rebuild widgets
    // (and destroy keyboard focus) forty times a minute.
    void setRadios(const QList<RadioTabEntry>& radios);
    void setActiveRadio(const QString& id);
    QString activeRadioId() const { return m_activeId; }

    // Rows offered by the "+" popover — discovery's current view, which is a
    // superset of the tab strip while a radio is still unconfigured.
    void setDiscoveredRadios(const QList<RadioTabEntry>& radios);

    // Animated glow on the active tab's dot.  Follows the operator's existing
    // "Blink status indicator" preference so one switch governs every
    // animated status light in the title bar.
    void setPulseEnabled(bool on);

    // ── Radio-link indicator (folded in from the old standalone lamp) ───────
    // `overrideColor` invalid = show the active tab's own status colour;
    // `alarm` = link lost, which blinks when the operator has blink enabled and
    // holds solid red when they don't (losing a link must stay visible either
    // way).  `pulseLink()` is one heartbeat: the dot's glow swells and decays.
    void setLinkIndicator(const QColor& overrideColor, bool alarm);
    void pulseLink(const QColor& beatColor = QColor());

    // Open the discovered-radios popover programmatically (the automation
    // bridge and the keyboard both need a non-mouse entry point).
    void showDiscoveryPopover();
    bool isDiscoveryPopoverVisible() const;

    // Introspection for the automation bridge (`titlebar` model).
    QVariantMap state() const;

signals:
    void radioActivated(const QString& id);
    void discoveryPopoverRequested();
    void connectManuallyRequested();

private:
    void rebuild();
    void applyActiveState();
    // Push the current link state (override colour, alarm phase, glow level)
    // onto the tabs — the active one carries it, the rest stay neutral.
    void applyLinkVisuals();

    QHBoxLayout*         m_layout{nullptr};
    QList<RadioTabEntry> m_radios;
    QList<RadioTabEntry> m_discovered;
    QList<RadioTab*>     m_tabs;
    QAbstractButton*     m_addButton{nullptr};
    QWidget*             m_popover{nullptr};
    QString              m_activeId;
    QTimer*              m_pulseTimer{nullptr};   // glow decay after a heartbeat
    QTimer*              m_alarmTimer{nullptr};   // 500 ms red blink on link loss
    bool                 m_pulseEnabled{true};
    qreal                m_pulseLevel{0.0};
    QColor               m_linkOverride;
    QColor               m_beatColor;
    bool                 m_alarm{false};
    bool                 m_alarmVisible{true};
};

} // namespace AetherSDR
