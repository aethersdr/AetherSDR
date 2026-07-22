#pragma once

#include "core/AcomProtocol.h"

#include <QWidget>
#include <QPushButton>
#include <QTimer>

class QLabel;

namespace AetherSDR {

class HGauge;

// Dedicated applet for an ACOM S-series amplifier (600S primary target) —
// a sibling of AmpApplet (PGXL), not a variant of it. Split out after
// discovering that bolting ACOM's richer telemetry onto AmpApplet's
// PGXL-shaped 3-gauge layout (a) couldn't actually match the proposed
// design, and (b) leaked a dead SWR/reflected-power toggle into PGXL's
// panel. See docs/architecture/acom-600s-amplifier-design.md.
//
// Unlike AmpApplet's PWR/SWR/Id gauges (PGXL's own data shape), ACOM's
// 0x2F telemetry frame reports forward power, reflected power, AND SWR as
// independently real fields — no toggle needed, all three get a permanent
// gauge row. PAM drain current (Id) has no protocol-defined scale to gauge
// against, so it's a plain text readout instead, same treatment as
// PA temperature already gets.
class AcomApplet : public QWidget {
    Q_OBJECT

public:
    explicit AcomApplet(QWidget* parent = nullptr);

    // Gauge scale configuration — call once the model is known (600S
    // default is set by the caller today; see design doc §6 for the full
    // 500S/600S/700S/1200S/2020S table).
    void setPowerRange(float nominalW, float maxW);
    void setReflectedRange(float nominalW, float maxW);

    // Telemetry
    void setForwardPower(float watts);
    void setReflectedPower(float watts);
    void setSwr(float swr);
    void setDrainCurrent(float amps);   // text readout, not a gauge
    void setDrainVoltage(float volts);  // HV1 rail
    void setTemp(float degC);
    void setBand(const QString& band);
    // Cumulative total operating time (not session uptime — the amp's
    // "system clock" telemetry field does not reset per power cycle).
    void setUptime(quint32 totalSeconds);
    void setSource(const QString& text);  // "SERIAL" or "NETWORK"
    void setMode(Acom::Mode mode);        // drives the status pill + STANDBY/OPERATE/OFF buttons
    void setFaultText(const QString& text);  // empty clears/hides the banner
    // Clear stays visible always (matches the proposed design) and is only
    // enabled — not hidden — while a fault is actually active.
    void setClearFaultEnabled(bool enabled);
    void setConnected(bool connected);    // shows/hides live controls, resets on disconnect

    // Model auto-detection/auto-ranging diagnostic info, shown as a tooltip
    // on the status pill rather than a permanent on-screen field — this is
    // "why is the scale what it is" context, not operational telemetry.
    // Chosen over the Peripherals settings row because no other device row
    // there surfaces detected-hardware info; connection status only.
    void setDiagnosticTooltip(const QString& text);

signals:
    void operateToggled(bool on);  // true = OPERATE clicked, false = STANDBY clicked
    void offClicked();
    void clearFaultClicked();

private:
    void updateTempLabel();
    void updateValueLabels();  // 10 Hz throttled label text refresh

    HGauge* m_pwrGauge{nullptr};
    HGauge* m_refGauge{nullptr};
    HGauge* m_swrGauge{nullptr};

    QLabel* m_pwrLabel{nullptr};
    QLabel* m_refLabel{nullptr};
    QLabel* m_swrLabel{nullptr};

    QLabel* m_statusPill{nullptr};
    QLabel* m_sourceLabel{nullptr};  // sits next to the status pill, not in the info grid

    // Info grid — 3 cells per row: temp/HV/Id, then band/uptime/clear.
    // Carrier frequency (byte 48-49) turned out to only read nonzero while
    // the amp senses actual RF drive — idle/standby leaves it blank most of
    // the time in practice, so it doesn't get a permanent slot; the clear-
    // fault button lives there instead (see setConnected/setClearFaultEnabled),
    // which also gives STANDBY/OPERATE/OFF a 3-button row with room to
    // breathe instead of 4 buttons clipping at default width.
    QPushButton* m_tempBtn{nullptr};
    QLabel* m_hvLabel{nullptr};
    QLabel* m_idLabel{nullptr};
    QLabel* m_bandLabel{nullptr};
    QLabel* m_uptimeLabel{nullptr};

    QLabel* m_faultLabel{nullptr};
    QPushButton* m_clearFaultBtn{nullptr};  // lives in the info grid, not the button row

    QPushButton* m_standbyBtn{nullptr};
    QPushButton* m_operateBtn{nullptr};
    QPushButton* m_offBtn{nullptr};

    QTimer m_labelTimer;
    QTimer* m_peakTimer{nullptr};
    float m_peakFwd{0.0f};

    float m_fwdWatts{0.0f};
    float m_reflectedWatts{0.0f};
    float m_swrVal{1.0f};
    float m_drainAmps{0.0f};
    float m_drainVolts{0.0f};
    float m_tempC{0.0f};
    bool m_hasTemp{false};
    bool m_tempFahrenheit{false};
    Acom::Mode m_mode{Acom::Mode::Unknown};

    // setTemp/setBand/setUptime/setDrainCurrent/setDrainVoltage arrive at
    // telemetry rate (well above 10 Hz in practice), same as the PWR/REF/SWR
    // gauge values above. Those already funnel display updates through the
    // 10 Hz m_labelTimer tick (updateValueLabels) instead of repainting and
    // re-announcing accessibility text on every frame; these dirty flags let
    // the info-grid/temp fields join that same throttle rather than flooding
    // screen-reader NameChanged events and label repaints at full telemetry
    // rate.
    bool m_tempDirty{false};
    bool m_diagDirty{false};  // Id + HV labels
    bool m_bandDirty{false};
    QString m_pendingBand;
    bool m_uptimeDirty{false};
    quint32 m_pendingUptimeSeconds{0};
};

}  // namespace AetherSDR
