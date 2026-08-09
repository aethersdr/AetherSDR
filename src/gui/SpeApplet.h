#pragma once

#include "core/SpeProtocol.h"

#include <QWidget>
#include <QPushButton>
#include <QTimer>

class QLabel;

namespace AetherSDR {

class HGauge;

// Dedicated applet for an SPE Expert linear amplifier (1.3K-FA/1.5K-FA/
// 2K-FA) — a sibling of AcomApplet and AmpApplet (PGXL), not a variant of
// either, per the dedicated-applet-per-peripheral lesson recorded in
// docs/architecture/acom-600s-amplifier-design.md §2. See
// docs/architecture/spe-expert-amplifier-design.md for this applet's own
// design note.
//
// The SPE's Status string reports output power, antenna-side SWR, AND
// ATU-input-side SWR as independently real fields, so all three get a
// permanent gauge row (same all-real-fields-get-gauges reasoning as
// AcomApplet's Power/Reflected/SWR). Supply voltage/current and the
// heatsink temperatures are text readouts — the protocol defines no
// nominal/max scale to size an axis against.
//
// Buttons mirror the amplifier's own front panel keys (every remote command
// IS a keystroke in this protocol): OPER/STBY toggle, power-level cycle
// (the button label shows the CURRENT level, LOW/MID/HIGH), TUNE, OFF,
// INPUT, ANT, and the ▲/▼ arrow keys — which on the Expert adjust the
// drive power the amplifier requests from the radio over CAT. Band keys
// are not exposed: the amp follows the radio's band via CAT/RF sensing.
// SET/DISPLAY menu navigation and manual L/C tuning are deliberately not
// exposed — SPE reserves complex operations for their own KTerm
// application, and blind menu navigation without the amp's display is a
// foot-gun.
class SpeApplet : public QWidget {
    Q_OBJECT

public:
    explicit SpeApplet(QWidget* parent = nullptr);

    // Gauge scale configuration — call when the model is known (the Status
    // ID field identifies it on the very first poll reply; see
    // SpeConnection::modelChanged).
    void setPowerRange(float nominalW, float warnW, float maxW);
    void setModelName(const QString& displayName);

    // Telemetry
    void setForwardPower(float watts);
    void setSwrAnt(float swr);
    void setSwrAtu(float swr);
    void setSupplyVoltage(float volts);   // text readout, not a gauge
    void setSupplyCurrent(float amps);    // text readout, not a gauge
    // Heatsink temperatures. The amp reports degrees in whichever unit its
    // own display is configured for (C or F, not indicated on the wire —
    // spec §5), so values are shown verbatim with a bare ° sign rather
    // than guessing a unit or offering a conversion toggle. Lower/combiner
    // are only real on the 2K-FA; hasCombiner hides them elsewhere.
    void setTemps(int upper, int lower, int combiner, bool hasCombiner);
    void setBand(const QString& band);
    void setAntenna(int antenna, QChar atuState);
    void setInputPort(int input);
    // "LOW"/"MID"/"HIGH" — shown in the info grid AND as the power-level
    // button's own label, mirroring the reference application.
    void setPowerLevel(const QString& levelName);
    void setMode(bool operate, bool transmitting); // drives pill + OPR/STBY button
    void setFaultText(const QString& text);        // empty clears/hides the banner
    void setSource(const QString& text);           // "SERIAL" or "NETWORK"
    void setConnected(bool connected);   // shows/hides live controls, resets on disconnect
    // Transport up but the amplifier isn't answering polls (ser2net with the
    // amp switched off). Commands are disabled and the pill goes neutral
    // until it answers again — the transport-level connected state alone
    // can't tell this apart.
    void setResponding(bool responding);

signals:
    void powerOnClicked();     // hardware power-ON pulse (works while the amp is silent)
    void operateClicked();     // OPERATE key — toggles STANDBY <-> OPERATE
    void powerLevelClicked();  // POWER key — cycles LOW/MID/HIGH
    void tuneClicked();        // TUNE key
    void offClicked();         // SWITCH OFF key
    void inputClicked();       // INPUT key — toggles input 1/2
    void antennaClicked();     // ANTENNA key
    void driveUpClicked();     // ▲ (RIGHT-arrow key) — raise requested drive power
    void driveDownClicked();   // ▼ (LEFT-arrow key) — lower requested drive power

private:
    void updateValueLabels();  // 10 Hz throttled label text refresh
    void updateCommandsEnabled();
    void applyModePill();
    // Blanks every reading back to its not-yet-known state. Shared by the
    // disconnect path and the stopped-answering path — both mean "what is on
    // screen is no longer telemetry", and a frozen-but-plausible panel is the
    // worse failure of the two.
    void clearTelemetry();

    HGauge* m_pwrGauge{nullptr};
    HGauge* m_swrAntGauge{nullptr};
    HGauge* m_swrAtuGauge{nullptr};

    QLabel* m_pwrLabel{nullptr};
    QLabel* m_swrAntLabel{nullptr};
    QLabel* m_swrAtuLabel{nullptr};

    QLabel* m_statusPill{nullptr};
    QLabel* m_sourceLabel{nullptr};
    QLabel* m_modelLabel{nullptr};

    // Info grid — 3 cells per row: temp / V / I, then band / antenna / input·level.
    QLabel* m_tempLabel{nullptr};
    QLabel* m_voltLabel{nullptr};
    QLabel* m_currLabel{nullptr};
    QLabel* m_bandLabel{nullptr};
    QLabel* m_antLabel{nullptr};
    QLabel* m_inputLabel{nullptr};

    QLabel* m_faultLabel{nullptr};

    QPushButton* m_onBtn{nullptr};
    QPushButton* m_operateBtn{nullptr};
    QPushButton* m_pwrLevelBtn{nullptr};
    QPushButton* m_tuneBtn{nullptr};
    QPushButton* m_offBtn{nullptr};
    QPushButton* m_inputBtn{nullptr};
    QPushButton* m_antBtn{nullptr};
    QPushButton* m_driveDownBtn{nullptr};
    QPushButton* m_driveUpBtn{nullptr};

    QTimer m_labelTimer;
    QTimer* m_peakTimer{nullptr};
    float m_peakFwd{0.0f};

    // Last applied power-gauge scale — setPowerRange() no-ops on repeats so
    // the wiring can re-derive the level-dependent scale on every status
    // frame (10/s) without triggering a repaint per frame.
    float m_rangeNominal{-1.0f};
    float m_rangeWarn{-1.0f};
    float m_rangeMax{-1.0f};

    float m_fwdWatts{0.0f};
    float m_swrAntVal{1.0f};
    float m_swrAtuVal{1.0f};
    float m_supplyVolts{0.0f};
    float m_supplyAmps{0.0f};
    bool  m_operate{false};
    bool  m_transmitting{false};
    bool  m_connected{false};
    bool  m_responding{false};

    // Telemetry arrives at the 10 Hz poll rate — same order as ACOM's ~10 Hz
    // push, but the same dirty-flag/10 Hz-timer throttle is kept so label
    // repaints and accessibility NameChanged events stay bounded regardless
    // of what a future faster poll (or a chatty firmware) delivers.
    bool m_tempDirty{false};
    QString m_pendingTempText;
    bool m_diagDirty{false};
    bool m_bandDirty{false};
    QString m_pendingBand;
    bool m_antDirty{false};
    QString m_pendingAntText;
    bool m_inputDirty{false};
    int m_inputPort{0};       // 0 = not yet reported
    QString m_levelName;      // empty = not yet reported
};

}  // namespace AetherSDR
