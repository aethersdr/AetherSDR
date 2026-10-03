#pragma once

#include <QPushButton>
#include <QWidget>
#include <QSize>
#include <QString>
#include <QTimer>
#include <limits>

class QLabel;

namespace AetherSDR {

// Panel scaling shared by the tuner and amplifier front panels: every metric
// scales from one factor derived from the available room, so a bigger window
// grows the contents, not the padding (like CrossNeedleMeterWidget, but applied
// per metric to a widget tree).

// Below this the type stops being legible; above it the panel is being
// stretched rather than filled.
constexpr qreal kPanelMinScale = 0.8;
constexpr qreal kPanelMaxScale = 3.0;

// The gap below the controls. Deliberately not scaled: it exists so they
// clear the frame rather than sit against it, which is a constant few pixels
// at any size. It is also the floor of the pad that absorbs whatever the
// scaling did not use.
constexpr int kPanelBottomGap = 8;

// How long a radio-relayed meter sample stays authoritative over the
// accessory's own socket. PGXL and TGXL publish fwd power/SWR both as relayed
// AMP meters (~20 fps) and on their management socket (polled 1–5 Hz); the
// values agree within 0.05 dB, so the faster relay wins while fresh. 1500 ms
// exceeds the slowest relay gap yet hands over within ~1 s. Shared by both
// accessory applets.
constexpr qint64 kRelayMeterFreshnessMs = 1500;

// Panel keys are letterbox-shaped rather than square. Their height is tied to
// the instruments beside them so the two groups read as one row of peers, and
// the width follows from it at this aspect.
constexpr qreal kPanelKeyAspect = 16.0 / 9.0;

// Scale for a panel `available` px wide whose contents cost `naturalHeight` at
// scale 1.0 and whose widest row costs `designWidth`. The height term excludes
// the pad's minimum, so the pad drains first before contents shrink.
// `naturalHeight` must be measured once at scale 1.0, never re-derived from a
// scaled layout (rounding and widget minimums feed back and it never
// settles). `designWidth` is a constant for the same reason: a measured width
// divided by the scale runs away.
qreal panelContentScale(const QSize& available, qreal designWidth, qreal naturalHeight);

// The floor a panel may be shrunk to: what it needs at kPanelMinScale, not
// what its children happen to need right now. Letting the layout answer that
// instead makes the floor follow the current scale, and it ratchets — every
// metric the scale sizes raises the minimum as the panel grows, so a panel
// enlarged once can never be made small again.
QSize panelMinimumSize(qreal designWidth, qreal naturalHeight);

// One key box. `heightPx` is already scaled (it is tied to whatever sits
// beside the keys); `seedWidthPx` is the widest caption's natural width at
// scale 1.0, so the narrowest caption gets the same box as the widest rather
// than the box its own text happened to need. 16:9 off the height is roomy
// enough that the caption floor should never bind, but a caption that
// outgrew it would be clipped rather than wrapped.
QSize panelKeySize(int heightPx, int seedWidthPx, qreal scale);

// Widgets used only by TunerApplet's expanded (popped-out / on-canvas)
// presentation, which lays the TGXL out the way the tuner's own front panel
// does. They live here rather than in HGauge.h beside RelayBar so the ~20
// applets that include that header for its gauge don't pay to compile a
// presentation none of them use.
//
// Colours resolve through ThemeManager tokens, so the panel follows the active
// theme instead of hardcoding the hardware's palette.

// RelayDial: one relay bank (C1 / L / C2) as a needle dial over the same
// 0–255 datum RelayBar draws. Scroll or Up/Down steps the relay when the
// direct TGXL connection is up (#469); the accessibility announcement is
// debounced because ATU sweeps outpace screen readers. The needle sweeps a
// 270° arc, 0 lower-left to 255 lower-right clockwise (the hardware's own
// mapping isn't consistent enough to copy).
class RelayDial : public QWidget {
    Q_OBJECT

public:
    explicit RelayDial(const QString& label, QWidget* parent = nullptr);

    void setValue(int v);
    int  value() const { return m_value; }

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
    // Preferred diameter, so the dial grows with the panel around it rather
    // than sitting at one size in a window twice its natural height. The
    // painting is already radius-relative, so this is all the dial needs.
    void setPreferredDiameter(int px);

