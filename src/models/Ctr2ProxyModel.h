#pragma once

#include "core/Ctr2FirmwareUpdater.h"
#include "core/Ctr2RelaySettings.h"
#include "core/Ctr2HidPort.h"
#include "core/TcpByteProxy.h"

#include <QDeadlineTimer>
#include <QFutureWatcher>
#include <QHash>
#include <QHostAddress>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <vector>

class QTimer;

namespace AetherSDR {

class Ctr2UsbRelay;

// Operator-facing state for the CTR2 relay. Wi-Fi mode runs TcpByteProxy
// (the CTR2 connects to this PC over TCP); USB mode runs a Ctr2UsbRelay per
// started HID device, so several controllers (a CTR2 and an AetherKnob, say)
// relay at once, each its own client of the radio. Both forward to the radio
// AetherSDR is connected to, which the owner pushes in through
// setAetherRadio(); that destination is captured when the first relay starts
// and never retargeted while any runs. The model never reads RadioModel or a
// backend itself: each controller is its own radio client. USB mode is the
// default when the build has hidapi, with the first recognized CTR2
// preselected. In USB mode the selected device (usbDevicePath()) is the focus:
// the calls without a device path (start(), state(), stats(), the firmware
// calls) act on it.
// Design: docs/ctr2-tcp-proxy-design.md, docs/ctr2-usb-relay-design.md.
class Ctr2ProxyModel : public QObject {
    Q_OBJECT

public:
    static constexpr quint16 kDefaultPort = 4992;

    enum class Transport { Wifi, Usb };

    // USB HID access. Defaults to hidapi (absent when the build has none);
    // tests inject a device list and a port opener. The periodic rescan calls
    // a source marked thread-safe on the thread pool, off the GUI thread; any
    // other source is called on this object's thread.
    using UsbDeviceSource = std::function<QList<Ctr2HidPort::DeviceInfo>()>;
    using UsbPortOpener =
        std::function<Ctr2HidPort*(const Ctr2HidPort::DeviceInfo&, QString* error)>;
    // Opens a device accepting only the given link extensions: Firmware for
    // an update, none to read the firmware version. Empty keeps updates and
    // the idle version readout unavailable.
    using UsbCapsPortOpener = std::function<Ctr2HidPort*(const Ctr2HidPort::DeviceInfo&,
                                                         quint32 hostCapabilities, QString* error)>;
    struct UsbBackend {
        UsbDeviceSource devices;
        UsbPortOpener opener;
        UsbCapsPortOpener firmwareOpener;
        bool threadSafeSource{false};
    };
    // The build's hidapi; empty when it has none.
    static UsbBackend hidapiBackend();
    // Tests: the backend the parameterless constructor uses from now on, in
    // place of hidapi, so a test that forgets to inject one never reaches a
    // real device.
    static void setDefaultUsbBackendForTesting(UsbBackend backend);

    // With hidapiBackend() (or the one set for tests).
    explicit Ctr2ProxyModel(QObject* parent = nullptr);
    // With this backend from the start: the first scan and the first idle
    // probe use it, never hidapi.
    explicit Ctr2ProxyModel(UsbBackend backend, QObject* parent = nullptr);
    ~Ctr2ProxyModel() override;

    void setUsbBackend(UsbDeviceSource devices, UsbPortOpener opener,
                       UsbCapsPortOpener firmwareOpener = {},
                       bool threadSafeSource = false);
    bool usbAvailable() const { return static_cast<bool>(m_usbOpener); }
    // The local IPv4 addresses offered for Wi-Fi. Defaults to the machine's
    // interfaces; tests inject a list. Called on the thread pool by the
    // periodic rescan, so it must be thread-safe.
    using ListenAddressSource = std::function<QStringList()>;
    void setListenAddressSource(ListenAddressSource source);

