#pragma once

#include <QWidget>
#include <QPushButton>
#include <QComboBox>
#include <QTimer>

class QLabel;
class QVBoxLayout;
class QGridLayout;
class QSpacerItem;

namespace AetherSDR {

class HGauge;
class AmpModel;
class AccessoryPortRow;
class PanelKey;
struct AmpPortInfo;

// Amplifier applet for the 4O3A Power Genius XL (PGXL).
//
// Two presentations, chosen by setFloating() from the container's dock mode —
// the same docked-is-compact / popped-out-is-roomy split TunerApplet and
// SpeApplet use, and the same scaling machinery (AccessoryPanelWidgets.h).
//
// Docked in the applet rail (compact):
//  - PWR / SWR / Id horizontal gauges
//  - temperature, drain and mains readouts beside a fan-mode pull-down and
//    the OPERATE/STANDBY button
//
// Popped out or on the workspace canvas (expanded), laid out the way the
// amplifier's own front panel is:
//  - the same three gauges, taller, the SWR track carrying its scale gradient
//  - a status strip per RF port: PTT lamp, band, bias profile, source radio
//    — collapsed to a single STANDBY banner while the amplifier is in
//    standby, since nothing on those strips is live then
//  - the fan-speed and standby keys beside those strips, spanning both
//  - the temperatures, drain and mains voltages as one row along the bottom
//
// The per-port block is only on the amplifier's own port-9008 status; the
// radio-relayed "amplifier" object carries model, serial, ip, state and the
// antenna map and nothing else. Without the direct connection the strips show
// what is still knowable — which port is keyed, and which one transmit is
// routed to — rather than inventing the rest.
class AmpApplet : public QWidget {
    Q_OBJECT
public:
    explicit AmpApplet(QWidget* parent = nullptr);

    // Attach to an AmpModel (connects signals/slots). Supplies the per-port
    // block, the state word, the antenna map and the alert channel.
    void setAmpModel(AmpModel* model);

    void setFwdPower(float watts);
    void setSwr(float swr);
    void setTemp(float degC);
    void setTempB(float degC);
    void setDrainCurrent(float amps);
    void setDrainVoltage(float volts);
    void setMainsVoltage(int volts);
    void setState(const QString& state);
    void setFanMode(const QString& mode);  // STANDARD, CONTEST, BROADCAST
    void setMeff(const QString& meff);
    void setDirectConnected(bool direct);

    // Docked (applet rail) vs floating/canvas presentation. Driven by the
    // container's dockModeChanged — see the AMP entry in AppletPanel.
    void setFloating(bool floating);
    bool isFloating() const { return m_floating; }

    // The transmit slice's antenna ("ANT1"/"ANT2"), matched against the
    // amplifier's antenna → output map to outline the port transmit is
    // routed to.
    void setTxAntenna(const QString& antenna);

    // The floor the panel may be shrunk to. Derived from the minimum scale,
    // not from the children's current sizes — see panelMinimumSize. Public
    // because QWidget declares it so: narrowing an override's access hides it
    // from every caller that works through the base class, the layout
    // included.
    QSize minimumSizeHint() const override;

signals:
    void operateToggled(bool on);
    void fanModeChanged(const QString& mode);  // uppercase, ready for sendCommand

protected:
    // Keeps the alert overlay covering the applet as it resizes.
    void resizeEvent(QResizeEvent* event) override;

private:
    void buildUI();
    void buildExpandedUI();
    // The four readouts run as a row along the bottom of the panel and stack
    // in the rail's single-tile width. Same widgets either way — the grid is
    // re-flowed rather than the controls rebuilt.
    void applyTelemetryLayout();
    void updateTempLabel();
    void updateValueLabels();

    // Re-applies every presentation-dependent metric for the current
    // m_floating, and swaps which of the two operate controls is shown.
    void applyDensity();
    void applyDensityAtScale(qreal scale);
    // One uniform scale for every metric, from how much room the panel has.
    qreal contentScale() const;
    // Measures what the column costs at scale 1.0. Re-runs until the figure
    // stops moving, then never again — see the note on the implementation.
    void calibrateNaturalHeight();
    // Sizes the panel key: one seed size, scaled like every other metric.
    void applyKeySize(qreal scale);
    // The readouts carry their own colours (live vs. not proxied by the
    // radio) and their own scale, so one place resolves both.
    void applyTelemetryStyles(qreal scale);
    // Both operate controls wear the amplifier's current state, so a single
    // place decides what each of them says and how it is lit.
    void applyStateToControls();
    // Which way a press goes. One place, so the rail button and the panel key
    // can never ask the amplifier for opposite things.
    bool wantsOperate() const;
    // Same for the two fan controls — the rail's pull-down and the panel's
    // one-letter key are two faces of one mode.
    void applyFanControls();

    void updatePortRows();
    void applyPortInfo(AccessoryPortRow* row, const AmpPortInfo& info);
    // Outlines exactly the port transmit is routed to, or neither when that
    // is not knowable. Never both.
    void updateActivePort();
    void setAlertText(const QString& text);
    void applyAlertStyle();
    void layOutAlertOverlay();

    AmpModel* m_model{nullptr};

    // Bargraph gauges
    HGauge*  m_fwdGauge{nullptr};
    HGauge*  m_swrGauge{nullptr};
    HGauge*  m_idGauge{nullptr};

