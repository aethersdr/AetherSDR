#pragma once

#include <QHash>
#include <QPointer>
#include <QWidget>

class QCheckBox;
class QJsonObject;
class QComboBox;
class QFormLayout;
class QLabel;
class QNetworkAccessManager;
class QNetworkReply;
class QSpinBox;
class QTimer;

namespace AetherSDR {

class RadioModel;

// GATE — Aether-gate device controls.
//
// Aether-gate presents non-Flex hardware (an SDRplay RSP, an HPSDR, an Icom) to
// AetherSDR as a FLEX-6600, which means everything reaches us as Flex wire text.
// That works for the things Flex has verbs for. It has none for an RSPdx's
// antenna port, bias-T, MW/DAB notches, HDR mode or AGC setpoint, so those
// controls could previously only be reached by opening the gate's own web panel
// in a browser. This applet brings them into the app.
//
// Deliberately NOT a fixed set of widgets: the gate reports what the attached
// device actually offers (it asks the driver — listAntennas/getSettingInfo) and
// the controls are built from that answer. An RSP1a, an RSPdx and an RTL stick
// share almost no settings, and hardcoding one device's list is the same
// mistake as hardcoding its sample rates.
//
// Presence is detected by PROBING the gate's control port rather than by
// sniffing the radio serial: the serial is operator-settable (--serial), so a
// renamed gate would vanish from the UI. If the endpoint answers, it is a gate.
class AetherGateApplet : public QWidget {
    Q_OBJECT
public:
    explicit AetherGateApplet(QWidget* parent = nullptr);

    void setRadioModel(RadioModel* model);

    // True once the gate's control port has answered at least once. AppletPanel
    // uses this to keep the button hidden for operators on a real Flex.
    bool gatePresent() const { return m_present; }

signals:
    void gatePresenceChanged(bool present);

protected:
    void showEvent(QShowEvent* e) override;
    void hideEvent(QHideEvent* e) override;

private:
    QString baseUrl() const;
    void reprobe();                               // address changed — ask again
    void poll();                                  // /status — cheap, on the timer
    void refreshDeviceControls();                 // /device — only when it can have changed
    void applyStatus(const QByteArray& json);
    void applyDeviceControls(const QByteArray& json);
    void buildDeviceControls(const QJsonObject& dev);
    void sendResolution();
    void get(const QString& path, void (AetherGateApplet::*handler)(const QByteArray&));
    void setPresent(bool present);

    QPointer<RadioModel>   m_model;
    QNetworkAccessManager* m_net{nullptr};
    QTimer*                m_timer{nullptr};
    bool                   m_present{false};
    int                    m_failures{0};
    QString                m_probedIp;            // the address we last asked

    // Header
    QLabel* m_status{nullptr};

    // Resolution
    QComboBox* m_span{nullptr};
    QComboBox* m_bins{nullptr};
    QLabel*    m_binWidth{nullptr};

    // Device controls, rebuilt from whatever the gate reports.
    QWidget*     m_deviceBox{nullptr};
    QFormLayout* m_deviceForm{nullptr};
    QComboBox*   m_antenna{nullptr};
    QHash<QString, QWidget*> m_settingWidgets;    // Soapy setting key -> control
    QString      m_controlsFingerprint;           // rebuild only when the SET changes
};

} // namespace AetherSDR