    // Rescans local IPv4 addresses and CTR2-class USB HID devices, now, on
    // this thread. The periodic rescan does the same work off the GUI thread.
    void refreshDevices();
    // Forgets what the idle probe last read from the selected device and
    // reads it again (its version and FIRMWARE offer, usbFirmwareVersion() and
    // usbFirmwareCommands()). A probe taken while the firmware was still
    // starting can read nothing; a firmware switch keeps the same path.
    void reprobeFirmware();

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
    // The focus. Changing it never stops or starts a relay.
    QString usbDevicePath() const { return m_usbDevicePath; }
    bool setUsbDevicePath(const QString& path);
    // Linux: the selected CTR2's device node exists but this user may not
    // open it, i.e. the udev access rule is not installed yet. Always false
    // on other platforms, in Wi-Fi mode, or with no device selected.
    bool usbDeviceNeedsAccessRule() const;
    bool usbDeviceNeedsAccessRule(const QString& path) const;

    // The radio AetherSDR is connected to. A null address means the relay
    // has no usable destination, and unavailableReason says why. A running
    // relay stops as soon as this is no longer the radio it started with:
    // AetherSDR's TX indicator is the operator's only view of that radio.
    void setAetherRadio(const QHostAddress& address, quint16 port, const QString& label,
                        const QString& unavailableReason);
    QString aetherRadioLabel() const { return m_aetherRadioLabel; }

    // Empty when Start may be pressed; otherwise the reason it may not. With
    // a path: for that USB device.
    QString configurationProblem() const;
    QString configurationProblem(const QString& usbPath) const;
    // Any relay is up: the Wi-Fi proxy, or a USB relay for any device.
    bool isRunning() const;
    // The relay for this USB device is up.
    bool isRunning(const QString& usbPath) const;
    // The USB devices being relayed now.
    QStringList runningUsbDevices() const;

    // Wi-Fi: starts the proxy. USB: starts the focus device's relay.
    bool start();
    bool start(const QString& usbPath);
    // Stops every relay.
    void stop();
    void stop(const QString& usbPath);
    // Stop pressed by the operator: also holds off the automatic start until
    // AetherSDR next connects to a radio. Without a path, for every relay.
    // The hold is on the device, by serial number and USB IDs, so it survives
    // an unplug and replug at the same path or another; a device with no
    // serial number is held by its path.
    void stopByOperator();
    void stopByOperator(const QString& usbPath);
    // The operator stopped this listed device by hand this connection.
    bool usbDeviceHeldByOperator(const QString& usbPath) const;

    // A device remembered for the automatic start (started before, and with a
    // serial number to tell it by).
    bool usbDeviceRemembered(const QString& usbPath) const;
    // Forget: the device leaves the remembered list, and its Stop hold goes
    // with it, so the automatic start no longer starts it. Only for a listed,
    // remembered device that is not relaying, in USB mode, outside a firmware
    // update; false otherwise. Forgetting the last one leaves the automatic
    // start with nothing to start (Ctr2RelaySettings::usbForgotten) until a
    // device is started by hand. Saved at once.
    bool canForgetUsbDevice(const QString& usbPath) const;
    bool forgetUsbDevice(const QString& usbPath);
    // The same for a remembered device by its identity, plugged in or not:
    // an unplugged one can be forgotten too.
    bool canForgetUsbDevice(const Ctr2RelaySettings::UsbDevice& device) const;
    bool forgetUsbDevice(const Ctr2RelaySettings::UsbDevice& device);
    // The remembered devices not plugged in now (none listed with their
    // serial number and IDs), in the order first started.
    QList<Ctr2RelaySettings::UsbDevice> absentUsbDevices() const;
    // A remembered device's name: as listed when last started, else (saved
    // before names were kept) its CTR2 model by USB IDs, else the IDs, with
    // its serial number.
    static QString rememberedName(const Ctr2RelaySettings::UsbDevice& device);

