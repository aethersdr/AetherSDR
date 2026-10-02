#pragma once

#include "core/Kpa500Protocol.h"

#include <QWidget>
#include <QComboBox>
#include <QPushButton>
#include <QTimer>

class QLabel;

namespace AetherSDR {

class HGauge;

// Dedicated applet for an Elecraft KPA500 serial amplifier — a peripheral
// accessory alongside AcomApplet/SpeApplet, not a variant of AmpApplet.
// See docs/architecture/elecraft-kpa500-kat500-design.md.
//
// Principle II: no control method latches its own state. setOperate() is
// wired to Kpa500Connection::setOperate(); the applet repaints when the
// ^OS poll reply lands.
class Kpa500Applet : public QWidget {
    Q_OBJECT

public:
    explicit Kpa500Applet(QWidget* parent = nullptr);

    // Apply a status snapshot — call on every statusUpdated() from the connection.
    void applyStatus(const Kpa500::Status& status);

    // Reflect transport state. Disconnected → resets all live readouts and
    // disables controls.
    void setConnected(bool connected);

    // Amp responding to polls within the silence window (vs. transport up but
    // amp not answering — e.g. USB-serial with the amp switched off).
    void setResponding(bool responding);

    // Diagnostic info (firmware version + serial number), shown as a tooltip
    // on the status pill so it doesn't consume permanent panel space.
    void setDiagnosticTooltip(const QString& text);

signals:
    void operateToggled(bool on);  // true = OPERATE clicked, false = STANDBY clicked
    void clearFaultClicked();
    // Emitted when the user changes the fan minimum speed combo (0–6).
    void fanSpeedChangeRequested(int n);

private:
    void updateStatusPill();
    void updateValueLabels();  // 10 Hz throttled text refresh

    HGauge* m_pwrGauge{nullptr};

    QLabel* m_pwrLabel{nullptr};
    QLabel* m_swrLabel{nullptr};
    QLabel* m_tempLabel{nullptr};
    QLabel* m_voltLabel{nullptr};
    QLabel* m_currLabel{nullptr};
    QLabel* m_bandLabel{nullptr};

    QLabel*      m_statusPill{nullptr};
    QPushButton* m_standbyBtn{nullptr};
    QPushButton* m_operateBtn{nullptr};

    QLabel*      m_faultLabel{nullptr};
    QPushButton* m_clearFaultBtn{nullptr};

    QComboBox*   m_fanCombo{nullptr};

    QTimer m_labelTimer;

    // Last decoded values, updated by applyStatus() at telemetry rate.
    // Rendered to labels on the 10 Hz m_labelTimer tick.
    float   m_fwdW{0.0f};
    float   m_swr{0.0f};
    float   m_tempC{0.0f};
    float   m_voltV{0.0f};
    float   m_currA{0.0f};
    int     m_band{-1};
    int     m_faultCode{-1};
    int     m_fanSpeed{-1};
    bool    m_operate{false};

    bool m_connected{false};
    bool m_responding{false};

    // One-shot identification fields — cached so the tooltip survives future
    // statusUpdated() calls that don't include these rarely-changing fields.
    QString m_firmware;
    QString m_serial;

    // Dirty flags for 10 Hz throttle — set by applyStatus(), consumed by tick.
    bool m_pwrDirty{false};
    bool m_infoDirty{false};
    bool m_faultDirty{false};
    bool m_fanDirty{false};
    bool m_statusDirty{false};
};

}  // namespace AetherSDR
