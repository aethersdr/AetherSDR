#include "Ctr2ProxyModel.h"

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
{
    m_statsTimer = new QTimer(this);
    m_statsTimer->setSingleShot(true);
    m_statsTimer->setInterval(kStatsRefreshMs);
    connect(m_statsTimer, &QTimer::timeout, this, &Ctr2ProxyModel::statsChanged);
    connect(m_proxy, &TcpByteProxy::statsChanged, this, [this] {
        if (!m_statsTimer->isActive()) {
            m_statsTimer->start();
        }
    });
    connect(m_proxy, &TcpByteProxy::stateChanged, this, [this] {
        emit stateChanged();
        emit statsChanged();
    });
    connect(m_proxy, &TcpByteProxy::endpointsChanged, this, &Ctr2ProxyModel::endpointsChanged);
    connect(m_proxy, &TcpByteProxy::lastErrorChanged, this, &Ctr2ProxyModel::lastErrorChanged);
    refreshListenAddresses();
}

Ctr2ProxyModel::~Ctr2ProxyModel()
{
    stop();
}

void Ctr2ProxyModel::refreshListenAddresses()
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
    if (choices == m_listenChoices) {
        return;
    }
    m_listenChoices = choices;
    emit listenAddressesChanged();
    emit configurationChanged();
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
    if (m_radioAddress.isEmpty()) {
        *problem = tr("Enter the radio's IPv4 address");
        return false;
    }
    if (!parseIpv4(m_radioAddress, &config->upstreamAddress)) {
        *problem = tr("Radio address must be an IPv4 address such as 192.168.1.50");
        return false;
    }
    if (!parsePort(m_radioPort, &config->upstreamPort)) {
        *problem = tr("Radio port must be 1-65535");
        return false;
    }
    const QString invalid = TcpByteProxy::validate(*config);
    if (!invalid.isEmpty()) {
        *problem = invalid;
        return false;
    }
    return true;
}

QString Ctr2ProxyModel::configurationProblem() const
{
    TcpByteProxy::Config config;
    QString problem;
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
    if (isRunning()) {
        return false;
    }
    TcpByteProxy::Config config;
    QString problem;
    if (!buildConfig(&config, &problem)) {
        return false;
    }
    const bool ok = m_proxy->start(config);
    emit configurationChanged();
    return ok;
}

void Ctr2ProxyModel::stop()
{
    if (m_proxy->state() == TcpByteProxy::State::Stopped) {
        return;
    }
    m_proxy->stop();
    emit configurationChanged();
}

QString Ctr2ProxyModel::peerEndpoint() const
{
    const TcpByteProxy::State s = state();
    if (s == TcpByteProxy::State::Connecting || s == TcpByteProxy::State::Relaying
        || s == TcpByteProxy::State::Closing) {
        return m_proxy->peerDescription();
    }
    return {};
}

} // namespace AetherSDR