    // Starts the relay by itself whenever AetherSDR is connected to a radio,
    // with the setup it last started with (Ctr2RelaySettings, saved), and
    // keeps trying while a device is missing. In USB mode it starts every
    // device it has relayed before that is plugged in (the focus only when it
    // remembers none and none was forgotten, so selecting a row starts
    // nothing); only a device whose
    // firmware relays over USB
    // (usbLink()) is started, and a device that failed without ever answering
    // is retried only every kNoAnswerRetryMs, or at once when the device
    // list, the selected device, the mode or the radio changes.
    bool autoStart() const { return m_settings.autoStart; }
    void setAutoStart(bool on);
    static constexpr int kAutoStartRetryMs = 5000;
    static constexpr int kNoAnswerRetryMs = 60000;
    void setNoAnswerRetryMs(int ms);   // tests
    // Tests: the radio socket every USB relay started from now on uses.
    void setUsbRadioSocketFactory(TcpByteProxy::SocketFactory factory);
    // USB devices are rescanned this often (except while the Wi-Fi proxy or a
    // firmware update runs), so a controller plugged in, or one that switched
    // firmware and so changed its USB identity, appears without pressing
    // Rescan.
    static constexpr int kDeviceRescanMs = 2000;
    // After a restart request, the longest auto-start waits for the device to
    // leave the bus and come back before trying anyway.
    static constexpr int kRestartHoldMs = 20000;
    // Shown while a USB relay that has run this long has never had an answer
    // from the CTR2, as firmware without USB mode never answers; empty
    // otherwise.
    QString noAnswerHint() const;
    QString noAnswerHint(const QString& usbPath) const;
    static constexpr int kNoAnswerHintMs = 10000;
    void setNoAnswerHintMs(int ms);   // tests

    // Wi-Fi: the proxy. USB: the focus device's relay; with a path, that one.
    TcpByteProxy::State state() const;
    TcpByteProxy::State state(const QString& usbPath) const;
    QString stateText() const;
    QString stateText(const QString& usbPath) const;
    QString lastError() const;
    QString lastError(const QString& usbPath) const;
    QString listenerEndpoint() const;
    QString peerEndpoint() const;
    QString radioEndpoint() const;
    TcpByteProxy::Stats stats() const;

    // True while a USB relay is up with a device that negotiated the
    // AudioSpectrum link extension (AetherKnob; never a stock CTR2).
    bool audioSpectrumWanted() const;
    // And such a device also negotiated AudioSpectrumLinear: windows not
    // starting at 0. While one does, the feeder sends CW as linear frames
    // only, so a device with AudioSpectrum but not AudioSpectrumLinear gets
    // nothing in CW; no device today has one without the other.
    bool audioSpectrumLinearWanted() const;
    // True while a USB relay is up with a device that negotiated CwText.
    bool cwTextWanted() const;
    // Decoded CW text for every device that wants it, with the decoder's cost
    // (lower is better). False if none wants it now or nothing printable is left.
    bool sendCwText(const QString& text, float cost);

