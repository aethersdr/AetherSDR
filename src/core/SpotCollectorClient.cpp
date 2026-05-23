#include "SpotCollectorClient.h"
#include "LogManager.h"

#include <QRegularExpression>
#include <QStandardPaths>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>

namespace MasterSDR {

SpotCollectorClient::SpotCollectorClient(QObject* parent)
    : QObject(parent)
{
    // Socket is created in initialize() on the SpotClients thread (#1929).
}

void SpotCollectorClient::initialize()
{
    if (m_socket) return;
    m_socket = new QUdpSocket(this);
    connect(m_socket, &QUdpSocket::readyRead, this, &SpotCollectorClient::onReadyRead);
}

SpotCollectorClient::~SpotCollectorClient()
{
    stopListening();
    m_logFile.close();
}

QString SpotCollectorClient::logFilePath() const
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
           + "/MasterSDR/spotcollector.log";
}

void SpotCollectorClient::startListening(quint16 port)
{
    if (m_listening) return;

    m_port = port;

    qCDebug(lcDxCluster) << "SpotCollectorClient: binding UDP port" << port;

    if (!m_socket->bind(QHostAddress::AnyIPv4, port,
                        QAbstractSocket::ShareAddress | QAbstractSocket::ReuseAddressHint)) {
        qCWarning(lcDxCluster) << "SpotCollectorClient: bind failed:" << m_socket->errorString();
        return;
    }

    // Open log file (truncate on each start)
    m_logFile.close();
    m_logFile.setFileName(logFilePath());
    QDir().mkpath(QFileInfo(m_logFile).absolutePath());
    if (m_logFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        m_logFile.write(QString("--- SpotCollector listener started at %1 on port %2 ---\n")
            .arg(QDateTime::currentDateTimeUtc().toString("yyyy-MM-dd HH:mm:ss UTC"))
            .arg(port).toUtf8());
        m_logFile.flush();
    }

    m_listening = true;
    qCDebug(lcDxCluster) << "SpotCollectorClient: listening on port" << port;
    emit listening();
}

void SpotCollectorClient::stopListening()
{
    if (!m_listening) return;
    m_socket->close();
    m_listening = false;
    emit stopped();
}

// ── UDP read ────────────────────────────────────────────────────────────────

void SpotCollectorClient::onReadyRead()
{
    while (m_socket->hasPendingDatagrams()) {
        QByteArray data;
        data.resize(static_cast<int>(m_socket->pendingDatagramSize()));
        m_socket->readDatagram(data.data(), data.size());

        // SpotCollector sends one or more "DX de" lines per datagram
        QString payload = QString::fromLatin1(data);
        const QStringList lines = payload.split('\n', Qt::SkipEmptyParts);
        for (const QString& raw : lines) {
            QString line = raw.trimmed();
            if (line.isEmpty()) continue;

            if (m_logFile.isOpen()) {
                m_logFile.write((line + "\n").toUtf8());
                m_logFile.flush();
            }
            emit rawLineReceived(line);

            DxSpot spot;
            if (parseDxSpotLine(line, spot)) {
                spot.source = "SpotCollector";
                qCDebug(lcDxCluster) << "SpotCollectorClient: spot" << spot.dxCall
                         << spot.freqMhz << "MHz de" << spot.spotterCall;
                emit spotReceived(spot);
            }
        }
    }
}

// ── DX spot line parser ─────────────────────────────────────────────────────

bool SpotCollectorClient::parseDxSpotLine(const QString& line, DxSpot& spot) const
{
    // Standard format: DX de W3LPL:     14025.0  JA1ABC       CW big signal       1824Z
    static const QRegularExpression rx(
        R"(^DX\s+de\s+(\S+?):\s+(\d+\.?\d*)\s+(\S+)\s+(.*?)\s+(\d{4})Z)",
        QRegularExpression::CaseInsensitiveOption);

    auto match = rx.match(line);
    if (!match.hasMatch())
        return false;

    spot.spotterCall = match.captured(1);
    double freqKhz   = match.captured(2).toDouble();
    spot.freqMhz     = freqKhz / 1000.0;
    spot.dxCall       = match.captured(3);
    spot.comment      = match.captured(4).trimmed();

    QString timeStr = match.captured(5);
    int hh = timeStr.left(2).toInt();
    int mm = timeStr.mid(2, 2).toInt();
    spot.utcTime = QTime(hh, mm);

    return spot.freqMhz > 0.0 && !spot.dxCall.isEmpty();
}

} // namespace MasterSDR
