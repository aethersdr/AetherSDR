#pragma once

#include "core/Ctr2FirmwareUpdater.h"
#include "core/Ctr2RelaySettings.h"
#include "core/Ctr2HidPort.h"
#include "core/TcpByteProxy.h"

#include <QHostAddress>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

#include <functional>
#include <vector>

class QTimer;

namespace AetherSDR {

class Ctr2UsbRelay;

// Operator-facing state for the CTR2 relay. Wi-Fi mode runs TcpByteProxy
// (the CTR2 connects to this PC over TCP); USB mode runs Ctr2UsbRelay over
// a selected HID device. Both forward to the radio AetherSDR is connected
// to, which the owner pushes in through setAetherRadio(); that destination is
// captured at start() and never retargeted while running. The model never
// reads RadioModel or a backend itself: the CTR2 is its own radio client.
// Nothing persists; the relay is off on every launch. USB mode is the default
// when the build has hidapi, with the first recognized CTR2 preselected.
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
    // Opens a device accepting only the given link extensions: Firmware for
    // an update, none to read the firmware version. Empty keeps updates and
    // the idle version readout unavailable.
    using UsbCapsPortOpener = std::function<Ctr2HidPort*(const Ctr2HidPort::DeviceInfo&,
                                                         quint32 hostCapabilities, QString* error)>;
    void setUsbBackend(UsbDeviceSource devices, UsbPortOpener opener,
                       UsbCapsPortOpener firmwareOpener = {});
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
    // Linux: the selected CTR2's device node exists but this user may not
    // open it, i.e. the udev access rule is not installed yet. Always false
    // on other platforms, in Wi-Fi mode, or with no device selected.
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
    bool isRunning() const;

    bool start();
    void stop();
    // Stop pressed by the operator: also holds off the automatic start until
    // AetherSDR next connects to a radio.
    void stopByOperator();

    // Starts the relay by itself whenever AetherSDR is connected to a radio,
    // with the setup it last started with (Ctr2RelaySettings, saved), and
    // keeps trying while the device is missing. In USB mode only a device
    // whose firmware relays over USB (AetherCTR2, AetherKnob; usbLink()) is
    // started: stock CTR2 firmware has no USB mode.
    bool autoStart() const { return m_settings.autoStart; }
    void setAutoStart(bool on);
    static constexpr int kAutoStartRetryMs = 5000;
    // While the relay is stopped, USB devices are rescanned this often, so a
    // controller that switched firmware (and so changed its USB identity)
    // appears without pressing Rescan.
    static constexpr int kDeviceRescanMs = 2000;

    TcpByteProxy::State state() const;
    QString stateText() const;
    QString lastError() const;
    QString listenerEndpoint() const;
    QString peerEndpoint() const;
    QString radioEndpoint() const;
    TcpByteProxy::Stats stats() const;

    // True while the USB relay is up with a device that negotiated the
    // AudioSpectrum link extension (AetherKnob; never a stock CTR2).
    bool audioSpectrumWanted() const;
    // And it also negotiated AudioSpectrumLinear: windows not starting at 0.
    bool audioSpectrumLinearWanted() const;
    // True while the USB relay is up with a device that negotiated CwText.
    bool cwTextWanted() const;
    // Decoded CW text for the device, with the decoder's cost (lower is
    // better). False if not wanted now or nothing printable is left.
    bool sendCwText(const QString& text, float cost);

    // Firmware version of the selected USB device: from the relay while it
    // runs, otherwise read when the device is selected or rescanned. Empty
    // for a device that does not report one (any stock CTR2).
    QString usbFirmwareVersion() const;
    bool firmwareUpdateAvailable() const { return static_cast<bool>(m_firmwareOpener); }
    // Whether the selected USB device takes FIRMWARE commands (restart into
    // its other firmware or download mode, restartUsbDevice()), as read with
    // its version. A maker's stock firmware may offer them without taking
    // updates over the link.
    bool usbFirmwareCommands() const;
    bool firmwareUpdateRunning() const;
    // Updates the selected USB device with image (Ctr2FirmwareUpdater::readImage).
    // Stops the relay first and starts it again afterwards if it was
    // running. False, with *problem, when it cannot start.
    bool startFirmwareUpdate(const Ctr2FirmwareUpdater::Image& image, QString* problem);
    // Before the device starts checking the image; the device keeps its
    // current firmware. Later the update runs to its end.
    void cancelFirmwareUpdate();
    // Restarts the selected USB device into its other firmware or its ROM
    // download mode (Ctr2FirmwareUpdater::restartDevice); the relay stops
    // for it and does not come back, as the device leaves.
    bool restartUsbDevice(Ctr2FirmwareUpdater::Restart target, QString* problem);
    Ctr2FirmwareUpdater* firmwareUpdater() const { return m_firmware; }
    // One AudioSpectrum frame: bars in dBFS, log-spaced from lowHz to spanHz
    // (a non-finite bar reads as silence). False if not wanted now or the
    // link is busy.
    // With linear, the bars split lowHz..spanHz evenly (AudioSpectrumLinear;
    // only when audioSpectrumLinearWanted()).
    bool sendAudioSpectrum(int lowHz, int spanHz, const std::vector<float>& barsDb,
                           bool linear = false);
    // The span to send for a passband: its width, rounded up to a clean
    // scale step, capped at the audio's Nyquist frequency.
    static int audioSpectrumSpanHz(int filterLo, int filterHi, double sampleRate);
    // The low edge of the log frequency axis for a span.
    static int audioSpectrumLowHz(int spanHz);
    // Lower edge of bar i of n on that axis (i == n gives spanHz).
    static double audioSpectrumBandEdgeHz(int lowHz, int spanHz, int n, int i, bool linear = false);
    // CW: the linear window centered on the pitch (AudioSpectrumLinear).
    static void audioSpectrumCwWindow(int filterLo, int filterHi, int pitchHz, double sampleRate,
                                      int* lowHz, int* highHz);

signals:
    void configurationChanged();
    void autoStartChanged();
    void listenAddressesChanged();   // also covers the USB device list
    void stateChanged();
    void endpointsChanged();
    void statsChanged();      // coalesced for display
    void lastErrorChanged();
    void extensionsChanged();   // the USB device's link extensions; audioSpectrumWanted() may move
    void firmwareUpdateProgress(qint64 bytesStored, qint64 imageBytes);
    void firmwareUpdateStep(const QString& step);
    void firmwareUpdateFinished(bool ok, const QString& message);
    void firmwareVersionChanged();

private:
    void tryAutoStart();
    void saveSetup();
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
    UsbCapsPortOpener m_firmwareOpener;
    void probeFirmwareVersion();
    QPointer<Ctr2HidPort> m_probe;
    QString m_probedPath;      // the device m_probedVersion was read from
    QString m_probedVersion;
    quint32 m_probedOffers{0};  // the link extensions it offered
    Ctr2FirmwareUpdater* m_firmware{nullptr};
    bool m_restartAfterFirmware{false};
    Ctr2RelaySettings m_settings;
    QTimer* m_autoStartTimer{nullptr};
    QTimer* m_deviceRescan{nullptr};
    bool m_autoStartHeld{false};   // the operator stopped it this connection
};

} // namespace AetherSDR
