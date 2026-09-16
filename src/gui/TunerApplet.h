#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QString>
#include <QWidget>

class QPushButton;
class QLabel;
class QTimer;
class QVBoxLayout;
class QHBoxLayout;
class QSpacerItem;

namespace AetherSDR {

class TunerModel;
class MeterModel;
struct TunerPortInfo;
class PanelKey;
class RelayDial;
class AccessoryPortRow;

// Tuner applet for the 4o3a Tuner Genius XL (TGXL).
//
// Two presentations, chosen by setFloating() from the container's dock mode —
// the same docked-is-compact / popped-out-is-roomy split SpeApplet uses.
//
// Docked in the applet rail (compact):
//  - Forward Power horizontal gauge (0–2000 W, red > 1500 W)
//  - SWR horizontal gauge (1.0–3.0, red > 2.5)
//  - C1 / L / C2 relay position bars (0–255)
//  - TUNE + OPERATE/BYPASS/STANDBY cycling button
//
// Popped out or on the workspace canvas (expanded), laid out the way the
// tuner's own front panel is:
//  - the same two gauges, taller, the SWR track carrying its scale gradient
//  - a status strip per RF port: PTT lamp, band, source, frequency, state
//    — collapsed to a single STANDBY banner when the tuner is in standby,
//    since nothing on those strips is live then
//  - C1 / L / C2 as round moving-needle dials
//  - discrete STBY / BYP / TUNE keys
//
// Only the PTT lamps and the state text on the port strips come from the
// tuner. The TGXL's own status carries nothing about what is feeding each
// port, so the source/band/frequency are filled from the radio this client is
// connected to — see setRadioModelName / setPortAFrequencyMhz.
class TunerApplet : public QWidget {
    Q_OBJECT

public:
    explicit TunerApplet(QWidget* parent = nullptr);

    // Attach to a TunerModel (connects signals/slots).
    void setTunerModel(TunerModel* model);

    // Store MeterModel pointer for reading SWR at tune completion.
    void setMeterModel(MeterModel* meter) { m_meter = meter; }

    // Switch Fwd Power gauge scale: barefoot (0–200W), Aurora (0–600W), amplifier (0–2000W).
    void setAmplifierMode(bool hasAmp);  // legacy — calls setPowerScale internally
    void setPowerScale(int maxWatts, bool hasAmplifier);

    // Docked (applet rail) vs floating/canvas presentation. Driven by the
    // container's dockModeChanged — see the TUN entry in AppletPanel.
    void setFloating(bool floating);
    bool isFloating() const { return m_floating; }

    // What is feeding the tuner, for the expanded port strips. The radio's
    // model names port A's source; blank until connected to a radio.
    void setRadioModelName(const QString& model);
    // Transmit frequency on port A, in MHz. Non-positive reads as "N/A",
    // which is also what port B always shows: a port on RF sense reports no
    // frequency, and guessing one would be inventing telemetry.
    void setPortAFrequencyMhz(double mhz);
    void setRadioConnected(bool connected);
    // The transmit slice's antenna ("ANT1"/"ANT2"). The tuner's own status
    // cannot say which port carries transmit — with one radio cabled to both
    // it reports both live — so the port is identified by matching this
    // against each port's configured antenna, the same comparison FlexLib
    // makes before it will autotune.
    void setTxAntenna(const QString& antenna);

public slots:
    // The tuner reports forward power and SWR twice — as radio-relayed AMP
    // meters and on its own port-9010 status. Same measurement, different
    // rate, so these stamp their arrival and the relay wins while it is
    // fresh. See kRelayMeterFreshnessMs.
    //
    // Callers use these, never updateMeters(), which applies blind: a second
    // unmediated writer is the defect this path exists to remove, and it is
    // private for the same reason AmpApplet::setFwdPower is.
    void setRadioMeters(float fwdPower, float swr);
    void setDeviceMeters(float fwdPower, float swr);

    // The floor the panel may be shrunk to. Derived from the minimum scale,
    // not from the children's current sizes. Public because QWidget declares
    // it so — narrowing an override's access hides it from every caller that
    // works through the base class, the layout included.
    QSize minimumSizeHint() const override;

protected:
    // Keeps the alert overlay covering the applet as it resizes.
    void resizeEvent(QResizeEvent* event) override;

private:
    // Applies a forward-power (W) / SWR pair to the gauges blind, without
    // consulting the source rule. Private precisely so it cannot be reached
    // from the wiring — both stamped entry points above end here.
    void updateMeters(float fwdPower, float swr);

