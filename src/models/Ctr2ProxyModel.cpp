#include "Ctr2ProxyModel.h"

#include "core/Ctr2HidFraming.h"
#include "core/Ctr2UsbRelay.h"
#include "core/LogManager.h"
#include "core/UnicastEndpoint.h"
#ifdef HAVE_HIDAPI
#include "core/Ctr2HidapiPort.h"
#endif

#include <QHostAddress>
#include <QNetworkInterface>
#include <QRegularExpression>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>

#include <utility>

#ifdef Q_OS_LINUX
#include <cerrno>
#include <unistd.h>
#endif

namespace AetherSDR {

namespace {

constexpr int kStatsRefreshMs = 250;

} // namespace

Ctr2ProxyModel::Ctr2ProxyModel(QObject* parent)
    : QObject(parent)
    , m_proxy(new TcpByteProxy(this))
    , m_aetherRadioReason(tr("Connect AetherSDR to a radio first"))
    , m_listenAddresses(&Ctr2ProxyModel::scanListenAddresses)
{
    m_statsTimer = new QTimer(this);
    m_statsTimer->setSingleShot(true);
    m_statsTimer->setInterval(kStatsRefreshMs);
    connect(m_statsTimer, &QTimer::timeout, this, &Ctr2ProxyModel::statsChanged);
    const auto coalesceStats = [this] {
        if (!m_statsTimer->isActive()) {
            m_statsTimer->start();
        }
    };
    const auto stateMoved = [this] {
        emit stateChanged();
        emit statsChanged();
    };
    connect(m_proxy, &TcpByteProxy::statsChanged, this, coalesceStats);
    connect(m_proxy, &TcpByteProxy::stateChanged, this, stateMoved);
    connect(m_proxy, &TcpByteProxy::endpointsChanged, this, &Ctr2ProxyModel::endpointsChanged);
    connect(m_proxy, &TcpByteProxy::lastErrorChanged, this, &Ctr2ProxyModel::lastErrorChanged);
    // A USB relay per device is made, and connected, when the device is
    // first started (ensureUsbLink()).
    // Through the members, so a backend set later (tests) is the one used.
    m_firmware = new Ctr2FirmwareUpdater(
        [this] { return m_usbDevices ? m_usbDevices() : QList<Ctr2HidPort::DeviceInfo>(); },
        [this](const Ctr2HidPort::DeviceInfo& device, QString* error) -> Ctr2HidPort* {
            return m_firmwareOpener
                ? m_firmwareOpener(device,
                                   ctr2hid::capabilityBit(ctr2hid::MessageType::Firmware)
                                       | ctr2hid::kWideReportsBit,
                                   error)
                : nullptr;
        },
        this);
    connect(m_firmware, &Ctr2FirmwareUpdater::progress, this, &Ctr2ProxyModel::firmwareUpdateProgress);
    connect(m_firmware, &Ctr2FirmwareUpdater::stepChanged, this, &Ctr2ProxyModel::firmwareUpdateStep);
    connect(m_firmware, &Ctr2FirmwareUpdater::finished, this,
            [this](bool ok, const QString& message, const QString&) {
        // A refused restart: the device stays. One written but never answered
        // may still be on its way off the bus, so it stays held.
        if (!ok && !m_firmware->restartUnanswered())
            m_restartedPath.clear();
        refreshDevices();   // the device re-enumerated, perhaps at a new path
        m_probedPath.clear();
        if (m_restartAfterFirmware) {
            m_restartAfterFirmware = false;
            start();
        }
        probeFirmwareVersion();
        emit configurationChanged();
        emit firmwareUpdateFinished(ok, message);
    });
#ifdef HAVE_HIDAPI
    m_usbDevices = [] { return Ctr2HidapiPort::enumerate(); };
    // HidDiscovery serializes HIDAPI discovery across threads (#6604).
    m_usbDevicesThreadSafe = true;
    m_usbOpener = [](const Ctr2HidPort::DeviceInfo& device, QString* error) -> Ctr2HidPort* {
        return Ctr2HidapiPort::open(device, error);
    };
    m_firmwareOpener = [](const Ctr2HidPort::DeviceInfo& device, quint32 hostCapabilities,
                          QString* error) -> Ctr2HidPort* {
        return Ctr2HidapiPort::open(device, error, nullptr, hostCapabilities);
    };
    // USB is the default whenever this build can open a CTR2.
    m_transport = Transport::Usb;
    m_activeTransport = Transport::Usb;
#endif
    // The setup the relay last started with, if any; refreshDevices()
    // prefers its USB device.
    m_settings = Ctr2RelaySettings::load();
    if (m_settings.hasSetup) {
        if (!m_settings.usb || m_usbOpener) {
            m_transport = m_settings.usb ? Transport::Usb : Transport::Wifi;
            m_activeTransport = m_transport;
        }
        if (!m_settings.listenAddress.isEmpty())
            m_listenAddress = m_settings.listenAddress;
        if (!m_settings.listenPort.isEmpty())
            m_listenPort = m_settings.listenPort;
    }
    m_autoStartTimer = new QTimer(this);
    m_autoStartTimer->setInterval(kAutoStartRetryMs);
    connect(m_autoStartTimer, &QTimer::timeout, this, &Ctr2ProxyModel::tryAutoStart);
    if (m_settings.autoStart)
        m_autoStartTimer->start();
    m_rescanWatcher = new QFutureWatcher<DeviceScan>(this);
    m_deviceRescan = new QTimer(this);
    m_deviceRescan->setInterval(kDeviceRescanMs);
    // Network interfaces too, so a build without hidapi still notices a
    // listen address that appears after startup. USB relays running do not
    // pause it: another controller plugged in meanwhile has to appear.
    connect(m_deviceRescan, &QTimer::timeout, this, [this] {
        if (!wifiRunning() && !m_firmware->isRunning())
            rescanInBackground();
    });
    m_deviceRescan->start();
    refreshDevices();
}

Ctr2ProxyModel::~Ctr2ProxyModel()
{
    stop();
    if (m_probe) {
        disconnect(m_probe, nullptr, this, nullptr);
        m_probe->shutdown({});
    }
}

void Ctr2ProxyModel::setUsbBackend(UsbDeviceSource devices, UsbPortOpener opener,
                                   UsbCapsPortOpener firmwareOpener, bool threadSafeSource)
{
    m_usbDevices = std::move(devices);
    m_usbDevicesThreadSafe = threadSafeSource;
    m_usbOpener = std::move(opener);
    m_firmwareOpener = std::move(firmwareOpener);
    m_usbChoices.clear();
    refreshDevices();
    emit configurationChanged();
}

QStringList Ctr2ProxyModel::scanListenAddresses()
{
    QStringList choices;
    for (const QNetworkInterface& iface : QNetworkInterface::allInterfaces()) {
        const auto flags = iface.flags();
        if (!flags.testFlag(QNetworkInterface::IsUp)
            || !flags.testFlag(QNetworkInterface::IsRunning)
            || flags.testFlag(QNetworkInterface::IsLoopBack)) {
            continue;
        }
        for (const QNetworkAddressEntry& entry : iface.addressEntries()) {
            const QHostAddress ip = entry.ip();
            if (ip.protocol() == QAbstractSocket::IPv4Protocol && !ip.isLoopback()) {
                choices.append(ip.toString());
            }
        }
    }
    choices.removeDuplicates();
    return choices;
}

void Ctr2ProxyModel::setListenAddressSource(ListenAddressSource source)
{
    m_listenAddresses = std::move(source);
    refreshDevices();
}

void Ctr2ProxyModel::refreshDevices()
{
    ++m_scanGeneration;   // a background scan already under way is stale
    applyDeviceScan({m_listenAddresses ? m_listenAddresses() : QStringList(),
                     m_usbDevices ? m_usbDevices() : QList<Ctr2HidPort::DeviceInfo>()});
}

// Enumerating interfaces and HID devices can block for a long time, on
// Windows and inside HidDiscovery's process-wide lock alike, so the periodic
// rescan does it on the thread pool and only applies the result here.
void Ctr2ProxyModel::rescanInBackground()
{
    if (m_usbDevices && !m_usbDevicesThreadSafe) {
        refreshDevices();
        return;
    }
    if (m_rescanWatcher->isRunning()) {
        return;   // the previous scan is still out; this tick is not needed
    }
    const quint64 generation = m_scanGeneration;
    disconnect(m_rescanWatcher, &QFutureWatcher<DeviceScan>::finished, this, nullptr);
    connect(m_rescanWatcher, &QFutureWatcher<DeviceScan>::finished, this, [this, generation] {
        // Dropped if a synchronous scan, a backend change or a relay start
        // came first, or the Wi-Fi proxy or an update is running now.
        if (generation != m_scanGeneration || wifiRunning() || m_firmware->isRunning()) {
            return;
        }
        applyDeviceScan(m_rescanWatcher->result());
    });
    // Copies of the sources, never `this`: the scan may outlive the model.
    m_rescanWatcher->setFuture(QtConcurrent::run(
        [listen = m_listenAddresses, usb = m_usbDevices] {
            return DeviceScan{listen ? listen() : QStringList(),
                              usb ? usb() : QList<Ctr2HidPort::DeviceInfo>()};
        }));
}

void Ctr2ProxyModel::applyDeviceScan(DeviceScan scan)
{
    QStringList& choices = scan.listenChoices;
    const QList<Ctr2HidPort::DeviceInfo>& usb = scan.usb;
    // By identity, not just path: on Linux a CTR2-Max that switches firmware
    // comes back at the same hidraw path with another product and serial.
    const auto same = [](const Ctr2HidPort::DeviceInfo& a, const Ctr2HidPort::DeviceInfo& b) {
        return a.path == b.path && a.vendorId == b.vendorId && a.productId == b.productId
            && a.manufacturer == b.manufacturer && a.product == b.product && a.serial == b.serial;
    };
    bool usbChanged = usb.size() != m_usbChoices.size();
    for (int i = 0; !usbChanged && i < usb.size(); ++i) {
        usbChanged = !same(usb[i], m_usbChoices[i]);
    }
    if (!m_restartedPath.isEmpty()) {
        const auto at = std::find_if(usb.begin(), usb.end(), [this](const auto& d) {
            return d.path == m_restartedPath;
        });
        if (at == usb.end())
            m_restartedSeenGone = true;
        // Back: seen gone and listed again, or another firmware at the same
        // path (a Linux switch that re-enumerates between two scans).
        else if (m_restartedSeenGone || !same(*at, m_restartedDevice))
            m_restartedPath.clear();
    }
    if (choices == m_listenChoices && !usbChanged) {
        return;
    }
    m_listenChoices = std::move(choices);
    m_usbChoices = usb;
    if (usbChanged) {
        for (auto it = m_usbLinks.begin(); it != m_usbLinks.end();) {
            UsbLink* link = it->second.get();
            link->noAnswerHold = QDeadlineTimer();   // replugged, or switched firmware: try it again
            // A device gone for good, its relay stopped: nothing left to show.
            if (!listedUsbDevice(it->first) && !isRunning(it->first)) {
                link->relay->deleteLater();
                link->noAnswerTimer->deleteLater();
                it = m_usbLinks.erase(it);
            } else {
                ++it;
            }
        }
    }
    // Default to the first recognized CTR2 while nothing is selected or the
    // selected device has gone; an existing choice stands.
    if (!selectedUsbDevice()) {
        m_usbDevicePath.clear();
        // A device the relay has started before, when one is here.
        for (const Ctr2HidPort::DeviceInfo& d : std::as_const(m_usbChoices)) {
            if (remembered(d)) {
                m_usbDevicePath = d.path;
                break;
            }
        }
        for (const Ctr2HidPort::DeviceInfo& d : std::as_const(m_usbChoices)) {
            if (!m_usbDevicePath.isEmpty())
                break;
            if (!d.ctr2Model().isEmpty())
                m_usbDevicePath = d.path;
        }
    }
    emit listenAddressesChanged();
    emit configurationChanged();
    m_probedPath.clear();   // a device may have been swapped at the same path
    // From the event loop: a backend set right after construction (tests)
    // is the one that opens the device.
    QTimer::singleShot(0, this, &Ctr2ProxyModel::probeFirmwareVersion);
}

// Opens the selected device just long enough to read its firmware version
// (Feature report 0x03, read when a port opens), accepting no link
// extensions so the device's state is untouched. Never while the relay or an
// update holds the device.
bool Ctr2ProxyModel::restartHeld()
{
    return restartHeld(m_usbDevicePath);
}

bool Ctr2ProxyModel::restartHeld(const QString& path)
{
    if (m_restartedPath.isEmpty())
        return false;
    if (m_restartedHold.hasExpired()) {
        m_restartedPath.clear();
        return false;
    }
    return m_transport == Transport::Usb && path == m_restartedPath;
}

void Ctr2ProxyModel::reprobeFirmware()
{
    m_probedPath.clear();
    probeFirmwareVersion();
}

void Ctr2ProxyModel::probeFirmwareVersion()
{
    if (!m_firmwareOpener || m_probe || isRunning(m_usbDevicePath) || m_firmware->isRunning()
        || m_transport != Transport::Usb || restartHeld()) {
        return;
    }
    const Ctr2HidPort::DeviceInfo* device = selectedUsbDevice();
    if (!device || device->path == m_probedPath) {
        return;
    }
    QString error;
    Ctr2HidPort* port = m_firmwareOpener(*device, 0, &error);
    if (!port) {
        return;   // no access yet (Linux rule) or gone; Start reports why
    }
    m_probe = port;
    const QString path = device->path;
    const auto finish = [this, port, path](const QString& version, quint32 offers) {
        disconnect(port, nullptr, this, nullptr);
        port->shutdown({});
        m_probe.clear();
        m_probedPath = path;
        m_probedOffers = offers;
        if (m_probedVersion != version) {
            m_probedVersion = version;
        }
        emit firmwareVersionChanged();
        // The selection may have changed while this probe ran; probing again
        // does nothing when it is still this device.
        QTimer::singleShot(0, this, &Ctr2ProxyModel::probeFirmwareVersion);
    };
    connect(port, &Ctr2HidPort::extensionsNegotiated, this, [port, finish](quint32) {
        const std::optional<ctr2hid::deviceinfo::Info>& info = port->firmwareInfo();
        finish(info ? info->version() : QString(), port->offeredExtensions());
    });
    connect(port, &Ctr2HidPort::failed, this, [finish](const QString&) { finish(QString(), 0); });
}

bool Ctr2ProxyModel::setTransport(Transport transport)
{
    if (isRunning()) {
        return false;
    }
    if (m_transport != transport) {
        m_transport = transport;
        m_activeTransport = transport;
        for (auto& [path, link] : m_usbLinks) {
            link->startError.clear();
            link->stopReason.clear();
            link->noAnswerHold = QDeadlineTimer();
        }
        m_stopReason.clear();
        emit configurationChanged();
        emit stateChanged();
        emit endpointsChanged();
        emit lastErrorChanged();
        emit statsChanged();
        probeFirmwareVersion();
    }
    return true;
}

bool Ctr2ProxyModel::setListenAddress(const QString& address)
{
    if (isRunning()) {
        return false;
    }
    if (m_listenAddress != address) {
        m_listenAddress = address;
        emit configurationChanged();
    }
    return true;
}

bool Ctr2ProxyModel::setListenPortText(const QString& port)
{
    if (isRunning()) {
        return false;
    }
    if (m_listenPort != port) {
        m_listenPort = port;
        emit configurationChanged();
    }
    return true;
}

bool Ctr2ProxyModel::setUsbDevicePath(const QString& path)
{
    if (m_usbDevicePath != path) {
        m_usbDevicePath = path;
        if (UsbLink* link = usbLink(path))
            link->noAnswerHold = QDeadlineTimer();   // picked again: try it again
        emit configurationChanged();
        emit stateChanged();
        emit endpointsChanged();
        emit lastErrorChanged();
        emit statsChanged();
        emit firmwareVersionChanged();
        probeFirmwareVersion();
    }
    return true;
}

void Ctr2ProxyModel::setAetherRadio(const QHostAddress& address, quint16 port,
                                    const QString& label, const QString& unavailableReason)
{
    const QHostAddress radio = port != 0 ? usableUnicastAddress(address) : QHostAddress();
    const QString reason = radio.isNull() && unavailableReason.isEmpty()
        ? tr("Connect AetherSDR to a radio first") : unavailableReason;
    if (isRunning() && (radio != m_runningRadioAddress || port != m_runningRadioPort)) {
        const QString was = m_runningRadioLabel.isEmpty() ? m_runningRadioAddress.toString()
                                                          : m_runningRadioLabel;
        const QString why = radio.isNull()
            ? tr("Stopped: AetherSDR is no longer connected to %1").arg(was)
            : tr("Stopped: AetherSDR switched from %1 to another radio").arg(was);
        qCInfo(lcDevices) << "CTR2 relay:" << why;
        const bool wifi = wifiRunning();
        const QStringList usb = runningUsbDevices();
        stop();
        if (wifi)
            m_stopReason = why;
        for (const QString& path : usb) {
            if (UsbLink* link = usbLink(path))
                link->stopReason = why;
        }
        emit lastErrorChanged();
    }
    if (radio == m_aetherRadioAddress && port == m_aetherRadioPort
        && label == m_aetherRadioLabel && reason == m_aetherRadioReason) {
        return;
    }
    const bool newRadio = !radio.isNull()
        && (radio != m_aetherRadioAddress || port != m_aetherRadioPort);
    m_aetherRadioAddress = radio;
    m_aetherRadioPort = port;
    m_aetherRadioLabel = label;
    m_aetherRadioReason = radio.isNull() ? reason : QString();
    emit configurationChanged();
    if (newRadio) {
        m_autoStartHeld = false;   // a new connection: start again
        for (auto& [path, link] : m_usbLinks) {
            link->heldByOperator = false;
            link->noAnswerHold = QDeadlineTimer();
        }
        QTimer::singleShot(0, this, &Ctr2ProxyModel::tryAutoStart);
    }
}

void Ctr2ProxyModel::setAutoStart(bool on)
{
    if (m_settings.autoStart == on)
        return;
    m_settings.autoStart = on;
    m_settings.save();
    if (on) {
        for (auto& [path, link] : m_usbLinks)
            link->noAnswerHold = QDeadlineTimer();
        m_autoStartTimer->start();
        QTimer::singleShot(0, this, &Ctr2ProxyModel::tryAutoStart);
    } else {
        m_autoStartTimer->stop();
    }
    emit autoStartChanged();
}

void Ctr2ProxyModel::tryAutoStart()
{
    if (!m_settings.autoStart || m_firmware->isRunning() || m_aetherRadioAddress.isNull())
        return;
    if (m_transport == Transport::Wifi) {
        if (m_autoStartHeld || isRunning() || !configurationProblem().isEmpty())
            return;
        qCInfo(lcDevices) << "CTR2 relay: starting automatically";
        start();
        return;
    }
    // The device list is the one the periodic rescan keeps current, so a
    // controller plugged in since shows up within kDeviceRescanMs. Every
    // device relayed before is started, and the focus.
    QStringList paths;
    for (const Ctr2HidPort::DeviceInfo& d : std::as_const(m_usbChoices)) {
        if (remembered(d) || d.path == m_usbDevicePath)
            paths.append(d.path);
    }
    for (const QString& path : std::as_const(paths)) {
        const Ctr2HidPort::DeviceInfo* device = listedUsbDevice(path);
        if (!device || isRunning(path))
            continue;
        if (const UsbLink* link = usbLink(path);
            link && (link->heldByOperator || !link->noAnswerHold.hasExpired())) {
            continue;
        }
        // Restarted on request and not back yet: opening it now would catch
        // it on its way off the bus.
        if (restartHeld(path) || !configurationProblem(path).isEmpty())
            continue;
        if (!device->usbLink())
            continue;   // firmware with no USB mode
        if (usbDeviceNeedsAccessRule(path))
            continue;   // Start offers to install the rule; nothing to retry
        qCInfo(lcDevices) << "CTR2 relay: starting" << device->shortName() << "automatically";
        start(path);
    }
}

void Ctr2ProxyModel::onUsbStateChanged(const QString& path)
{
    using State = TcpByteProxy::State;
    UsbLink* link = usbLink(path);
    if (!link)
        return;
    const State s = link->relay->state();
    if (s == State::Listening) {
        if (!link->answered && !link->noAnswerTimer->isActive() && !link->noAnswer)
            link->noAnswerTimer->start();
        return;
    }
    if (s == State::Error && !link->answered && usbActive()) {
        link->noAnswerHold = QDeadlineTimer(m_noAnswerRetryMs);
        QString why = link->relay->lastError();
        while (why.endsWith(QLatin1Char('.')))
            why.chop(1);
        link->stopReason = (why.isEmpty() ? QString() : why + QStringLiteral(". "))
            + tr("The CTR2 never answered. If it is still plugged in, USB mode needs CTR2-Max "
                 "firmware v2.00.00 or later, AetherCTR2, or CTR2-MIDI firmware with USB "
                 "support. For other firmware, use Wi-Fi mode.");
        emit lastErrorChanged();
    }
    // Connecting means the CTR2 answered; Stopped and Error end the run.
    link->answered = s != State::Stopped && s != State::Error;
    // What the relay knew of the firmware goes with it: read it again idle.
    if (!link->answered && path == m_usbDevicePath)
        QTimer::singleShot(0, this, &Ctr2ProxyModel::reprobeFirmware);
    if (!isRunning())
        m_runningRadioAddress = QHostAddress();
    link->noAnswerTimer->stop();
    if (link->noAnswer) {
        link->noAnswer = false;
        emit configurationChanged();
    }
}

QString Ctr2ProxyModel::noAnswerHint() const
{
    return noAnswerHint(m_usbDevicePath);
}

QString Ctr2ProxyModel::noAnswerHint(const QString& usbPath) const
{
    const UsbLink* link = usbLink(usbPath);
    if (!link || !link->noAnswer || !usbActive())
        return {};
    return tr("No answer from the CTR2. USB mode needs CTR2-Max firmware v2.00.00 or "
              "later, AetherCTR2, or CTR2-MIDI firmware with USB support. For other "
              "firmware, stop and use Wi-Fi mode.");
}

void Ctr2ProxyModel::setNoAnswerHintMs(int ms)
{
    m_noAnswerHintMs = ms;
    for (auto& [path, link] : m_usbLinks)
        link->noAnswerTimer->setInterval(ms);
}

void Ctr2ProxyModel::setUsbRadioSocketFactory(TcpByteProxy::SocketFactory factory)
{
    m_usbRadioSocketFactory = std::move(factory);
    for (auto& [path, link] : m_usbLinks)
        link->relay->setRadioSocketFactory(m_usbRadioSocketFactory);
}

bool Ctr2ProxyModel::wifiRunning() const
{
    const TcpByteProxy::State s = m_proxy->state();
    return s != TcpByteProxy::State::Stopped && s != TcpByteProxy::State::Error;
}

Ctr2ProxyModel::UsbLink* Ctr2ProxyModel::usbLink(const QString& path) const
{
    const auto it = m_usbLinks.find(path);
    return it == m_usbLinks.end() ? nullptr : it->second.get();
}

Ctr2ProxyModel::UsbLink* Ctr2ProxyModel::ensureUsbLink(const QString& path)
{
    std::unique_ptr<UsbLink>& slot = m_usbLinks[path];
    if (slot)
        return slot.get();
    slot = std::make_unique<UsbLink>();
    UsbLink* link = slot.get();
    link->relay = new Ctr2UsbRelay(this);
    if (m_usbRadioSocketFactory)
        link->relay->setRadioSocketFactory(m_usbRadioSocketFactory);
    link->noAnswerTimer = new QTimer(this);
    link->noAnswerTimer->setSingleShot(true);
    link->noAnswerTimer->setInterval(m_noAnswerHintMs);
    // Through the path, never the link: it may be gone when these fire.
    connect(link->noAnswerTimer, &QTimer::timeout, this, [this, path] {
        UsbLink* l = usbLink(path);
        if (l && !l->answered && l->relay->state() == TcpByteProxy::State::Listening) {
            l->noAnswer = true;
            emit configurationChanged();
        }
    });
    connect(link->relay, &Ctr2UsbRelay::statsChanged, this, [this] {
        if (!m_statsTimer->isActive())
            m_statsTimer->start();
    });
    connect(link->relay, &Ctr2UsbRelay::stateChanged, this, [this, path] {
        emit stateChanged();
        emit statsChanged();
        onUsbStateChanged(path);
    });
    connect(link->relay, &Ctr2UsbRelay::endpointsChanged, this, &Ctr2ProxyModel::endpointsChanged);
    connect(link->relay, &Ctr2UsbRelay::lastErrorChanged, this, &Ctr2ProxyModel::lastErrorChanged);
    connect(link->relay, &Ctr2UsbRelay::extensionsChanged, this,
            &Ctr2ProxyModel::extensionsChanged);
    return link;
}

void Ctr2ProxyModel::stopUsbLink(UsbLink* link)
{
    if (link->relay->state() != TcpByteProxy::State::Stopped)
        link->relay->stop();
    link->startError.clear();
    link->stopReason.clear();
}

const Ctr2HidPort::DeviceInfo* Ctr2ProxyModel::listedUsbDevice(const QString& path) const
{
    for (const Ctr2HidPort::DeviceInfo& d : m_usbChoices) {
        if (d.path == path)
            return &d;
    }
    return nullptr;
}

bool Ctr2ProxyModel::remembered(const Ctr2HidPort::DeviceInfo& device) const
{
    return !device.serial.isEmpty()
        && m_settings.usbDevices.contains(
            Ctr2RelaySettings::UsbDevice{device.serial, device.vendorId, device.productId});
}

void Ctr2ProxyModel::setNoAnswerRetryMs(int ms)
{
    m_noAnswerRetryMs = ms;
}

void Ctr2ProxyModel::saveSetup(const QString& usbPath)
{
    m_settings.hasSetup = true;
    m_settings.usb = m_activeTransport == Transport::Usb;
    if (m_settings.usb) {
        // Remembered for the automatic start; one with no serial number
        // cannot be told apart from the next, so it is not.
        const Ctr2HidPort::DeviceInfo* device = listedUsbDevice(usbPath);
        if (device && !device->serial.isEmpty() && !remembered(*device)) {
            m_settings.usbDevices.append({device->serial, device->vendorId, device->productId});
        }
    } else {
        m_settings.listenAddress = m_listenAddress;
        m_settings.listenPort = m_listenPort;
    }
    m_settings.save();
}

bool Ctr2ProxyModel::parsePort(const QString& text, quint16* port)
{
    bool ok = false;
    const int value = text.trimmed().toInt(&ok);
    if (!ok || value < 1 || value > 65535) {
        return false;
    }
    *port = static_cast<quint16>(value);
    return true;
}

bool Ctr2ProxyModel::parseIpv4(const QString& text, QHostAddress* address)
{
    // Dotted quad only; hostnames and IPv6 are out of prototype scope.
    static const QRegularExpression kDottedQuad(
        QStringLiteral("^\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}$"));
    if (!kDottedQuad.match(text).hasMatch()) {
        return false;
    }
    QHostAddress parsed;
    if (!parsed.setAddress(text) || parsed.protocol() != QAbstractSocket::IPv4Protocol) {
        return false;
    }
    *address = parsed;
    return true;
}

bool Ctr2ProxyModel::radioProblem(QString* problem) const
{
    if (m_aetherRadioAddress.isNull()) {
        *problem = m_aetherRadioReason;
        return true;
    }
    return false;
}

bool Ctr2ProxyModel::buildConfig(TcpByteProxy::Config* config, QString* problem) const
{
    if (radioProblem(problem)) {
        return false;
    }
    if (m_listenAddress.isEmpty()) {
        *problem = tr("Select the local address the CTR2 will connect to");
        return false;
    }
    if (!m_listenChoices.contains(m_listenAddress)
        || !parseIpv4(m_listenAddress, &config->listenAddress)) {
        *problem = tr("Listen address %1 is not on this computer").arg(m_listenAddress);
        return false;
    }
    if (!parsePort(m_listenPort, &config->listenPort)) {
        *problem = tr("Listen port must be 1-65535");
        return false;
    }
    config->upstreamAddress = m_aetherRadioAddress;
    config->upstreamPort = m_aetherRadioPort;
    const QString invalid = TcpByteProxy::validate(*config);
    if (!invalid.isEmpty()) {
        *problem = invalid;
        return false;
    }
    return true;
}

const Ctr2HidPort::DeviceInfo* Ctr2ProxyModel::selectedUsbDevice() const
{
    return listedUsbDevice(m_usbDevicePath);
}

QString Ctr2ProxyModel::configurationProblem() const
{
    if (m_transport == Transport::Usb) {
        return configurationProblem(m_usbDevicePath);
    }
    QString problem;
    TcpByteProxy::Config config;
    buildConfig(&config, &problem);
    return problem;
}

QString Ctr2ProxyModel::configurationProblem(const QString& usbPath) const
{
    QString problem;
    if (!usbAvailable()) {
        return tr("This build has no USB HID support (hidapi)");
    }
    if (m_firmware && m_firmware->isRunning()) {
        return tr("Updating the device's firmware");
    }
    if (radioProblem(&problem)) {
        return problem;
    }
    if (usbPath.isEmpty()) {
        return tr("Select the CTR2 USB device");
    }
    if (!listedUsbDevice(usbPath)) {
        return tr("The selected USB device is no longer connected; rescan");
    }
    return {};
}

bool Ctr2ProxyModel::isRunning() const
{
    return wifiRunning() || !runningUsbDevices().isEmpty();
}

bool Ctr2ProxyModel::isRunning(const QString& usbPath) const
{
    const TcpByteProxy::State s = state(usbPath);
    return s != TcpByteProxy::State::Stopped && s != TcpByteProxy::State::Error;
}

QStringList Ctr2ProxyModel::runningUsbDevices() const
{
    QStringList paths;
    for (const auto& [path, link] : m_usbLinks) {
        if (isRunning(path))
            paths.append(path);
    }
    return paths;
}

bool Ctr2ProxyModel::usbDeviceNeedsAccessRule() const
{
    return usbDeviceNeedsAccessRule(m_usbDevicePath);
}

bool Ctr2ProxyModel::usbDeviceNeedsAccessRule(const QString& path) const
{
#ifdef Q_OS_LINUX
    if (m_transport != Transport::Usb || path.isEmpty()) {
        return false;
    }
    // hidraw nodes are root-only until the rule grants the session user an
    // ACL; access() honours that ACL. A missing node (ENOENT) is not ours to fix.
    const QByteArray node = path.toLocal8Bit();
    return ::access(node.constData(), R_OK | W_OK) != 0 && errno == EACCES;
#else
    Q_UNUSED(path);
    return false;
#endif
}

bool Ctr2ProxyModel::start()
{
    if (m_transport == Transport::Usb) {
        return start(m_usbDevicePath);
    }
    if (isRunning() || !configurationProblem().isEmpty()) {
        return false;
    }
    m_activeTransport = m_transport;
    m_stopReason.clear();
    m_runningRadioAddress = m_aetherRadioAddress;
    m_runningRadioPort = m_aetherRadioPort;
    m_runningRadioLabel = m_aetherRadioLabel;
    TcpByteProxy::Config config;
    QString problem;
    buildConfig(&config, &problem);
    const bool ok = m_proxy->start(config);
    if (ok) {
        ++m_scanGeneration;   // a scan from before this session is stale
        saveSetup();   // what an automatic start uses next time
    }
    emit configurationChanged();
    return ok;
}

bool Ctr2ProxyModel::start(const QString& usbPath)
{
    if (m_transport != Transport::Usb || isRunning(usbPath)
        || !configurationProblem(usbPath).isEmpty()) {
        return false;
    }
    // The first relay up fixes the radio; every other starts to the same one
    // (a change of radio stops them all).
    if (!isRunning()) {
        m_runningRadioAddress = m_aetherRadioAddress;
        m_runningRadioPort = m_aetherRadioPort;
        m_runningRadioLabel = m_aetherRadioLabel;
    }
    m_activeTransport = Transport::Usb;
    UsbLink* link = ensureUsbLink(usbPath);
    link->startError.clear();
    link->stopReason.clear();
    link->heldByOperator = false;
    link->noAnswerHold = QDeadlineTimer();
    bool ok = false;
    QString error;
    Ctr2HidPort* hid = m_usbOpener(*listedUsbDevice(usbPath), &error);
    if (!hid) {
        link->startError = error;
        emit lastErrorChanged();
        emit stateChanged();
    } else {
        ok = link->relay->start(hid, m_runningRadioAddress, m_runningRadioPort);
        if (!ok) {
            hid->shutdown({});  // never blocks; the port deletes itself
        }
    }
    if (ok) {
        ++m_scanGeneration;   // a scan from before this session is stale
        saveSetup(usbPath);   // what an automatic start uses next time
    }
    emit configurationChanged();
    return ok;
}

void Ctr2ProxyModel::stopByOperator()
{
    m_autoStartHeld = true;
    for (const QString& path : runningUsbDevices()) {
        if (UsbLink* link = usbLink(path))
            link->heldByOperator = true;
    }
    stop();
}

void Ctr2ProxyModel::stopByOperator(const QString& usbPath)
{
    ensureUsbLink(usbPath)->heldByOperator = true;
    stop(usbPath);
}

void Ctr2ProxyModel::stop()
{
    if (m_proxy->state() != TcpByteProxy::State::Stopped) {
        m_proxy->stop();
    }
    for (auto& [path, link] : m_usbLinks) {
        stopUsbLink(link.get());
    }
    m_stopReason.clear();
    m_runningRadioAddress = QHostAddress();
    emit configurationChanged();
    emit lastErrorChanged();
    // Keep showing the device's version once the relay has let go of it.
    m_probedPath.clear();
    QTimer::singleShot(0, this, &Ctr2ProxyModel::probeFirmwareVersion);
}

void Ctr2ProxyModel::stop(const QString& usbPath)
{
    if (UsbLink* link = usbLink(usbPath))
        stopUsbLink(link);
    if (!isRunning())
        m_runningRadioAddress = QHostAddress();
    emit configurationChanged();
    emit lastErrorChanged();
    if (usbPath == m_usbDevicePath) {
        m_probedPath.clear();
        QTimer::singleShot(0, this, &Ctr2ProxyModel::probeFirmwareVersion);
    }
}

TcpByteProxy::State Ctr2ProxyModel::state() const
{
    if (usbActive()) {
        return state(m_usbDevicePath);
    }
    return m_proxy->state();
}

TcpByteProxy::State Ctr2ProxyModel::state(const QString& usbPath) const
{
    const UsbLink* link = usbLink(usbPath);
    if (!link)
        return TcpByteProxy::State::Stopped;
    return link->startError.isEmpty() ? link->relay->state() : TcpByteProxy::State::Error;
}

QString Ctr2ProxyModel::stateText() const
{
    if (usbActive()) {
        return stateText(m_usbDevicePath);
    }
    return TcpByteProxy::stateName(state());
}

QString Ctr2ProxyModel::stateText(const QString& usbPath) const
{
    const TcpByteProxy::State s = state(usbPath);
    if (s == TcpByteProxy::State::Listening) {
        return tr("Waiting for CTR2");
    }
    return TcpByteProxy::stateName(s);
}

QString Ctr2ProxyModel::lastError() const
{
    if (usbActive()) {
        return lastError(m_usbDevicePath);
    }
    return m_stopReason.isEmpty() ? m_proxy->lastError() : m_stopReason;
}

QString Ctr2ProxyModel::lastError(const QString& usbPath) const
{
    const UsbLink* link = usbLink(usbPath);
    if (!link)
        return {};
    if (!link->stopReason.isEmpty())
        return link->stopReason;
    return link->startError.isEmpty() ? link->relay->lastError() : link->startError;
}

QString Ctr2ProxyModel::listenerEndpoint() const
{
    return usbActive() ? QString() : m_proxy->listenerDescription();
}

QString Ctr2ProxyModel::peerEndpoint() const
{
    if (usbActive()) {
        if (!isRunning(m_usbDevicePath))
            return {};
        // Just the device's name: the list above has its full identity.
        const Ctr2HidPort::DeviceInfo* device = selectedUsbDevice();
        return device ? device->shortName() : usbLink(m_usbDevicePath)->relay->deviceDescription();
    }
    const TcpByteProxy::State s = state();
    if (s == TcpByteProxy::State::Connecting || s == TcpByteProxy::State::Relaying
        || s == TcpByteProxy::State::Closing) {
        return m_proxy->peerDescription();
    }
    return {};
}

QString Ctr2ProxyModel::radioEndpoint() const
{
    if (!usbActive())
        return m_proxy->upstreamDescription();
    const UsbLink* link = usbLink(m_usbDevicePath);
    return link ? link->relay->radioDescription() : QString();
}

QString Ctr2ProxyModel::usbFirmwareVersion() const
{
    return usbFirmwareVersion(m_usbDevicePath);
}

QString Ctr2ProxyModel::usbFirmwareVersion(const QString& usbPath) const
{
    if (usbActive() && isRunning(usbPath)) {
        const std::optional<ctr2hid::deviceinfo::Info> info = usbLink(usbPath)->relay->firmwareInfo();
        return info ? info->version() : QString();
    }
    return m_probedPath == usbPath ? m_probedVersion : QString();
}

bool Ctr2ProxyModel::usbFirmwareCommands() const
{
    // As usbFirmwareVersion(): from the relay's own port while it runs (the
    // idle probe never opens a device the relay holds).
    const quint32 firmware = ctr2hid::capabilityBit(ctr2hid::MessageType::Firmware);
    if (usbActive() && isRunning(m_usbDevicePath))
        return usbLink(m_usbDevicePath)->relay->offeredExtensions() & firmware;
    return m_probedPath == m_usbDevicePath && (m_probedOffers & firmware);
}

bool Ctr2ProxyModel::firmwareUpdateRunning() const
{
    return m_firmware->isRunning();
}

bool Ctr2ProxyModel::startFirmwareUpdate(const Ctr2FirmwareUpdater::Image& image, QString* problem)
{
    if (!m_firmwareOpener) {
        *problem = tr("This build of AetherSDR cannot update USB devices");
        return false;
    }
    if (m_firmware->isRunning()) {
        *problem = tr("An update is already running");
        return false;
    }
    if (m_transport != Transport::Usb) {
        *problem = tr("Select USB mode and the device to update");
        return false;
    }
    const Ctr2HidPort::DeviceInfo* device = selectedUsbDevice();
    if (!device) {
        *problem = tr("Select the USB device to update");
        return false;
    }
    const Ctr2HidPort::DeviceInfo target = *device;
    // Only one program may drive the link: its relay lets go first, and
    // comes back afterwards if it was running. Other devices' relays run on.
    m_restartAfterFirmware = isRunning(target.path);
    if (m_restartAfterFirmware) {
        stop(target.path);
    }
    m_firmware->start(target, image);
    emit configurationChanged();
    return true;
}

bool Ctr2ProxyModel::restartUsbDevice(Ctr2FirmwareUpdater::Restart target, QString* problem)
{
    if (!m_firmwareOpener || m_firmware->isRunning() || m_transport != Transport::Usb) {
        *problem = tr("Select USB mode and the device");
        return false;
    }
    const Ctr2HidPort::DeviceInfo* device = selectedUsbDevice();
    if (!device) {
        *problem = tr("Select the USB device");
        return false;
    }
    const Ctr2HidPort::DeviceInfo d = *device;
    m_restartAfterFirmware = false;   // the device leaves the bus
    m_restartedPath = d.path;
    m_restartedDevice = d;
    m_restartedSeenGone = false;
    m_restartedHold = QDeadlineTimer(kRestartHoldMs);
    if (isRunning(d.path)) {
        stop(d.path);
    }
    m_firmware->restartDevice(d, target);
    emit configurationChanged();
    return true;
}

void Ctr2ProxyModel::cancelFirmwareUpdate()
{
    m_firmware->cancel();
}

namespace {
using AetherSDR::ctr2hid::MessageType;

bool relayWants(const Ctr2UsbRelay* relay, MessageType type)
{
    return relay->state() == TcpByteProxy::State::Relaying
        && (relay->extensions() & ctr2hid::capabilityBit(type));
}
} // namespace

bool Ctr2ProxyModel::audioSpectrumWanted() const
{
    if (!usbActive())
        return false;
    for (const auto& [path, link] : m_usbLinks) {
        if (relayWants(link->relay, MessageType::AudioSpectrum))
            return true;
    }
    return false;
}

bool Ctr2ProxyModel::audioSpectrumLinearWanted() const
{
    if (!usbActive())
        return false;
    for (const auto& [path, link] : m_usbLinks) {
        if (relayWants(link->relay, MessageType::AudioSpectrum)
            && relayWants(link->relay, MessageType::AudioSpectrumLinear)) {
            return true;
        }
    }
    return false;
}

bool Ctr2ProxyModel::cwTextWanted() const
{
    if (!usbActive())
        return false;
    for (const auto& [path, link] : m_usbLinks) {
        if (relayWants(link->relay, MessageType::CwText))
            return true;
    }
    return false;
}

bool Ctr2ProxyModel::sendCwText(const QString& text, float cost)
{
    if (!cwTextWanted()) {
        return false;
    }
    const QByteArray payload = ctr2hid::cwtext::encode(text, cost);
    if (payload.isEmpty())
        return false;
    bool sent = false;
    for (const auto& [path, link] : m_usbLinks) {
        if (relayWants(link->relay, MessageType::CwText))
            sent = link->relay->sendCwText(payload) || sent;
    }
    return sent;
}

bool Ctr2ProxyModel::sendAudioSpectrum(int lowHz, int spanHz, const std::vector<float>& barsDb,
                                       bool linear)
{
    if (!usbActive()) {
        return false;
    }
    const MessageType type = linear ? MessageType::AudioSpectrumLinear : MessageType::AudioSpectrum;
    QByteArray payload;
    bool sent = false;
    for (const auto& [path, link] : m_usbLinks) {
        if (!relayWants(link->relay, MessageType::AudioSpectrum)
            || (linear && !relayWants(link->relay, MessageType::AudioSpectrumLinear))) {
            continue;
        }
        if (payload.isEmpty())
            payload = ctr2hid::spectrum::encode(lowHz, spanHz, barsDb);
        sent = link->relay->sendAudioSpectrum(payload, type) || sent;
    }
    return sent;
}

int Ctr2ProxyModel::audioSpectrumLowHz(int spanHz)
{
    return ctr2hid::spectrum::displayLowHz(spanHz);
}

double Ctr2ProxyModel::audioSpectrumBandEdgeHz(int lowHz, int spanHz, int n, int i, bool linear)
{
    return linear ? ctr2hid::spectrum::linearEdgeHz(lowHz, spanHz, n, i)
                  : ctr2hid::spectrum::bandEdgeHz(lowHz, spanHz, n, i);
}

void Ctr2ProxyModel::audioSpectrumCwWindow(int filterLo, int filterHi, int pitchHz,
                                           double sampleRate, int* lowHz, int* highHz)
{
    const auto w = ctr2hid::spectrum::cwWindow(filterLo, filterHi, pitchHz, sampleRate);
    *lowHz = w.lowHz;
    *highHz = w.highHz;
}

int Ctr2ProxyModel::audioSpectrumSpanHz(int filterLo, int filterHi, double sampleRate)
{
    return ctr2hid::spectrum::displaySpanHz(filterLo, filterHi, sampleRate);
}

TcpByteProxy::Stats Ctr2ProxyModel::stats() const
{
    if (!usbActive())
        return m_proxy->stats();
    const UsbLink* link = usbLink(m_usbDevicePath);
    return link ? link->relay->stats() : TcpByteProxy::Stats{};
}

} // namespace AetherSDR