    // Enabled only while the direct port-9010 connection is up — the relays
    // cannot be stepped over the Flex-relayed status path.
    void setScrollEnabled(bool on);

signals:
    void relayAdjusted(int direction);  // +1 step up, -1 step down

protected:
    void paintEvent(QPaintEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void focusOutEvent(QFocusEvent*) override;

private:
    void refreshAccessibleValue();

    static constexpr int kAccessibilityAnnouncementIntervalMs = 100;

    QString m_label;
    int  m_diameter{76};
    int  m_value{0};
    bool m_scrollEnabled{false};
    int  m_angleAccum{0};
    QTimer m_accessibilityTimer;
    int  m_lastAccessibleValue{std::numeric_limits<int>::min()};
};

// PanelKey: a control-row key (STBY / BYP / TUNE) sized from the panel scale,
// not its caption, so all keys match. It reports a small scale-independent
// minimum; a fixed size would ratchet the layout's minimum up so the panel
// could never shrink again.
class PanelKey : public QPushButton {
    Q_OBJECT

public:
    explicit PanelKey(const QString& text, QWidget* parent = nullptr);

    // The size the panel's scale has chosen for this key.
    void setTargetSize(const QSize& size);

    QSize sizeHint() const override { return m_target; }
    QSize minimumSizeHint() const override;

private:
    QSize m_target{0, 0};
};

// AccessoryPortRow: one RF port's status strip (letter, PTT lamp, band chip,
// source, frequency, tuner state). Only PTT and state come from the tuner;
// source, band and frequency aren't in the Flex-relayed ATU status, so
// TunerApplet fills them from the radio (setPortASource / setPortAFrequency).
class AccessoryPortRow : public QWidget {
    Q_OBJECT

public:
    explicit AccessoryPortRow(const QString& portLetter, QWidget* parent = nullptr);

    void setPtt(bool keyed);
    // Empty band or a non-positive frequency renders as "N/A" — the honest
    // reading for a port whose source cannot report one (RF sense).
    void setBandText(const QString& band);
    void setSourceText(const QString& source);
    void setFrequencyMhz(double mhz);
    // Per-port state. Empty hides the cell — which is what bypass does: it is
    // a tuner-wide condition, shown once across both strips rather than
    // repeated per port (see TunerApplet's spanning indicator).
    void setStateText(const QString& state);
    // While the tuner is bypassed the frequency is still correct but is not
    // being matched, so it reads in the bypass colour rather than as a good
    // reading.
    void setBypassed(bool bypassed);
    // The amplifier's bias profile for this port (the tuner has none, and
    // hides the cell by never setting it). Empty hides it.
    void setBiasText(const QString& bias);
    // The tuner knows the frequency it is matching; the amplifier does not
    // report one per port, so its strips leave the cell out rather than
    // standing an N/A in it forever.
    void setFrequencyVisible(bool visible);
    // The tuner only knows what is on a port while it is hearing it, so its
    // strips leave the source cell out rather than guess at one. The
    // amplifier's source is configuration and stays shown — the default.
    void setSourceVisible(bool visible);
    // The port carrying the radio's transmit path, outlined to match the
    // panel's highlight of the port in use.
    void setActive(bool active);

    // Re-resolves every themed stylesheet for the current state. Called on
    // construction and whenever a value that carries its own colour changes.
    void applyTheme();
    // Scales every metric on the strip — cell widths, padding and type — so a
    // taller panel gets a proportionally larger strip rather than the same
    // strip with more space around it. 1.0 is the compact rail size.
    void setScale(qreal scale);

protected:
    void paintEvent(QPaintEvent*) override;

private:
    void updateAccessibleText();
    int  px(int base) const;   // a design-pixel metric at the current scale

    QString m_portLetter;
    QLabel* m_portLabel{nullptr};
    QLabel* m_pttLabel{nullptr};
    QLabel* m_bandLabel{nullptr};
    QLabel* m_biasLabel{nullptr};
    QLabel* m_sourceLabel{nullptr};
    QLabel* m_freqLabel{nullptr};
    QLabel* m_stateLabel{nullptr};

    qreal m_scale{1.0};
    bool m_ptt{false};
    bool m_active{false};
    bool m_bypassed{false};
};

}  // namespace AetherSDR