    void buildUI();
    void buildExpandedUI(QVBoxLayout* vbox);
    void syncFromModel();
    void cycleOperateState();
    void updateAntennaButtons(int antA);
    void updateValueLabels();
    // Re-applies every presentation-dependent metric and swaps which control
    // group is shown, for the current m_floating.
    void applyDensity();
    void applyDensityAtScale(qreal scale);
    // One uniform scale for every metric, from how much room the panel has.
    qreal contentScale() const;
    // Measures what the column costs at scale 1.0. Runs once.
    void calibrateNaturalHeight();
    // TUNE exists once per presentation, so tuning feedback (TUNING… / the
    // settled SWR / the restored idle style) has to reach both buttons. Every
    // site that touches a TUNE button goes through here so the two cannot
    // drift apart.
    void applyTuneButtonText(const QString& text);
    void applyTuneButtonStyle(const char* styleTemplate);
    void updatePortRows();
    void applyPortInfo(AccessoryPortRow* row, const TunerPortInfo& info);
    // Outlines exactly the port carrying transmit, or neither when that is
    // not yet knowable. Never both: only one port can be transmitting.
    void updateActivePort();
    void setAlertText(const QString& text);
    // The tuner sends no severity with an alert, so it is read off the text.
    void applyAlertStyle();
    void layOutAlertOverlay();
    // Sizes the three discrete keys: one seed size for all of them, scaled
    // like every other metric on the panel.
    void applyKeySize(qreal scale);
    // Applies a rail button's style with a font size that fits its caption,
    // and remembers the style so a later refit can re-apply it.
    void applyRailStyle(QPushButton* btn, const QString& base);
    // Re-fits both rail captions to the width the rail has given them.
    void fitRailCaptions();
    int  fittedRailFontPx(QPushButton* btn) const;
    // Operate / bypass / standby drive three different port-area presentations
    // (per-port state, a spanning bypass overlay, or the standby banner).
    // Both callers of it need the same three-way decision, so it lives here.
    void applyTunerStateToPorts(bool operate, bool bypass);

    TunerModel* m_model{nullptr};
    MeterModel* m_meter{nullptr};

    // Gauges (custom-painted inner widgets)
    QWidget* m_fwdGauge{nullptr};
    QWidget* m_swrGauge{nullptr};

    // Row labels that show live numeric values ("PWR 987", "SWR 1.2:1")
    QLabel*  m_pwrLabel{nullptr};
    QLabel*  m_swrLabel{nullptr};
    QTimer*  m_labelClearTimer{nullptr};  // holds label visible after power drops
    bool     m_labelShowing{false};

    // True only while the tuner is actually matching. It drives what the TUNE
    // key says and what pressing it does, so the label and the action can
    // never disagree about which one the operator is looking at.
    bool     m_tuning{false};

    // Relay bars
    QWidget* m_c1Bar{nullptr};
    QWidget* m_lBar{nullptr};
    QWidget* m_c2Bar{nullptr};

    // Buttons
    QPushButton* m_tuneBtn{nullptr};
    QPushButton* m_operateBtn{nullptr};

    // ── Expanded (floating / canvas) presentation ───────────────────────
    // Built up-front and hidden while docked, so switching presentation is a
    // visibility change rather than a rebuild — no widget is ever reparented
    // between the two layouts.
    QVBoxLayout* m_vbox{nullptr};
    bool         m_floating{false};
    qreal        m_appliedScale{1.0};
    // The rail buttons' style sheets without their font-size, kept so the
    // size can be re-chosen and the sheet re-applied when the rail's width
    // or the caption changes.
    QHash<QPushButton*, QString> m_railStyles;
    // What the contents need at scale 1.0, measured from the laid-out column
    // rather than assumed, so the height budget below tracks the panel as
    // rows are added to it instead of drifting out of date.
    qreal        m_naturalContentHeight{0.0};

