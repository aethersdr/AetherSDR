#include "DiscoveryModel.h"

#include <QDateTime>
#include <QNetworkDatagram>

#ifdef Q_OS_ANDROID
#include <QJniObject>
#include <QCoreApplication>
#endif

namespace {

constexpr quint16 kDiscoveryPort = 4992;
constexpr int kStaleMs = 15000;

#ifdef Q_OS_ANDROID
// Android drops UDP broadcast unless the app holds a WifiManager
// MulticastLock. Acquire one for the process lifetime (spike-grade).
void acquireMulticastLock()
{
    static QJniObject lock;
    if (lock.isValid())
        return;
    QJniObject context = QNativeInterface::QAndroidApplication::context();
    QJniObject wifi = context.callObjectMethod(
        "getSystemService",
        "(Ljava/lang/String;)Ljava/lang/Object;",
        QJniObject::fromString("wifi").object<jstring>());
    if (!wifi.isValid())
        return;
    lock = wifi.callObjectMethod(
        "createMulticastLock",
        "(Ljava/lang/String;)Landroid/net/wifi/WifiManager$MulticastLock;",
        QJniObject::fromString("aethercompanion-discovery").object<jstring>());
    if (lock.isValid())
        lock.callMethod<void>("acquire");
}
#endif

} // namespace

DiscoveryModel::DiscoveryModel(QObject* parent)
    : QAbstractListModel(parent)
{
    connect(&m_socket, &QUdpSocket::readyRead, this, &DiscoveryModel::onReadyRead);
    m_staleTimer.setInterval(5000);
    connect(&m_staleTimer, &QTimer::timeout, this, &DiscoveryModel::onStaleSweep);
}

int DiscoveryModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_radios.size());
}

QVariant DiscoveryModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= static_cast<int>(m_radios.size()))
        return {};
    const SpikeRadio& r = m_radios[static_cast<size_t>(index.row())];
    switch (role) {
    case SerialRole:   return r.serial;
    case RadioModelRole: return r.model;
    case NicknameRole: return r.nickname;
    case CallsignRole: return r.callsign;
    case VersionRole:  return r.version;
    case StatusRole:   return r.status;
    case AddressRole:  return r.address.toString();
    }
    return {};
}

QHash<int, QByteArray> DiscoveryModel::roleNames() const
{
    return {
        {SerialRole, "serial"},
        {RadioModelRole, "radioModel"},
        {NicknameRole, "nickname"},
        {CallsignRole, "callsign"},
        {VersionRole, "version"},
        {StatusRole, "status"},
        {AddressRole, "address"},
    };
}

void DiscoveryModel::start()
{
    if (m_listening)
        return;
#ifdef Q_OS_ANDROID
    acquireMulticastLock();
#endif
    m_listening = m_socket.bind(QHostAddress::AnyIPv4, kDiscoveryPort,
                                QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint);
    if (m_listening)
        m_staleTimer.start();
    emit listeningChanged();
}

void DiscoveryModel::onReadyRead()
{
    while (m_socket.hasPendingDatagrams()) {
        const QNetworkDatagram datagram = m_socket.receiveDatagram();
        SpikeRadio radio;
        // Datagram is (possibly VITA-framed) text; tokens without '=' are
        // skipped, so parsing the raw bytes as UTF-8 key=value pairs works.
        const QString text = QString::fromUtf8(datagram.data()).trimmed();
        for (const QString& token : text.split(' ', Qt::SkipEmptyParts)) {
            const int eq = token.indexOf('=');
            if (eq <= 0)
                continue;
            const QString key = token.left(eq).toLower();
            QString value = token.mid(eq + 1);
            value.remove(QChar('\0'));
            if (key == "serial")        radio.serial = value;
            else if (key == "model")    radio.model = value;
            else if (key == "nickname") radio.nickname = value;
            else if (key == "callsign") radio.callsign = value;
            else if (key == "version")  radio.version = value;
            else if (key == "status")   radio.status = value;
            else if (key == "ip")       radio.address = QHostAddress(value);
        }
        if (radio.serial.isEmpty())
            continue;
        if (radio.address.isNull())
            radio.address = datagram.senderAddress();
        radio.lastSeenMs = QDateTime::currentMSecsSinceEpoch();
        upsert(std::move(radio));
    }
}

void DiscoveryModel::upsert(SpikeRadio radio)
{
    for (size_t i = 0; i < m_radios.size(); ++i) {
        if (m_radios[i].serial == radio.serial) {
            m_radios[i] = std::move(radio);
            const QModelIndex idx = index(static_cast<int>(i));
            emit dataChanged(idx, idx);
            return;
        }
    }
    beginInsertRows({}, static_cast<int>(m_radios.size()),
                    static_cast<int>(m_radios.size()));
    m_radios.push_back(std::move(radio));
    endInsertRows();
}

void DiscoveryModel::onStaleSweep()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (int i = static_cast<int>(m_radios.size()) - 1; i >= 0; --i) {
        if (now - m_radios[static_cast<size_t>(i)].lastSeenMs > kStaleMs) {
            beginRemoveRows({}, i, i);
            m_radios.erase(m_radios.begin() + i);
            endRemoveRows();
        }
    }
}