    // Left-side label+value (updated as telemetry arrives)
    QLabel*  m_pwrLabel{nullptr};   // "PWR 1148"
    QLabel*  m_swrLabel{nullptr};   // "SWR 1.2:1"
    QLabel*  m_idLabel{nullptr};    // "Id   39"

    // Right-side info column (one per gauge row)
    QPushButton* m_tempBtn{nullptr}; // "34.7/28.4 C"  (click to toggle C/F)
    QLabel*  m_vddLabel{nullptr};   // "Vdd  50.0 V"  (beside SWR row)
    QLabel*  m_vacLabel{nullptr};   // "Vac   240 V"  (beside Id  row)
    QLabel*  m_sourceLabel{nullptr}; // "● DIRECT" or "● RADIO"
    bool     m_directConnected{false};

    QWidget*     m_telemetryBox{nullptr};
    QGridLayout* m_telemetryGrid{nullptr};
    // Which way the grid is currently flowed, so a density pass that changes
    // nothing does not re-add four widgets to it.
    bool         m_telemetryInRow{false};

    QComboBox*   m_fanCombo{nullptr};
    QPushButton* m_operateBtn{nullptr};
    QString      m_fanMode{"STANDARD"};
    // Neither fan control is shown before the amplifier has reported a mode:
    // only the direct connection carries fanmode, and a control that cannot
    // say what it is set to is worse than none.
    bool         m_haveFanMode{false};

    // ── Expanded (floating / canvas) presentation ───────────────────────
    // Built up-front and hidden while docked, so switching presentation is a
    // visibility change rather than a rebuild — no widget is ever reparented
    // between the two layouts.
    QVBoxLayout* m_vbox{nullptr};
    bool         m_floating{false};
    qreal        m_appliedScale{1.0};
    // What the contents need at scale 1.0, measured from the laid-out column
    // rather than assumed. Measured once — see panelContentScale.
    qreal        m_naturalContentHeight{0.0};
    // Bounded so a column that never settles cannot re-measure forever.
    int          m_calibrationPasses{0};

    QWidget*     m_portRowsBox{nullptr};
    QWidget*     m_portLiveBox{nullptr};
    AccessoryPortRow* m_portA{nullptr};
    AccessoryPortRow* m_portB{nullptr};
    // Standby takes the whole port area: with the amplifier out of circuit
    // there is no per-port reading left to show.
    QLabel*      m_standbyBanner{nullptr};
    // Takes every pixel left over after the contents have been scaled, so the
    // controls keep the proportions the scale gave them instead of absorbing
    // the slack themselves. Its minimum is the gap that keeps them off the
    // frame.
    QSpacerItem* m_bottomStretch{nullptr};
    // The amplifier's alert channel — the same `M|<text>` frame the tuner
    // sends, on the same vendor's protocol. Not in any layout: it is a child
    // of the applet, sized to cover it and raised, because a fault is the
    // outcome of the thing the operator just did and a strip tucked above the
    // readings is missable at exactly the moment it matters.
    //
    // How long it stands is the amplifier's call: it sends the text and later
    // an empty frame to clear it. A local timer would have to guess that, and
    // would fight the device the moment it changed its mind.
    QLabel*      m_alertOverlay{nullptr};
    // The panel's discrete standby key — lit while the amplifier is in
    // standby, and commanding the opposite state either way. The rail keeps
    // the plain button: its column has room for one control, not a key of
    // panel proportions.
    PanelKey*    m_stbyKey{nullptr};
    // Fan speed on the panel: one letter — S, C, B — cycling the same three
    // modes the rail's pull-down lists. Square rather than letterbox, because
    // a single glyph has nothing for the extra width to hold, and exactly as
    // tall as the key beside it.
    PanelKey*    m_fanKey{nullptr};
    // The widest caption's natural width at scale 1.0, measured once before
    // the key has been given a fixed size — deriving it from the laid-out row
    // instead is a one-way ratchet.
    int          m_keySeedWidth{0};

    // The amplifier's own state word, upper-cased.
    //
    // Three questions get asked of it and they are NOT the same question.
    // `m_standby` is "is it in STANDBY" — that is what the banner and the
    // key's lit state follow, and it is FlexLib's Operate (State != Standby).
    // `m_operating` is "can it amplify right now" — IDLE, OPERATE and the two
    // TRANSMIT states, which is what the rail button's colour follows.
    // POWERUP, SELFCHECK and FAULT are neither: not standby, not operating.
    QString  m_stateWord;
    bool     m_standby{false};
    bool     m_operating{false};
    QString  m_txAntenna;

    // 100 ms timer — updates label text independently of gauge fill rate
    QTimer   m_labelTimer;
    // Peak hold: white tick on fwd gauge, cleared 2.5 s after last new peak
    QTimer*  m_peakTimer{nullptr};
    float    m_peakFwd{0.0f};

    // Cached telemetry values — gauges update every call, labels update at 10 Hz
    float    m_fwdWatts{0.0f};
    float    m_swrVal{1.0f};
    float    m_drainAmps{0.0f};
    float    m_tempA{0.0f};
    float    m_tempB{0.0f};
    bool     m_hasTempA{false};
    bool     m_hasTempB{false};
    bool     m_tempFahrenheit{false};
    int      m_mainsVolts{0};
};

} // namespace AetherSDR