    QWidget*     m_dockedControls{nullptr};  // relay bars + cycling OPERATE
    QWidget*     m_panelControls{nullptr};   // dials + STBY/BYP/TUNE
    QWidget*     m_portRowsBox{nullptr};
    QWidget*     m_portLiveBox{nullptr};   // the two strips + the bypass overlay
    AccessoryPortRow* m_portA{nullptr};
    AccessoryPortRow* m_portB{nullptr};
    // Bypass is one device-wide field, so it is shown once across both strips
    // rather than repeated in each — repeating it reads as though a port could
    // be bypassed on its own.
    QLabel*      m_bypassSpan{nullptr};
    // Standby takes the whole port area: with the tuner out of circuit there
    // is no per-port reading left to show.
    QLabel*      m_standbyBanner{nullptr};
    // Takes every pixel left over after the contents have been scaled. The
    // controls keep the proportions the scale gave them instead of absorbing
    // the slack themselves — a tall narrow window otherwise stretches the
    // keys into columns while the dials stay small. Its minimum is the gap
    // that keeps the controls off the frame.
    QSpacerItem* m_bottomStretch{nullptr};
    // Tuner alerts ("LOW RF POWER"). Shown in both presentations: a tune that
    // refused to run is the operator's problem to fix wherever the applet
    // happens to be, so the rail tile does not get to stay silent about it.
    // Tuner alerts take the whole applet for a moment rather than sharing a
    // row with the readings: "Tuned SWR: 1.13:1" and "LOW RF POWER" are the
    // outcome of the thing the operator just did, and a strip tucked above
    // the port rows is missable at exactly the moment it matters. Not in any
    // layout — it is a child of the applet, sized to cover it and raised.
    //
    // How long it stands is the tuner's call, not ours: it sends the text and
    // later an empty frame to clear it, and the interval between them is its
    // own (~1.9 s after a successful tune, ~3.0 s after LOW RF POWER). A
    // local timer would have to guess that, and would fight the device the
    // moment it changed its mind.
    QLabel*      m_alertOverlay{nullptr};
    // A station reaching the tuner only through the radio gets no alert
    // channel: the relayed object carries no message, result or SWR field,
    // and the radio's own atu status stays TUNE_MANUAL_BYPASS throughout a
    // TGXL tune because the TGXL is the one tuning. The settled SWR is still
    // available there through the meters, so the completion notice is
    // composed locally and shown in the same banner rather than that
    // population losing the result entirely.
    QTimer*      m_relayResultTimer{nullptr};
    QTimer*      m_relayDwellTimer{nullptr};
    bool         m_alertIsGood{false};
    RelayDial*   m_c1Dial{nullptr};
    RelayDial*   m_lDial{nullptr};
    RelayDial*   m_c2Dial{nullptr};
    PanelKey*    m_stbyBtn{nullptr};
    PanelKey*    m_bypBtn{nullptr};
    PanelKey*    m_panelTuneBtn{nullptr};
    QHBoxLayout* m_keysLayout{nullptr};
    // The widest caption's natural width at scale 1.0, measured once before
    // any key has been given a fixed size. Deriving it from the laid-out
    // column instead is a one-way ratchet: fixing the keys makes the column's
    // own size hint that fixed width, so they can never grow back.
    int          m_keySeedWidth{0};

    QString m_radioModelName;
    double  m_portAFreqMhz{0.0};
    QString m_txAntenna;
    bool    m_radioConnected{false};

    // Antenna switch buttons (TGXL 3x1)
    QPushButton* m_ant1Btn{nullptr};
    QPushButton* m_ant2Btn{nullptr};
    QPushButton* m_ant3Btn{nullptr};
    QWidget*     m_antContainer{nullptr};

    // Meter values (updated by updateMeters)
    // When the radio relay last delivered a meter sample. See setDeviceMeters().
    // Monotonic: a wall-clock gate wedges shut across a backwards clock step.
    QElapsedTimer m_radioMeters;

    float m_fwdPower{0.0f};
    float m_swr{1.0f};

    // Peak hold for fwd gauge
    QTimer* m_peakTimer{nullptr};
    float   m_peakFwd{0.0f};

    // Relay values (updated from model)
    int m_relayC1{0};
    int m_relayL{0};
    int m_relayC2{0};


    // setPowerScale() no-ops when neither input moved (#4845) — it's called
    // on every RadioModel::infoChanged, most of which carry no scale-relevant
    // change, and gauge->setRange() forces a repaint.
    int  m_lastMaxWatts{-1};
    bool m_lastHasAmplifier{false};
    bool m_havePowerScale{false};
};

} // namespace AetherSDR