    // Firmware version of the selected USB device: from the relay while it
    // runs, otherwise the last one read from it. Empty for a device that does
    // not report one (any stock CTR2), or one never read. With a path: that
    // device's. What a device reports is kept per device (by serial number,
    // USB IDs and product name; by USB IDs, product and path for one with no
    // serial), in memory only, whether the idle probe read it (when the device
    // is the focus) or its relay did, so an idle row keeps its version after
    // the focus moves on or its relay stops. A firmware update or restart
    // forgets the device's; one with no serial is forgotten once unplugged.
    QString usbFirmwareVersion() const;
    QString usbFirmwareVersion(const QString& usbPath) const;
    bool firmwareUpdateAvailable() const { return static_cast<bool>(m_firmwareOpener); }
    // Whether the selected USB device takes FIRMWARE commands (restart into
    // its other firmware or download mode, restartUsbDevice()), as read with
    // its version. A maker's stock firmware may offer them without taking
    // updates over the link.
    bool usbFirmwareCommands() const;
    bool firmwareUpdateRunning() const;
    // Updates the selected USB device with image (Ctr2FirmwareUpdater::readImage).
    // Stops its relay first and, if it was running, starts that device's
    // relay again afterwards, found by serial number and USB IDs (by path
    // without a serial), wherever the focus has moved meanwhile. False, with
    // *problem, when it cannot start.
    bool startFirmwareUpdate(const Ctr2FirmwareUpdater::Image& image, QString* problem);
    // Before the device starts checking the image; the device keeps its
    // current firmware. Later the update runs to its end.
    void cancelFirmwareUpdate();
    // Restarts the selected USB device into its other firmware or its ROM
    // download mode (Ctr2FirmwareUpdater::restartDevice); the relay stops
    // for it and does not come back, as the device leaves.
    bool restartUsbDevice(Ctr2FirmwareUpdater::Restart target, QString* problem);
    Ctr2FirmwareUpdater* firmwareUpdater() const { return m_firmware; }
    // One AudioSpectrum frame, to every device that wants it: bars in dBFS,
    // log-spaced from lowHz to spanHz (a non-finite bar reads as silence).
    // False if none wants it now or every link is busy.
    // With linear, the bars split lowHz..spanHz evenly (AudioSpectrumLinear;
    // only to the devices that negotiated it).
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
    void listenAddressesChanged();   // also covers the USB device list and absentUsbDevices()
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
    struct DeviceScan {
        QStringList listenChoices;
        QList<Ctr2HidPort::DeviceInfo> usb;
    };
    static QStringList scanListenAddresses();
    void applyDeviceScan(DeviceScan scan);
    void rescanInBackground();
    void tryAutoStart();
    void saveSetup(const QString& usbPath = {});

    // One USB device's relay and what the model knows of its run. Created
    // when the device is first started, kept while it is listed so its stop
    // reason still shows, dropped once it is gone and stopped.
    struct UsbLink {
        Ctr2UsbRelay* relay{nullptr};
        QTimer* noAnswerTimer{nullptr};
        bool answered{false};   // the device has answered since its relay started
        bool noAnswer{false};   // and has not, kNoAnswerHintMs after starting
        QString startError;     // opening the device failed
        QString stopReason;     // why its relay stopped on its own
        // Failed before the device ever answered (firmware with no USB mode,
        // or a fault on the way up): no automatic retry before this expires,
        // rather than opening the device on this thread every
        // kAutoStartRetryMs. Expired (the default) means no hold.
        QDeadlineTimer noAnswerHold;
    };
    UsbLink* usbLink(const QString& path) const;
    UsbLink* ensureUsbLink(const QString& path);
    void onUsbStateChanged(const QString& path);
    void stopUsbLink(UsbLink* link);
    const Ctr2HidPort::DeviceInfo* listedUsbDevice(const QString& path) const;
    bool remembered(const Ctr2HidPort::DeviceInfo& device) const;
    // The operator's Stop, kept off the link (which goes when the device is
    // unplugged) and by identity (its path can change on a replug).
    void holdByOperator(const QString& path);
    void releaseOperatorHold(const QString& path);
    bool heldByOperator(const Ctr2HidPort::DeviceInfo& device) const;
    // The listed device that is this one: same serial and IDs, or, with no
    // serial, the same path and IDs.
    const Ctr2HidPort::DeviceInfo* listedSameDevice(const Ctr2HidPort::DeviceInfo& device) const;
    static bool parsePort(const QString& text, quint16* port);
    static bool parseIpv4(const QString& text, QHostAddress* address);
    bool radioProblem(QString* problem) const;
    bool buildConfig(TcpByteProxy::Config* config, QString* problem) const;
    const Ctr2HidPort::DeviceInfo* selectedUsbDevice() const;
    bool usbActive() const { return m_activeTransport == Transport::Usb; }
    bool wifiRunning() const;

