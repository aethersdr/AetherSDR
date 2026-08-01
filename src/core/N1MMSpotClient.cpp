#include "N1MMSpotClient.h"
#include "LogManager.h"

#include <QStandardPaths>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>

namespace AetherSDR {

N1MMSpotClient::N1MMSpotClient(QObject* parent)
    : QObject(parent)
{
    // Socket is created in initialize() on the SpotClients thread (#1929).
}

void N1MMSpotClient::initialize()
{
    if (m_socket) return;
    m_socket = new QUdpSocket(this);
    connect(m_socket, &QUdpSocket::readyRead, this, &N1MMSpotClient::onReadyRead);
}

N1MMSpotClient::~N1MMSpotClient()
{
    stopListening();
    m_logFile.close();
}

QString N1MMSpotClient::logFilePath() const
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
           + "/AetherSDR/spothub/n1mmspot.log";
}

void N1MMSpotClient::startListening(quint16 port)
{
    if (m_listening) return;

    // initialize() is normally queued onto the SpotClients thread at startup,
    // but don't assume it has run — construct the socket here if not, so an
    // early start can't null-deref. Safe either way: both entry points are
    // invoked on this object's own thread, which is where the socket must be
    // created (#1929).
    if (!m_socket)
        initialize();

    m_port = port;

    qCDebug(lcDxCluster) << "N1MMSpotClient: binding UDP port" << port;

    if (!m_socket->bind(QHostAddress::AnyIPv4, port,
                        QAbstractSocket::ShareAddress | QAbstractSocket::ReuseAddressHint)) {
        qCWarning(lcDxCluster) << "N1MMSpotClient: bind failed:" << m_socket->errorString();
        emit bindFailed(m_socket->errorString());
        return;
    }

    // Open log file (truncate on each start)
    m_logFile.close();
    m_logFile.setFileName(logFilePath());
    QDir().mkpath(QFileInfo(m_logFile).absolutePath());
    if (m_logFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        m_logFile.write(QString("--- N1MMSpot listener started at %1 on port %2 ---\n")
            .arg(QDateTime::currentDateTimeUtc().toString("yyyy-MM-dd HH:mm:ss UTC"))
            .arg(port).toUtf8());
        m_logFile.flush();
    }

    m_listening = true;
    qCDebug(lcDxCluster) << "N1MMSpotClient: listening on port" << port;
    emit listening();
}

void N1MMSpotClient::stopListening()
{
    if (!m_listening) return;
    m_socket->close();
    m_listening = false;
    emit stopped();
}

// ── UDP read ────────────────────────────────────────────────────────────────

void N1MMSpotClient::onReadyRead()
{
    while (m_socket->hasPendingDatagrams()) {
        // N1MM/DXLog send one <spot> document per datagram; cap the read at a
        // generous size so a malformed/oversized datagram can't allocate
        // unbounded memory.
        const qint64 pending = m_socket->pendingDatagramSize();
        QByteArray data;
        data.resize(static_cast<int>(qMin<qint64>(pending, 64 * 1024)));
        const qint64 got = m_socket->readDatagram(data.data(), data.size());
        if (got < 0) continue;
        data.resize(static_cast<int>(got));

        const QString xml = QString::fromUtf8(data);
        if (m_logFile.isOpen()) {
            m_logFile.write((xml + "\n").toUtf8());
            m_logFile.flush();
        }
        emit rawPacketReceived(xml);

        N1mmSpot spot;
        QString action;
        if (!N1MMSpotParser::parsePacket(data, spot, action)) {
            qCDebug(lcDxCluster) << "N1MMSpotClient: ignoring unparseable packet";
            continue;
        }

        if (action == "delete") {
            qCDebug(lcDxCluster) << "N1MMSpotClient: delete" << spot.dxCall << spot.freqMhz << "MHz";
            emit spotDeleted(spot.dxCall, spot.freqMhz);
        } else {
            qCDebug(lcDxCluster) << "N1MMSpotClient: spot" << spot.dxCall
                     << spot.freqMhz << "MHz status" << spot.statusRaw;
            emit spotAdded(spot);
        }
    }
}

} // namespace AetherSDR
