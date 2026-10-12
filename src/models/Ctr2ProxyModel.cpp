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
    , m_aetherRadioReason(tr("Connect AetherSDR to a radio first"))
{
    m_statsTimer = new QTimer(this);
    m_statsTimer->setSingleShot(true);
    m_statsTimer->setInterval(kStatsRefreshMs);
    connect(m_statsTimer, &QTimer::timeout, this, &Ctr2ProxyModel::statsChanged);
    TcpByteProxy::Tuning tuning;
    tuning.maxClients = kMaxWifiClients;
    m_proxy->setTuning(tuning);
    connect(m_proxy, &TcpByteProxy::statsChanged, this, &Ctr2ProxyModel::coalesceStats);
    connect(m_proxy, &TcpByteProxy::stateChanged, this, &Ctr2ProxyModel::stateMoved);
    connect(m_proxy, &TcpByteProxy::endpointsChanged, this, &Ctr2ProxyModel::endpointsChanged);
    connect(m_proxy, &TcpByteProxy::lastErrorChanged, this, &Ctr2ProxyModel::lastErrorChanged);
#ifdef HAVE_HIDAPI
    m_usbDevices = [] { return Ctr2HidapiPort::enumerate(); };
    m_usbOpener = [](const Ctr2HidPort::DeviceInfo& device, QString* error) -> Ctr2HidPort* {
        return Ctr2HidapiPort::open(device, error);
    };
    // USB is the default whenever this build can open a CTR2.
    m_wifiEnabled = false;
#endif
    refreshDevices();
}

Ctr2ProxyModel::~Ctr2ProxyModel()
{
    stop();
}

void Ctr2ProxyModel::coalesceStats()
{
    if (!m_statsTimer->isActive()) {
        m_statsTimer->start();
    }
}

void Ctr2ProxyModel::stateMoved()
{
    emit stateChanged();
    emit statsChanged();
}

void Ctr2ProxyModel::setUsbBackend(UsbDeviceSource devices, UsbPortOpener opener)
{
    m_usbDevices = std::move(devices);
    m_usbOpener = std::move(opener);
    m_usbChoices.clear();
    m_usbSelected.clear();
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
    if (usbChanged) {
        // Check each recognized CTR2 the first time it appears; a choice the
        // operator made for a device still present stands. Running links
        // keep their own devices, so this never touches a live relay.
        QSet<QString> seen;
        for (const Ctr2HidPort::DeviceInfo& d : std::as_const(m_usbChoices)) {
            seen.insert(d.path);
        }
        QSet<QString> selected;
        for (const Ctr2HidPort::DeviceInfo& d : usb) {
            if (seen.contains(d.path) ? m_usbSelected.contains(d.path) : !d.ctr2Model().isEmpty()) {
                selected.insert(d.path);
            }
        }
        m_usbSelected = selected;
    }
    m_usbChoices = usb;
    emit listenAddressesChanged();
    emit configurationChanged();
}

