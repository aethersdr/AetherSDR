#pragma once

#include <QElapsedTimer>
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

    // Two transports carry the same two readings. Both stamp their arrival and
    // defer to applyMeters(), which picks one — see the note there. Callers
    // must use these rather than setFwdPower/setSwr, which apply blind.
    // radio-relayed AMP meters. powerValid=false means this update carried no
    // forward-power/SWR sample (a TEMP- or DRV-only change), and must neither
    // move the gauges nor count as the relay being live — see the note there.
    void setRadioMeters(float watts, float swr, bool powerValid = true);
    void setDeviceMeters(float watts, float swr);  // the amplifier's own socket

    // Exciter power measured at the amplifier's input (the PGXL "DRV" meter).
    // valid=false blanks the row: not every amplifier publishes one, and a
    // drive gauge resting at zero would read as "no drive" rather than
    // "not measured".
    void setDrivePower(float watts, bool valid);

    void setTemp(float degC);
    void setTempB(float degC);
    void setDrainCurrent(float amps);
    void setDrainVoltage(float volts);
    void setMainsVoltage(int volts);
    void setState(const QString& state);
    void setFanMode(const QString& mode);  // STANDARD, CONTEST, BROADCAST
    // The Maximum Efficiency Algorithm's reported state — ACTIVE, STANDBY or
    // OFF — or empty when the amplifier has not reported one. `settable` is
    // false until the whole `setup` write group is known, which is what a
    // write needs; the control is shown but inert until then.
    void setMeffa(const QString& state, bool settable);
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
    // The operator asked to enable or disable MEffA. Only ever the one bit:
    // whether the amplifier then reports ACTIVE or STANDBY is its own call,
    // decided by the PA bias class. See AmpModel::meffa().
    void meffaToggled(bool enabled);

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
    // MEffA wears three states and the operator controls one bit of them, so
    // one place decides what both controls say and how each is lit.
    void applyMeffaControls();

    void updatePortRows();
    void applyPortInfo(AccessoryPortRow* row, const AmpPortInfo& info);
    // Outlines exactly the port transmit is routed to, or neither when that
    // is not knowable. Never both.
    void updateActivePort();
    // One rule, one place: the radio-relayed AMP meters and the amplifier's own
    // port-9008 status carry the SAME measurement — on a steady carrier the
    // relayed FWD meter and the device's `fwd` field agree to within 0.05 dB —
    // so the choice between them is about rate, not truth. The relay arrives
    // with the radio's meter packets (~20 fps); the device is polled at 5 Hz.
    // The relay therefore wins while its sample is fresh, and the device feed
    // takes over when the radio is not publishing amplifier meters at all
    // (no relay, or before the meter manifest lands). Last-writer-wins between
    // two live sources is what this replaces.
    void applyMeters(float watts, float swr);
    // Apply blind, without consulting the source rule. Private precisely so
    // that they cannot be reached from the wiring: reintroducing a second
    // unmediated writer is the defect this whole path exists to remove.
    void setFwdPower(float watts);
    void setSwr(float swr);
    void updateDriveLabel();

    void setAlertText(const QString& text);
    void applyAlertStyle();
    void layOutAlertOverlay();

    AmpModel* m_model{nullptr};

    // Bargraph gauges
    HGauge*  m_fwdGauge{nullptr};
    HGauge*  m_drvGauge{nullptr};
    HGauge*  m_swrGauge{nullptr};
    HGauge*  m_idGauge{nullptr};

    // Left-side label+value (updated as telemetry arrives)
    QLabel*  m_pwrLabel{nullptr};   // "PWR 1148"
    QLabel*  m_drvLabel{nullptr};   // "DRV   11"
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
    // MEffA: the rail's button and the panel's key, two faces of one control.
    // Placed beside the fan controls because it is the same kind of thing — a
    // run-time mode the amplifier holds until it is told otherwise, not a
    // stored setting (see AmpModel::setMeffaEnabled on why no `save` follows).
    QPushButton* m_meffaBtn{nullptr};
    PanelKey*    m_meffaKey{nullptr};
    QString      m_meffaState;
    bool         m_meffaSettable{false};
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

    // When the radio relay last delivered a power/SWR sample. See
    // setDeviceMeters() for the rule it decides. Monotonic on purpose: an
    // elapsed-time gate measured off the wall clock wedges shut for the length
    // of any backwards clock step.
    QElapsedTimer m_radioMeters;

    // Cached telemetry values — gauges update every call, labels update at 10 Hz
    float    m_fwdWatts{0.0f};
    float    m_swrVal{1.0f};
    float    m_drvWatts{0.0f};
    bool     m_haveDrive{false};
    float    m_drainAmps{0.0f};
    float    m_tempA{0.0f};
    float    m_tempB{0.0f};
    bool     m_hasTempA{false};
    bool     m_hasTempB{false};
    bool     m_tempFahrenheit{false};
    int      m_mainsVolts{0};
};

} // namespace AetherSDR
