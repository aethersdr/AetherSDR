#include "Ctr2ProxyModel.h"

#include "core/Ctr2UsbRelay.h"
#ifdef HAVE_HIDAPI
#include "core/Ctr2HidapiPort.h"
#endif

#include <QHostAddress>
#include <QNetworkInterface>
#include <QRegularExpression>
#include <QTimer>

namespace AetherSDR {

namespace {

constexpr int kStatsRefreshMs = 250;

} // namespace

Ctr2ProxyModel::Ctr2ProxyModel(QObject* parent)
    : QObject(parent)
    , m_proxy(new TcpByteProxy(this))
    , m_usb(new Ctr2UsbRelay(this))
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
    refreshDevices();
}

Ctr2ProxyModel::~Ctr2ProxyModel()
{
    stop();
}

bool Ctr2ProxyModel::usbSupported()
{
#ifdef HAVE_HIDAPI
    return true;
#else
    return false;
#endif
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

    QList<Ctr2HidPort::DeviceInfo> usb;
#ifdef HAVE_HIDAPI
    usb = Ctr2HidapiPort::enumerate();
#endif
    bool usbChanged = usb.size() != m_usbChoices.size();
    for (int i = 0; !usbChanged && i < usb.size(); ++i) {
        usbChanged = usb[i].path != m_usbChoices[i].path;
    }
    if (choices == m_listenChoices && !usbChanged) {
        return;
    }
    m_listenChoices = choices;
    m_usbChoices = usb;
    emit listenAddressesChanged();
    emit configurationChanged();
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
        emit configurationChanged();
        emit stateChanged();
        emit endpointsChanged();
        emit lastErrorChanged();
        emit statsChanged();
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
    }
    return true;
}

bool Ctr2ProxyModel::setRadioAddress(const QString& address)
{
    if (isRunning()) {
        return false;
    }
    const QString trimmed = address.trimmed();
    if (m_radioAddress != trimmed) {
        m_radioAddress = trimmed;
        emit configurationChanged();
    }
    return true;
}

bool Ctr2ProxyModel::setRadioPortText(const QString& port)
{
    if (isRunning()) {
        return false;
    }
    if (m_radioPort != port) {
        m_radioPort = port;
        emit configurationChanged();
    }
    return true;
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

bool Ctr2ProxyModel::buildRadioEndpoint(QHostAddress* address, quint16* port,
                                        QString* problem) const
{
    if (m_radioAddress.isEmpty()) {
        *problem = tr("Enter the radio's IPv4 address");
        return false;
    }
    if (!parseIpv4(m_radioAddress, address)) {
        *problem = tr("Radio address must be an IPv4 address such as 192.168.1.50");
        return false;
    }
    if (!parsePort(m_radioPort, port)) {
        *problem = tr("Radio port must be 1-65535");
        return false;
    }
    return true;
}

bool Ctr2ProxyModel::buildConfig(TcpByteProxy::Config* config, QString* problem) const
{
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
    if (!buildRadioEndpoint(&config->upstreamAddress, &config->upstreamPort, problem)) {
        return false;
    }
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
        if (!usbSupported()) {
            return tr("This build has no USB HID support (hidapi)");
        }
        if (m_usbDevicePath.isEmpty()) {
            return tr("Select the CTR2 USB device");
        }
        if (!selectedUsbDevice()) {
            return tr("The selected USB device is no longer connected; rescan");
        }
        QHostAddress address;
        quint16 port = 0;
        buildRadioEndpoint(&address, &port, &problem);
        return problem;
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

bool Ctr2ProxyModel::start()
{
    if (isRunning() || !configurationProblem().isEmpty()) {
        return false;
    }
    m_activeTransport = m_transport;
    m_usbStartError.clear();
    bool ok = false;
    if (m_transport == Transport::Usb) {
#ifdef HAVE_HIDAPI
        QHostAddress address;
        quint16 port = 0;
        QString problem;
        buildRadioEndpoint(&address, &port, &problem);
        QString error;
        Ctr2HidapiPort* hid = Ctr2HidapiPort::open(*selectedUsbDevice(), &error);
        if (!hid) {
            m_usbStartError = error;
            emit lastErrorChanged();
            emit stateChanged();
        } else {
            ok = m_usb->start(hid, address, port);
            if (!ok) {
                delete hid;
            }
        }
#endif
    } else {
        TcpByteProxy::Config config;
        QString problem;
        buildConfig(&config, &problem);
        ok = m_proxy->start(config);
    }
    emit configurationChanged();
    return ok;
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
    emit configurationChanged();
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
        return isRunning() ? m_usb->deviceDescription() : QString();
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

TcpByteProxy::Stats Ctr2ProxyModel::stats() const
{
    return usbActive() ? m_usb->stats() : m_proxy->stats();
}

} // namespace AetherSDR