bool Ctr2ProxyModel::setWifiEnabled(bool enabled)
{
    if (isRunning()) {
        return false;
    }
    if (m_wifiEnabled != enabled) {
        m_wifiEnabled = enabled;
        emit configurationChanged();
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

QStringList Ctr2ProxyModel::selectedUsbDevices() const
{
    QStringList paths;
    for (const Ctr2HidPort::DeviceInfo& d : m_usbChoices) {
        if (m_usbSelected.contains(d.path)) {
            paths.append(d.path);
        }
    }
    return paths;
}

bool Ctr2ProxyModel::setUsbDeviceSelected(const QString& path, bool selected)
{
    if (isRunning()) {
        return false;
    }
    if (selected && !usbDevice(path)) {
        return false;
    }
    if (m_usbSelected.contains(path) != selected) {
        if (selected) {
            m_usbSelected.insert(path);
        } else {
            m_usbSelected.remove(path);
        }
        emit configurationChanged();
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
    m_aetherRadioAddress = ipv4;
    m_aetherRadioPort = port;
    m_aetherRadioLabel = label;
    m_aetherRadioReason = ipv4.isNull() ? reason : QString();
    emit configurationChanged();
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

const Ctr2HidPort::DeviceInfo* Ctr2ProxyModel::usbDevice(const QString& path) const
{
    for (const Ctr2HidPort::DeviceInfo& d : m_usbChoices) {
        if (d.path == path) {
            return &d;
        }
    }
    return nullptr;
}

QString Ctr2ProxyModel::configurationProblem() const
{
    QString problem;
    if (radioProblem(&problem)) {
        return problem;
    }
    if (!m_wifiEnabled && m_usbSelected.isEmpty()) {
        return usbAvailable() ? tr("Check a CTR2 USB device or enable Wi-Fi")
                              : tr("Enable Wi-Fi (this build has no USB HID support)");
    }
    if (!m_usbSelected.isEmpty() && !usbAvailable()) {
        return tr("This build has no USB HID support (hidapi)");
    }
    for (const QString& path : m_usbSelected) {
        if (!usbDevice(path)) {
            return tr("A checked USB device is no longer connected; rescan");
        }
    }
    if (m_wifiEnabled) {
        TcpByteProxy::Config config;
        buildConfig(&config, &problem);
    }
    return problem;
}

bool Ctr2ProxyModel::isRunning() const
{
    const auto live = [](TcpByteProxy::State s) {
        return s != TcpByteProxy::State::Stopped && s != TcpByteProxy::State::Error;
    };
    if (live(m_proxy->state())) {
        return true;
    }
    for (const UsbLink& link : m_usbLinks) {
        if (link.relay && link.startError.isEmpty() && live(link.relay->state())) {
            return true;
        }
    }
    return false;
}

bool Ctr2ProxyModel::usbDeviceNeedsAccessRule() const
{
#ifdef Q_OS_LINUX
    // hidraw nodes are root-only until the rule grants the session user an
    // ACL; access() honours that ACL. A missing node (ENOENT) is not ours to fix.
    for (const QString& path : m_usbSelected) {
        const QByteArray node = path.toLocal8Bit();
        if (::access(node.constData(), R_OK | W_OK) != 0 && errno == EACCES) {
            return true;
        }
    }
#endif
    return false;
}

void Ctr2ProxyModel::clearUsbLinks()
{
    for (UsbLink& link : m_usbLinks) {
        if (!link.relay) {
            continue;
        }
        QObject::disconnect(link.relay, nullptr, this, nullptr);
        link.relay->stop();
        // Deferred: a relay may be mid-signal when the operator stops.
        link.relay->deleteLater();
    }
    m_usbLinks.clear();
}

void Ctr2ProxyModel::startUsb(const Ctr2HidPort::DeviceInfo& device)
{
    UsbLink link;
    link.path = device.path;
    link.label = device.label();
    link.relay = new Ctr2UsbRelay(this);
    connect(link.relay, &Ctr2UsbRelay::statsChanged, this, &Ctr2ProxyModel::coalesceStats);
    connect(link.relay, &Ctr2UsbRelay::stateChanged, this, &Ctr2ProxyModel::stateMoved);
    connect(link.relay, &Ctr2UsbRelay::endpointsChanged, this, &Ctr2ProxyModel::endpointsChanged);
    connect(link.relay, &Ctr2UsbRelay::lastErrorChanged, this, &Ctr2ProxyModel::lastErrorChanged);
    connect(link.relay, &Ctr2UsbRelay::extensionsChanged, this, &Ctr2ProxyModel::extensionsChanged);
    QString error;
    Ctr2HidPort* hid = m_usbOpener(device, &error);
    if (!hid) {
        link.startError = error.isEmpty() ? tr("Could not open the device") : error;
        qCWarning(lcDevices) << "CTR2 USB:" << link.label << link.startError;
    } else if (!link.relay->start(hid, m_runningRadioAddress, m_runningRadioPort)) {
        hid->shutdown({});  // never blocks; the port deletes itself
    }
    m_usbLinks.push_back(link);
}

bool Ctr2ProxyModel::start()
{
    if (isRunning() || !configurationProblem().isEmpty()) {
        return false;
    }
    if (m_proxy->state() != TcpByteProxy::State::Stopped) {
        m_proxy->stop();   // clear a previous run's error
    }
    clearUsbLinks();
    m_stopReason.clear();
    m_runningRadioAddress = m_aetherRadioAddress;
    m_runningRadioPort = m_aetherRadioPort;
    m_runningRadioLabel = m_aetherRadioLabel;
    m_wifiActive = m_wifiEnabled;
    if (m_wifiEnabled) {
        TcpByteProxy::Config config;
        QString problem;
        buildConfig(&config, &problem);
        m_proxy->start(config);
    }
    for (const QString& path : selectedUsbDevices()) {
        startUsb(*usbDevice(path));
    }
    const bool running = isRunning();
    if (!running) {
        m_runningRadioAddress = QHostAddress();
    }
    emit configurationChanged();
    emit stateChanged();
    emit endpointsChanged();
    emit lastErrorChanged();
    emit statsChanged();
    return running;
}

void Ctr2ProxyModel::stop()
{
    if (m_proxy->state() != TcpByteProxy::State::Stopped) {
        m_proxy->stop();
    }
    clearUsbLinks();
    m_wifiActive = false;
    m_stopReason.clear();
    m_runningRadioAddress = QHostAddress();
    emit configurationChanged();
    emit stateChanged();
    emit endpointsChanged();
    emit lastErrorChanged();
    emit statsChanged();
}

QList<Ctr2ProxyModel::Link> Ctr2ProxyModel::links() const
{
    QList<Link> out;
    if (m_wifiActive) {
        const QList<TcpByteProxy::Client> clients = m_proxy->clients();
        Link listener;
        listener.kind = Link::Kind::WifiListener;
        const QString endpoint = m_proxy->listenerDescription();
        listener.name = tr("Wi-Fi %1").arg(endpoint.isEmpty()
            ? QStringLiteral("%1:%2").arg(m_listenAddress, m_listenPort.trimmed()) : endpoint);
        const TcpByteProxy::State s = m_proxy->state();
        listener.state = s;
        if (s == TcpByteProxy::State::Stopped || s == TcpByteProxy::State::Error) {
            listener.stateText = TcpByteProxy::stateName(s);
        } else if (clients.isEmpty()) {
            listener.stateText = tr("Listening");
        } else {
            listener.stateText = tr("Listening, %1 of %2 connected")
                                     .arg(clients.size()).arg(kMaxWifiClients);
        }
        listener.stats.rejectedClients = m_proxy->stats().rejectedClients;
        listener.error = m_proxy->lastError();
        out.append(listener);
        for (const TcpByteProxy::Client& c : clients) {
            Link client;
            client.kind = Link::Kind::WifiClient;
            client.name = tr("CTR2 %1").arg(c.peer);
            client.state = c.state;
            client.stateText = TcpByteProxy::stateName(c.state);
            client.stats = c.stats;
            out.append(client);
        }
    }
    for (const UsbLink& usb : m_usbLinks) {
        Link link;
        link.kind = Link::Kind::Usb;
        link.name = usb.label;
        if (!usb.startError.isEmpty() || !usb.relay) {
            link.state = TcpByteProxy::State::Error;
            link.error = usb.startError;
        } else {
            link.state = usb.relay->state();
            link.stats = usb.relay->stats();
            link.error = usb.relay->lastError();
        }
        link.stateText = link.state == TcpByteProxy::State::Listening
            ? tr("Waiting for CTR2") : TcpByteProxy::stateName(link.state);
        out.append(link);
    }
    return out;
}

QString Ctr2ProxyModel::lastError() const
{
    if (!m_stopReason.isEmpty()) {
        return m_stopReason;
    }
    for (const Link& link : links()) {
        if (!link.error.isEmpty()) {
            return link.error;
        }
    }
    return {};
}

QString Ctr2ProxyModel::radioEndpoint() const
{
    if (m_runningRadioAddress.isNull()) {
        return {};
    }
    return QStringLiteral("%1:%2").arg(m_runningRadioAddress.toString()).arg(m_runningRadioPort);
}

bool Ctr2ProxyModel::audioSpectrumWanted() const
{
    const quint32 bit = ctr2hid::capabilityBit(ctr2hid::MessageType::AudioSpectrum);
    for (const UsbLink& link : m_usbLinks) {
        if (link.relay && link.startError.isEmpty()
            && link.relay->state() == TcpByteProxy::State::Relaying
            && (link.relay->extensions() & bit)) {
            return true;
        }
    }
    return false;
}

bool Ctr2ProxyModel::sendAudioSpectrum(int lowHz, int spanHz, const std::vector<float>& barsDb)
{
    if (!audioSpectrumWanted()) {
        return false;
    }
    const QByteArray payload = ctr2hid::spectrum::encode(lowHz, spanHz, barsDb);
    bool sent = false;
    for (const UsbLink& link : m_usbLinks) {
        // Each relay sends only to a device that negotiated the extension.
        if (link.relay && link.startError.isEmpty()) {
            sent = link.relay->sendAudioSpectrum(payload) || sent;
        }
    }
    return sent;
}

int Ctr2ProxyModel::audioSpectrumLowHz(int spanHz)
{
    return ctr2hid::spectrum::displayLowHz(spanHz);
}

double Ctr2ProxyModel::audioSpectrumBandEdgeHz(int lowHz, int spanHz, int n, int i)
{
    return ctr2hid::spectrum::bandEdgeHz(lowHz, spanHz, n, i);
}

int Ctr2ProxyModel::audioSpectrumSpanHz(int filterLo, int filterHi, double sampleRate)
{
    return ctr2hid::spectrum::displaySpanHz(filterLo, filterHi, sampleRate);
}

} // namespace AetherSDR
