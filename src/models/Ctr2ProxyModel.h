#pragma once

#include "core/Ctr2HidPort.h"
#include "core/TcpByteProxy.h"

#include <QHostAddress>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>

#include <functional>
#include <vector>

class QTimer;

namespace AetherSDR {

class Ctr2UsbRelay;

// Operator-facing state for the CTR2 relay. It relays any mix of controllers
// at once: Wi-Fi runs one TcpByteProxy that accepts up to kMaxWifiClients
// CTR2s (each connects to this PC over TCP), and every checked USB device gets
// its own Ctr2UsbRelay. Every link opens its own radio connection, so each
// CTR2 is a separate client on the radio and the radio's own client limits
// apply. All links forward to the radio AetherSDR is connected to, which the
// owner pushes in through setAetherRadio(); that destination is captured at
// start() and never retargeted while running. The model never reads
// RadioModel or a backend itself: the CTR2 is its own radio client.
// Nothing persists; the relay is off on every launch. When the build has
// hidapi, Wi-Fi starts unchecked and every recognized CTR2 is checked.
// Design: docs/ctr2-tcp-proxy-design.md, docs/ctr2-usb-relay-design.md.
class Ctr2ProxyModel : public QObject {
    Q_OBJECT

public:
    static constexpr quint16 kDefaultPort = 4992;
    // A resource bound, not a protocol limit: each Wi-Fi CTR2 holds a socket
    // pair and up to two relay budgets, and the radio caps its own clients.
    static constexpr int kMaxWifiClients = 8;

    // One row of relay status: the Wi-Fi listener, a CTR2 connected to it,
    // or a USB device.
    struct Link {
        enum class Kind { WifiListener, WifiClient, Usb };
        Kind kind{Kind::Usb};
        QString name;      // listener or peer endpoint, or the USB device label
        TcpByteProxy::State state{TcpByteProxy::State::Stopped};
        QString stateText;
        TcpByteProxy::Stats stats;
        QString error;     // the link's last local error
    };

    explicit Ctr2ProxyModel(QObject* parent = nullptr);
    ~Ctr2ProxyModel() override;

    // USB HID access. Defaults to hidapi (absent when the build has none);
    // tests inject a device list and a port opener.
    using UsbDeviceSource = std::function<QList<Ctr2HidPort::DeviceInfo>()>;
    using UsbPortOpener =
        std::function<Ctr2HidPort*(const Ctr2HidPort::DeviceInfo&, QString* error)>;
    void setUsbBackend(UsbDeviceSource devices, UsbPortOpener opener);
    bool usbAvailable() const { return static_cast<bool>(m_usbOpener); }

    // Rescans local IPv4 addresses and CTR2-class USB HID devices. A newly
    // seen recognized CTR2 is checked; the operator's choices for devices
    // still present stand, and devices that have gone are dropped.
    void refreshDevices();

    // Setters refuse (return false) while running.
    bool wifiEnabled() const { return m_wifiEnabled; }
    bool setWifiEnabled(bool enabled);

    // Local non-loopback IPv4 addresses the operator may bind to (Wi-Fi).
    QStringList availableListenAddresses() const { return m_listenChoices; }
    QString listenAddress() const { return m_listenAddress; }
    bool setListenAddress(const QString& address);
    QString listenPortText() const { return m_listenPort; }
    bool setListenPortText(const QString& port);

    // HID interfaces on usage page 0xFF00, usage 0x01 (USB).
    QList<Ctr2HidPort::DeviceInfo> availableUsbDevices() const { return m_usbChoices; }
    // Checked device paths, in device-list order.
    QStringList selectedUsbDevices() const;
    bool isUsbDeviceSelected(const QString& path) const { return m_usbSelected.contains(path); }
    bool setUsbDeviceSelected(const QString& path, bool selected);
    // Linux: a checked CTR2's device node exists but this user may not open
    // it, i.e. the udev access rule is not installed yet. Always false on
    // other platforms or with no device checked.
    bool usbDeviceNeedsAccessRule() const;

    // The radio AetherSDR is connected to. A null address means the relay
    // has no usable destination, and unavailableReason says why. A running
    // relay stops as soon as this is no longer the radio it started with:
    // AetherSDR's TX indicator is the operator's only view of that radio.
    void setAetherRadio(const QHostAddress& address, quint16 port, const QString& label,
                        const QString& unavailableReason);
    QString aetherRadioLabel() const { return m_aetherRadioLabel; }

    // Empty when Start may be pressed; otherwise the reason it may not.
    QString configurationProblem() const;
    // True while any link is up.
    bool isRunning() const;

    // Starts Wi-Fi (if enabled) and every checked USB device, each on its own.
    // A link that fails to start keeps its error in links() and does not hold
    // back the others. True if anything is running.
    bool start();
    void stop();

    // The links of the current (or last) run, Wi-Fi first, then USB in
    // device-list order. Empty before the first Start and after Stop.
    QList<Link> links() const;
    // Why the relay stopped on its own, else the first link error.
    QString lastError() const;
    QString radioEndpoint() const;   // the destination captured at start()

    // True while a relaying USB device negotiated the AudioSpectrum link
    // extension (AetherKnob; never a stock CTR2).
    bool audioSpectrumWanted() const;
    // One AudioSpectrum frame to every device that wants it: bars in dBFS,
    // log-spaced from lowHz to spanHz (a non-finite bar reads as silence).
    // False if none wants it now or every such link is busy.
    bool sendAudioSpectrum(int lowHz, int spanHz, const std::vector<float>& barsDb);
    // The span to send for a passband: its width, rounded up to a clean
    // scale step, capped at the audio's Nyquist frequency.
    static int audioSpectrumSpanHz(int filterLo, int filterHi, double sampleRate);
    // The low edge of the log frequency axis for a span.
    static int audioSpectrumLowHz(int spanHz);
    // Lower edge of bar i of n on that axis (i == n gives spanHz).
    static double audioSpectrumBandEdgeHz(int lowHz, int spanHz, int n, int i);

signals:
    void configurationChanged();
    void listenAddressesChanged();   // also covers the USB device list
    void stateChanged();
    void endpointsChanged();
    void statsChanged();      // coalesced for display
    void lastErrorChanged();
    void extensionsChanged();   // a USB device's link extensions; audioSpectrumWanted() may move

private:
    struct UsbLink {
        QString path;
        QString label;
        Ctr2UsbRelay* relay{nullptr};
        QString startError;   // the device could not be opened
    };

    static bool parsePort(const QString& text, quint16* port);
    static bool parseIpv4(const QString& text, QHostAddress* address);
    bool radioProblem(QString* problem) const;
    bool buildConfig(TcpByteProxy::Config* config, QString* problem) const;
    const Ctr2HidPort::DeviceInfo* usbDevice(const QString& path) const;
    void startUsb(const Ctr2HidPort::DeviceInfo& device);
    void clearUsbLinks();
    void coalesceStats();
    void stateMoved();

    TcpByteProxy* m_proxy{nullptr};
    std::vector<UsbLink> m_usbLinks;   // the current (or last) run's devices
    bool m_wifiActive{false};          // Wi-Fi was part of the current (or last) run
    QTimer* m_statsTimer{nullptr};
    bool m_wifiEnabled{true};
    QStringList m_listenChoices;
    QString m_listenAddress;
    QString m_listenPort{QString::number(kDefaultPort)};
    QList<Ctr2HidPort::DeviceInfo> m_usbChoices;
    QSet<QString> m_usbSelected;
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
