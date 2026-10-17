#include "Ctr2ProxyModel.h"

#include "core/Ctr2HidFraming.h"
#include "core/Ctr2UsbRelay.h"
#include "core/LogManager.h"
#ifdef HAVE_HIDAPI
#include "core/Ctr2HidapiPort.h"
#endif

#include <QHostAddress>
#include <QNetworkInterface>
#include <QRegularExpression>
#include <QTimer>

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
    , m_usb(new Ctr2UsbRelay(this))
    , m_aetherRadioReason(tr("Connect AetherSDR to a radio first"))
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
    connect(m_usb, &Ctr2UsbRelay::statsChanged, this, coalesceStats);
    connect(m_usb, &Ctr2UsbRelay::stateChanged, this, stateMoved);
    connect(m_usb, &Ctr2UsbRelay::endpointsChanged, this, &Ctr2ProxyModel::endpointsChanged);
    connect(m_usb, &Ctr2UsbRelay::lastErrorChanged, this, &Ctr2ProxyModel::lastErrorChanged);
    connect(m_usb, &Ctr2UsbRelay::extensionsChanged, this, &Ctr2ProxyModel::extensionsChanged);
    // Through the members, so a backend set later (tests) is the one used.
    m_firmware = new Ctr2FirmwareUpdater(
        [this] { return m_usbDevices ? m_usbDevices() : QList<Ctr2HidPort::DeviceInfo>(); },
        [this](const Ctr2HidPort::DeviceInfo& device, QString* error) -> Ctr2HidPort* {
            return m_firmwareOpener
                ? m_firmwareOpener(device, ctr2hid::capabilityBit(ctr2hid::MessageType::Firmware),
                                   error)
                : nullptr;
        },
        this);
    connect(m_firmware, &Ctr2FirmwareUpdater::progress, this, &Ctr2ProxyModel::firmwareUpdateProgress);
    connect(m_firmware, &Ctr2FirmwareUpdater::stepChanged, this, &Ctr2ProxyModel::firmwareUpdateStep);
    connect(m_firmware, &Ctr2FirmwareUpdater::finished, this,
            [this](bool ok, const QString& message, const QString&) {
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
    m_deviceRescan = new QTimer(this);
    m_deviceRescan->setInterval(kDeviceRescanMs);
    connect(m_deviceRescan, &QTimer::timeout, this, [this] {
        if (m_usbDevices && !isRunning() && !m_firmware->isRunning())
            refreshDevices();
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
                                   UsbCapsPortOpener firmwareOpener)
{
    m_usbDevices = std::move(devices);
    m_usbOpener = std::move(opener);
    m_firmwareOpener = std::move(firmwareOpener);
    m_usbChoices.clear();
    refreshDevices();
    emit configurationChanged();
}

void Ctr2ProxyModel::refreshDevices()
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

    const QList<Ctr2HidPort::DeviceInfo> usb = m_usbDevices ? m_usbDevices()
                                                            : QList<Ctr2HidPort::DeviceInfo>();
    bool usbChanged = usb.size() != m_usbChoices.size();
    for (int i = 0; !usbChanged && i < usb.size(); ++i) {
        usbChanged = usb[i].path != m_usbChoices[i].path;
    }
    if (choices == m_listenChoices && !usbChanged) {
        return;
    }
    m_listenChoices = choices;
    m_usbChoices = usb;
    // Default to the first recognized CTR2 while nothing is selected or the
    // selected device has gone; an existing choice stands.
    if (!isRunning() && !selectedUsbDevice()) {
        m_usbDevicePath.clear();
        // The device the relay last started with, when it is here.
        for (const Ctr2HidPort::DeviceInfo& d : std::as_const(m_usbChoices)) {
            if (!m_settings.usbSerial.isEmpty() && d.serial == m_settings.usbSerial
                && d.vendorId == m_settings.usbVendorId && d.productId == m_settings.usbProductId) {
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
void Ctr2ProxyModel::probeFirmwareVersion()
{
    if (!m_firmwareOpener || m_probe || isRunning() || m_firmware->isRunning()
        || m_transport != Transport::Usb) {
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
        m_usbStartError.clear();
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
    if (isRunning()) {
        return false;
    }
    if (m_usbDevicePath != path) {
        m_usbDevicePath = path;
        emit configurationChanged();
        probeFirmwareVersion();
    }
    return true;
}

void Ctr2ProxyModel::setAetherRadio(const QHostAddress& address, quint16 port,
                                    const QString& label, const QString& unavailableReason)
{
    QHostAddress ipv4;
    bool ok = false;
    const quint32 v4 = address.toIPv4Address(&ok);
    if (ok && v4 != 0 && port != 0) {
        ipv4 = QHostAddress(v4);
    }
    const QString reason = ipv4.isNull() && unavailableReason.isEmpty()
        ? tr("Connect AetherSDR to a radio first") : unavailableReason;
    if (isRunning() && (ipv4 != m_runningRadioAddress || port != m_runningRadioPort)) {
        const QString was = m_runningRadioLabel.isEmpty() ? m_runningRadioAddress.toString()
                                                          : m_runningRadioLabel;
        const QString why = ipv4.isNull()
            ? tr("Stopped: AetherSDR is no longer connected to %1").arg(was)
            : tr("Stopped: AetherSDR switched from %1 to another radio").arg(was);
        qCInfo(lcDevices) << "CTR2 relay:" << why;
        stop();
        m_stopReason = why;
        emit lastErrorChanged();
    }
    if (ipv4 == m_aetherRadioAddress && port == m_aetherRadioPort
        && label == m_aetherRadioLabel && reason == m_aetherRadioReason) {
        return;
    }
    const bool newRadio = !ipv4.isNull()
        && (ipv4 != m_aetherRadioAddress || port != m_aetherRadioPort);
    m_aetherRadioAddress = ipv4;
    m_aetherRadioPort = port;
    m_aetherRadioLabel = label;
    m_aetherRadioReason = ipv4.isNull() ? reason : QString();
    emit configurationChanged();
    if (newRadio) {
        m_autoStartHeld = false;   // a new connection: start again
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
        m_autoStartTimer->start();
        QTimer::singleShot(0, this, &Ctr2ProxyModel::tryAutoStart);
    } else {
        m_autoStartTimer->stop();
    }
    emit autoStartChanged();
}

void Ctr2ProxyModel::tryAutoStart()
{
    if (!m_settings.autoStart || m_autoStartHeld || isRunning()
        || m_firmware->isRunning() || m_aetherRadioAddress.isNull()) {
        return;
    }
    refreshDevices();   // the device may have been plugged in since
    if (!configurationProblem().isEmpty())
        return;
    if (m_transport == Transport::Usb) {
        const Ctr2HidPort::DeviceInfo* device = selectedUsbDevice();
        if (!device || !device->usbLink())
            return;   // a stock CTR2 has no USB mode
        if (usbDeviceNeedsAccessRule())
            return;   // Start offers to install the rule; nothing to retry
    }
    qCInfo(lcDevices) << "CTR2 relay: starting automatically";
    start();
}

void Ctr2ProxyModel::saveSetup()
{
    m_settings.hasSetup = true;
    m_settings.usb = m_activeTransport == Transport::Usb;
    if (m_settings.usb) {
        if (const Ctr2HidPort::DeviceInfo* device = selectedUsbDevice()) {
            m_settings.usbSerial = device->serial;
            m_settings.usbVendorId = device->vendorId;
            m_settings.usbProductId = device->productId;
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
    for (const Ctr2HidPort::DeviceInfo& d : m_usbChoices) {
        if (d.path == m_usbDevicePath) {
            return &d;
        }
    }
    return nullptr;
}

QString Ctr2ProxyModel::configurationProblem() const
{
    QString problem;
    if (m_transport == Transport::Usb) {
        if (!usbAvailable()) {
            return tr("This build has no USB HID support (hidapi)");
        }
        if (m_firmware && m_firmware->isRunning()) {
            return tr("Updating the device's firmware");
        }
        if (radioProblem(&problem)) {
            return problem;
        }
        if (m_usbDevicePath.isEmpty()) {
            return tr("Select the CTR2 USB device");
        }
        if (!selectedUsbDevice()) {
            return tr("The selected USB device is no longer connected; rescan");
        }
        return {};
    }
    TcpByteProxy::Config config;
    buildConfig(&config, &problem);
    return problem;
}

bool Ctr2ProxyModel::isRunning() const
{
    const TcpByteProxy::State s = state();
    return s != TcpByteProxy::State::Stopped && s != TcpByteProxy::State::Error;
}

bool Ctr2ProxyModel::usbDeviceNeedsAccessRule() const
{
#ifdef Q_OS_LINUX
    if (m_transport != Transport::Usb || m_usbDevicePath.isEmpty()) {
        return false;
    }
    // hidraw nodes are root-only until the rule grants the session user an
    // ACL; access() honours that ACL. A missing node (ENOENT) is not ours to fix.
    const QByteArray path = m_usbDevicePath.toLocal8Bit();
    return ::access(path.constData(), R_OK | W_OK) != 0 && errno == EACCES;
#else
    return false;
#endif
}

bool Ctr2ProxyModel::start()
{
    if (isRunning() || !configurationProblem().isEmpty()) {
        return false;
    }
    m_activeTransport = m_transport;
    m_usbStartError.clear();
    m_stopReason.clear();
    m_runningRadioAddress = m_aetherRadioAddress;
    m_runningRadioPort = m_aetherRadioPort;
    m_runningRadioLabel = m_aetherRadioLabel;
    bool ok = false;
    if (m_transport == Transport::Usb) {
        QString error;
        Ctr2HidPort* hid = m_usbOpener(*selectedUsbDevice(), &error);
        if (!hid) {
            m_usbStartError = error;
            emit lastErrorChanged();
            emit stateChanged();
        } else {
            ok = m_usb->start(hid, m_aetherRadioAddress, m_aetherRadioPort);
            if (!ok) {
                hid->shutdown({});  // never blocks; the port deletes itself
            }
        }
    } else {
        TcpByteProxy::Config config;
        QString problem;
        buildConfig(&config, &problem);
        ok = m_proxy->start(config);
    }
    if (ok)
        saveSetup();   // what an automatic start uses next time
    emit configurationChanged();
    return ok;
}

void Ctr2ProxyModel::stopByOperator()
{
    m_autoStartHeld = true;
    stop();
}

void Ctr2ProxyModel::stop()
{
    if (m_proxy->state() != TcpByteProxy::State::Stopped) {
        m_proxy->stop();
    }
    if (m_usb->state() != TcpByteProxy::State::Stopped) {
        m_usb->stop();
    }
    m_usbStartError.clear();
    m_stopReason.clear();
    m_runningRadioAddress = QHostAddress();
    emit configurationChanged();
    emit lastErrorChanged();
    // Keep showing the device's version once the relay has let go of it.
    m_probedPath.clear();
    QTimer::singleShot(0, this, &Ctr2ProxyModel::probeFirmwareVersion);
}

TcpByteProxy::State Ctr2ProxyModel::state() const
{
    if (usbActive()) {
        return m_usbStartError.isEmpty() ? m_usb->state() : TcpByteProxy::State::Error;
    }
    return m_proxy->state();
}

QString Ctr2ProxyModel::stateText() const
{
    const TcpByteProxy::State s = state();
    if (usbActive() && s == TcpByteProxy::State::Listening) {
        return tr("Waiting for CTR2");
    }
    return TcpByteProxy::stateName(s);
}

QString Ctr2ProxyModel::lastError() const
{
    if (!m_stopReason.isEmpty()) {
        return m_stopReason;
    }
    if (usbActive()) {
        return m_usbStartError.isEmpty() ? m_usb->lastError() : m_usbStartError;
    }
    return m_proxy->lastError();
}

QString Ctr2ProxyModel::listenerEndpoint() const
{
    return usbActive() ? QString() : m_proxy->listenerDescription();
}

QString Ctr2ProxyModel::peerEndpoint() const
{
    if (usbActive()) {
        if (!isRunning())
            return {};
        // Just the device's name: the list above has its full identity.
        const Ctr2HidPort::DeviceInfo* device = selectedUsbDevice();
        return device ? device->shortName() : m_usb->deviceDescription();
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
    return usbActive() ? m_usb->radioDescription() : m_proxy->upstreamDescription();
}

QString Ctr2ProxyModel::usbFirmwareVersion() const
{
    if (usbActive() && isRunning()) {
        const std::optional<ctr2hid::deviceinfo::Info> info = m_usb->firmwareInfo();
        return info ? info->version() : QString();
    }
    return m_probedPath == m_usbDevicePath ? m_probedVersion : QString();
}

bool Ctr2ProxyModel::usbFirmwareCommands() const
{
    // As usbFirmwareVersion(): from the relay's own port while it runs (the
    // idle probe never opens a device the relay holds).
    const quint32 firmware = ctr2hid::capabilityBit(ctr2hid::MessageType::Firmware);
    if (usbActive() && isRunning())
        return m_usb->offeredExtensions() & firmware;
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
    // Only one program may drive the link: the relay lets go first, and
    // comes back afterwards if it was running.
    m_restartAfterFirmware = isRunning() && usbActive();
    if (isRunning()) {
        stop();
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
    if (isRunning()) {
        stop();
    }
    m_firmware->restartDevice(d, target);
    emit configurationChanged();
    return true;
}

void Ctr2ProxyModel::cancelFirmwareUpdate()
{
    m_firmware->cancel();
}

bool Ctr2ProxyModel::audioSpectrumWanted() const
{
    return usbActive() && m_usb->state() == TcpByteProxy::State::Relaying
        && (m_usb->extensions() & ctr2hid::capabilityBit(ctr2hid::MessageType::AudioSpectrum));
}

bool Ctr2ProxyModel::audioSpectrumLinearWanted() const
{
    return audioSpectrumWanted()
        && (m_usb->extensions()
            & ctr2hid::capabilityBit(ctr2hid::MessageType::AudioSpectrumLinear));
}

bool Ctr2ProxyModel::cwTextWanted() const
{
    return usbActive() && m_usb->state() == TcpByteProxy::State::Relaying
        && (m_usb->extensions() & ctr2hid::capabilityBit(ctr2hid::MessageType::CwText));
}

bool Ctr2ProxyModel::sendCwText(const QString& text, float cost)
{
    if (!cwTextWanted()) {
        return false;
    }
    const QByteArray payload = ctr2hid::cwtext::encode(text, cost);
    return !payload.isEmpty() && m_usb->sendCwText(payload);
}

bool Ctr2ProxyModel::sendAudioSpectrum(int lowHz, int spanHz, const std::vector<float>& barsDb,
                                       bool linear)
{
    if (linear && !audioSpectrumLinearWanted()) {
        return false;
    }
    return audioSpectrumWanted()
        && m_usb->sendAudioSpectrum(ctr2hid::spectrum::encode(lowHz, spanHz, barsDb),
                                    linear ? ctr2hid::MessageType::AudioSpectrumLinear
                                           : ctr2hid::MessageType::AudioSpectrum);
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
    return usbActive() ? m_usb->stats() : m_proxy->stats();
}

} // namespace AetherSDR
