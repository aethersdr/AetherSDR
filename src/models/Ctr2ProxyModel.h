#pragma once

#include "core/Ctr2HidPort.h"
#include "core/TcpByteProxy.h"

#include <QHostAddress>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>

class QTimer;

namespace AetherSDR {

class Ctr2UsbRelay;

// Operator-facing state for the CTR2 relay. Wi-Fi mode runs TcpByteProxy
// (the CTR2 connects to this PC over TCP); USB mode runs Ctr2UsbRelay over
// a selected HID device. Both forward to the radio AetherSDR is connected
// to, which the owner pushes in through setAetherRadio(); that destination is
// captured at start() and never retargeted while running. The model never
// reads RadioModel or a backend itself: the CTR2 is its own radio client.
// Nothing persists; the relay is off on every launch.
// Design: docs/ctr2-tcp-proxy-design.md, docs/ctr2-usb-relay-design.md.
class Ctr2ProxyModel : public QObject {
    Q_OBJECT

public:
    static constexpr quint16 kDefaultPort = 4992;

    enum class Transport { Wifi, Usb };

    explicit Ctr2ProxyModel(QObject* parent = nullptr);
    ~Ctr2ProxyModel() override;

    // USB HID access. Defaults to hidapi (absent when the build has none);
    // tests inject a device list and a port opener.
    using UsbDeviceSource = std::function<QList<Ctr2HidPort::DeviceInfo>()>;
    using UsbPortOpener =
        std::function<Ctr2HidPort*(const Ctr2HidPort::DeviceInfo&, QString* error)>;
    void setUsbBackend(UsbDeviceSource devices, UsbPortOpener opener);
    bool usbAvailable() const { return static_cast<bool>(m_usbOpener); }

    // Rescans local IPv4 addresses and CTR2-class USB HID devices.
    void refreshDevices();

    // Setters refuse (return false) while running.
    Transport transport() const { return m_transport; }
    bool setTransport(Transport transport);

    // Local non-loopback IPv4 addresses the operator may bind to (Wi-Fi).
    QStringList availableListenAddresses() const { return m_listenChoices; }
    QString listenAddress() const { return m_listenAddress; }
    bool setListenAddress(const QString& address);
    QString listenPortText() const { return m_listenPort; }
    bool setListenPortText(const QString& port);

    // HID interfaces on usage page 0xFF00, usage 0x01 (USB).
    QList<Ctr2HidPort::DeviceInfo> availableUsbDevices() const { return m_usbChoices; }
    QString usbDevicePath() const { return m_usbDevicePath; }
    bool setUsbDevicePath(const QString& path);

    // The radio AetherSDR is connected to. A null address means the relay
    // has no usable destination, and unavailableReason says why. A running
    // relay stops as soon as this is no longer the radio it started with:
    // AetherSDR's TX indicator is the operator's only view of that radio.
    void setAetherRadio(const QHostAddress& address, quint16 port, const QString& label,
                        const QString& unavailableReason);
    QString aetherRadioLabel() const { return m_aetherRadioLabel; }

    // Empty when Start may be pressed; otherwise the reason it may not.
    QString configurationProblem() const;
    bool isRunning() const;

    bool start();
    void stop();

    TcpByteProxy::State state() const;
    QString stateText() const;
    QString lastError() const;
    QString listenerEndpoint() const;
    QString peerEndpoint() const;
    QString radioEndpoint() const;
    TcpByteProxy::Stats stats() const;

signals:
    void configurationChanged();
    void listenAddressesChanged();   // also covers the USB device list
    void stateChanged();
    void endpointsChanged();
    void statsChanged();      // coalesced for display
    void lastErrorChanged();

private:
    static bool parsePort(const QString& text, quint16* port);
    static bool parseIpv4(const QString& text, QHostAddress* address);
    bool radioProblem(QString* problem) const;
    bool buildConfig(TcpByteProxy::Config* config, QString* problem) const;
    const Ctr2HidPort::DeviceInfo* selectedUsbDevice() const;
    bool usbActive() const { return m_activeTransport == Transport::Usb; }

    TcpByteProxy* m_proxy{nullptr};
    Ctr2UsbRelay* m_usb{nullptr};
    QTimer* m_statsTimer{nullptr};
    Transport m_transport{Transport::Wifi};
    Transport m_activeTransport{Transport::Wifi};
    QString m_usbStartError;
    QStringList m_listenChoices;
    QString m_listenAddress;
    QString m_listenPort{QString::number(kDefaultPort)};
    QList<Ctr2HidPort::DeviceInfo> m_usbChoices;
    QString m_usbDevicePath;
    QHostAddress m_aetherRadioAddress;
    quint16 m_aetherRadioPort{kDefaultPort};
    QString m_aetherRadioLabel;
    QString m_aetherRadioReason;
    QHostAddress m_runningRadioAddress;  // captured at start()
    quint16 m_runningRadioPort{0};
    QString m_runningRadioLabel;
    QString m_stopReason;                // why the relay stopped on its own
    UsbDeviceSource m_usbDevices;
    UsbPortOpener m_usbOpener;
};

} // namespace AetherSDR
