#pragma once

#include <QWidget>
#include <QSize>
#include <QString>
#include <QTimer>
#include <limits>

class QLabel;

namespace AetherSDR {

// Widgets used only by TunerApplet's expanded (popped-out / on-canvas)
// presentation, which lays the TGXL out the way the tuner's own front panel
// does. They live here rather than in HGauge.h beside RelayBar so the ~20
// applets that include that header for its gauge don't pay to compile a
// presentation none of them use.
//
// Colours resolve through ThemeManager tokens, so the panel follows the active
// theme instead of hardcoding the hardware's palette.

// ── RelayDial ───────────────────────────────────────────────────────────────
//
// One relay bank (C1 / L / C2) as a round moving-needle dial — the same 0–255
// datum RelayBar draws as a horizontal bar, and the same interaction contract:
// scroll or Up/Down to step the relay when the direct TGXL connection is up
// (#469), with the accessibility announcement debounced because an ATU sweep
// pushes positions faster than a screen reader can speak them.
//
// The needle sweeps a conventional 270° meter arc — 0 at lower-left, 255 at
// lower-right, increasing clockwise. The hardware's own needle mapping is not
// reproduced: the panel photo shows three low values (48/16/64) whose needles
// do not order consistently, so there is nothing there to copy faithfully.
class RelayDial : public QWidget {
    Q_OBJECT

public:
    explicit RelayDial(const QString& label, QWidget* parent = nullptr);

    void setValue(int v);
    int  value() const { return m_value; }

    QSize sizeHint() const override;

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
    int  m_value{0};
    bool m_scrollEnabled{false};
    int  m_angleAccum{0};
    QTimer m_accessibilityTimer;
    int  m_lastAccessibleValue{std::numeric_limits<int>::min()};
};

// ── TgxlPortRow ─────────────────────────────────────────────────────────────
//
// One RF port's status strip: port letter, PTT lamp, band chip, signal source,
// frequency, and the tuner's operate state — the two-row block the front panel
// puts between the meters and the relay dials.
//
// Only the PTT lamp and the state text come from the tuner itself. The source,
// band and frequency describe what is feeding the port, which the Flex-relayed
// ATU status does not carry; TunerApplet fills them from the radio it is
// connected to (see TunerApplet::setPortASource / setPortAFrequency).
class TgxlPortRow : public QWidget {
    Q_OBJECT

public:
    explicit TgxlPortRow(const QString& portLetter, QWidget* parent = nullptr);

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
    // The port carrying the radio's transmit path, outlined to match the
    // panel's highlight of the port in use.
    void setActive(bool active);

    // Re-resolves every themed stylesheet for the current state. Called on
    // construction and whenever a value that carries its own colour changes.
    void applyTheme();

protected:
    void paintEvent(QPaintEvent*) override;

private:
    void updateAccessibleText();

    QString m_portLetter;
    QLabel* m_portLabel{nullptr};
    QLabel* m_pttLabel{nullptr};
    QLabel* m_bandLabel{nullptr};
    QLabel* m_sourceLabel{nullptr};
    QLabel* m_freqLabel{nullptr};
    QLabel* m_stateLabel{nullptr};

    bool m_ptt{false};
    bool m_active{false};
    bool m_bypassed{false};
};

}  // namespace AetherSDR