    TcpByteProxy* m_proxy{nullptr};
    // Keyed by HID path. unique_ptr: a link's address stays put while the
    // map changes, so callbacks may hold it.
    std::map<QString, std::unique_ptr<UsbLink>> m_usbLinks;
    TcpByteProxy::SocketFactory m_usbRadioSocketFactory;   // tests
    QTimer* m_statsTimer{nullptr};
    Transport m_transport{Transport::Wifi};
    Transport m_activeTransport{Transport::Wifi};
    QStringList m_listenChoices;
    QString m_listenAddress;
    QString m_listenPort{QString::number(kDefaultPort)};
    QList<Ctr2HidPort::DeviceInfo> m_usbChoices;
    QString m_usbDevicePath;
    QHostAddress m_aetherRadioAddress;
    quint16 m_aetherRadioPort{kDefaultPort};
    QString m_aetherRadioLabel;
    QString m_aetherRadioReason;
    QHostAddress m_runningRadioAddress;  // captured when the first relay starts
    quint16 m_runningRadioPort{0};
    QString m_runningRadioLabel;
    QString m_stopReason;                // why the Wi-Fi proxy stopped on its own
    ListenAddressSource m_listenAddresses;
    UsbDeviceSource m_usbDevices;
    bool m_usbDevicesThreadSafe{false};
    UsbPortOpener m_usbOpener;
    UsbCapsPortOpener m_firmwareOpener;
    void probeFirmwareVersion();
    QPointer<Ctr2HidPort> m_probe;
    QString m_probedPath;      // the focus the probe last read; cleared to read it again
    // What each device's firmware last reported, by firmwareKey(), read by
    // the idle probe or from its relay's port: the version and the link
    // extensions it offered. In memory only.
    struct FirmwareRead {
        QString version;
        quint32 offers{0};
    };
    QHash<QString, FirmwareRead> m_firmwareReads;
    static QString firmwareKey(const Ctr2HidPort::DeviceInfo& device);
    void rememberFirmware(const Ctr2HidPort::DeviceInfo& device, const QString& version,
                          quint32 offers);
    const FirmwareRead* firmwareRead(const QString& path) const;
    Ctr2FirmwareUpdater* m_firmware{nullptr};
    // The device being updated, while its relay is to come back afterwards.
    std::optional<Ctr2HidPort::DeviceInfo> m_restartAfterFirmware;
    Ctr2RelaySettings m_settings;
    QTimer* m_autoStartTimer{nullptr};
    QTimer* m_deviceRescan{nullptr};
    QFutureWatcher<DeviceScan>* m_rescanWatcher{nullptr};
    // Bumped by every synchronous scan, backend change and relay start, so a
    // background scan that started before one of them is dropped.
    quint64 m_scanGeneration{0};
    bool m_autoStartHeld{false};   // the operator stopped the Wi-Fi proxy this connection
    // USB devices the operator stopped this connection (holdByOperator()):
    // by serial and IDs, or by path for a device with no serial.
    QList<Ctr2RelaySettings::UsbDevice> m_heldDevices;
    QStringList m_heldPaths;
    // A restart request (restartUsbDevice) answered: the device leaves the bus
    // a moment later (Lynovation's firmware after about two seconds), and is
    // still listed until then. Auto-start leaves m_restartedPath alone until a
    // scan has seen it gone and back, or m_restartedHold expires.
    QString m_restartedPath;
    Ctr2HidPort::DeviceInfo m_restartedDevice;   // as listed when restarted
    bool m_restartedSeenGone{false};
    QDeadlineTimer m_restartedHold;
    // The restarted device is this one (the focus without a path) and still held.
    bool restartHeld();
    bool restartHeld(const QString& path);
    int m_noAnswerRetryMs{kNoAnswerRetryMs};
    int m_noAnswerHintMs{kNoAnswerHintMs};
};

} // namespace AetherSDR
