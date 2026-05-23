#include "WsjtxClient.h"
#include "LogManager.h"

#include <QDataStream>
#include <QNetworkInterface>
#include <QStandardPaths>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>

namespace MasterSDR {

WsjtxClient::WsjtxClient(QObject* parent)
    : QObject(parent)
{
    // Socket is created in initialize() on the SpotClients thread (#1929).
}

void WsjtxClient::initialize()
{
    if (m_socket) return;
    m_socket = new QUdpSocket(this);
    connect(m_socket, &QUdpSocket::readyRead, this, &WsjtxClient::onReadyRead);
}

WsjtxClient::~WsjtxClient()
{
    stopListening();
    m_logFile.close();
}

QString WsjtxClient::logFilePath() const
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
           + "/MasterSDR/wsjtx.log";
}

void WsjtxClient::startListening(const QString& address, quint16 port)
{
    if (m_listening) return;

    m_port = port;
    m_bindAddr = QHostAddress(address);
    m_isMulticast = m_bindAddr.isMulticast();

    qCDebug(lcDxCluster) << "WsjtxClient: binding to" << address << ":" << port
             << (m_isMulticast ? "(multicast)" : "(unicast)");

    if (!m_socket->bind(QHostAddress::AnyIPv4, port,
                        QAbstractSocket::ShareAddress | QAbstractSocket::ReuseAddressHint)) {
        qCWarning(lcDxCluster) << "WsjtxClient: bind failed:" << m_socket->errorString();
        return;
    }

    if (m_isMulticast) {
        if (!m_socket->joinMulticastGroup(m_bindAddr)) {
            qCWarning(lcDxCluster) << "WsjtxClient: joinMulticastGroup failed:" << m_socket->errorString();
            m_socket->close();
            return;
        }
    }

    // Open log file (truncate on each start)
    m_logFile.close();
    m_logFile.setFileName(logFilePath());
    QDir().mkpath(QFileInfo(m_logFile).absolutePath());
    if (m_logFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        m_logFile.write(QString("--- WSJT-X listener started at %1 on %2:%3 ---\n")
            .arg(QDateTime::currentDateTimeUtc().toString("yyyy-MM-dd HH:mm:ss UTC"),
                 address).arg(port).toUtf8());
        m_logFile.flush();
    }

    m_listening = true;
    qCDebug(lcDxCluster) << "WsjtxClient: listening on" << address << ":" << port;
    emit listening();
}

void WsjtxClient::stopListening()
{
    if (!m_listening) return;
    if (m_isMulticast)
        m_socket->leaveMulticastGroup(m_bindAddr);
    m_socket->close();
    m_listening = false;
    emit stopped();
}

// ── UDP read ────────────────────────────────────────────────────────────────

void WsjtxClient::onReadyRead()
{
    while (m_socket->hasPendingDatagrams()) {
        QByteArray data;
        data.resize(static_cast<int>(m_socket->pendingDatagramSize()));
        m_socket->readDatagram(data.data(), data.size());
        parseMessage(data);
    }
}

void WsjtxClient::parseMessage(const QByteArray& data)
{
    QDataStream ds(data);
    ds.setByteOrder(QDataStream::BigEndian);

    quint32 magic, schema, msgType;
    ds >> magic >> schema >> msgType;

    if (magic != WsjtxMagic) return;

    switch (msgType) {
    case 1:  parseStatus(ds);  break;  // Status — dial freq, mode
    case 2:  parseDecode(ds);  break;  // Decode — the spots
    default: break;
    }
}

// ── Status message (type 1) — track dial frequency ──────────────────────────

void WsjtxClient::parseStatus(QDataStream& ds)
{
    QString id;
    if (!readQString(ds, id)) return;

    quint64 dialFreq;
    ds >> dialFreq;  // dial frequency in Hz

    QString mode;
    if (!readQString(ds, mode)) return;

    m_dialFreqHz = static_cast<double>(dialFreq);
    m_mode = mode;

    emit statusReceived(id, m_dialFreqHz, mode);
}

// ── Decode message (type 2) — extract spots ─────────────────────────────────

void WsjtxClient::parseDecode(QDataStream& ds)
{
    // Fields: Id(QString), New(bool), Time(uint32), SNR(int32),
    //         DeltaTime(double), DeltaFreq(uint32), Mode(QString),
    //         Message(QString), LowConfidence(bool), OffAir(bool)

    QString id;
    if (!readQString(ds, id)) return;

    bool isNew;
    if (!readBool(ds, isNew)) return;
    if (!isNew) return;  // skip replayed decodes

    quint32 timeMs;
    qint32 snr;
    double deltaTime;
    quint32 deltaFreqHz;
    ds >> timeMs >> snr >> deltaTime >> deltaFreqHz;

    QString mode;
    if (!readQString(ds, mode)) return;

    QString message;
    if (!readQString(ds, message)) return;

    bool lowConfidence;
    if (!readBool(ds, lowConfidence)) return;
    // Skip low-confidence decodes
    if (lowConfidence) return;

    // Extract callsign from the decoded message
    QString call = extractCallsign(message);
    if (call.isEmpty()) return;

    // Calculate actual frequency: dial + audio offset
    double freqHz = m_dialFreqHz + deltaFreqHz;
    double freqMhz = freqHz / 1.0e6;

    // Build the spot
    DxSpot spot;
    spot.dxCall = call;
    spot.freqMhz = freqMhz;
    spot.spotterCall = "WSJT-X";
    spot.comment = message.trimmed();
    spot.utcTime = QTime::fromMSecsSinceStartOfDay(static_cast<int>(timeMs));
    spot.source = "WSJT-X";
    spot.snr = snr;

    // Log the decode
    QString logLine = QString("%1  %2  %3 kHz  %4 dB  %5")
        .arg(spot.utcTime.toString("HH:mm:ss"),
             call,
             QString::number(freqMhz * 1000.0, 'f', 1),
             QString::number(snr),
             message);
    if (m_logFile.isOpen()) {
        m_logFile.write((logLine + "\n").toUtf8());
        m_logFile.flush();
    }
    emit rawLineReceived(logLine);
    emit spotReceived(spot);
}

// ── Callsign extraction from WSJT-X message text ───────────────────────────

QString WsjtxClient::extractCallsign(const QString& message) const
{
    // WSJT-X message formats:
    //   "CQ W1AW FN42"           — CQ call, extract W1AW
    //   "CQ DX JA1ABC PM95"      — CQ DX, extract JA1ABC
    //   "CQ POTA K1ABC FN42"     — CQ directed, extract K1ABC
    //   "CQ NA W1AW FN42"        — CQ continent, extract W1AW
    //   "W1AW K1ABC +05"         — directed call, extract W1AW (first callsign)
    //   "W1AW K1ABC R-10"        — report, extract W1AW
    //   "W1AW K1ABC RR73"        — confirmation
    // We want to spot the OTHER station (not us). For CQ messages, that's the
    // caller. For directed messages, that's the first callsign.

    static const QRegularExpression callRx(R"(\b([A-Z0-9]{1,3}[0-9][A-Z0-9]{0,3}[A-Z])\b)");
    QStringList parts = message.trimmed().split(' ', Qt::SkipEmptyParts);

    if (parts.isEmpty()) return {};

    // CQ message: "CQ [modifier] CALLSIGN [GRID]"
    if (parts[0] == "CQ") {
        // Skip CQ and any modifier (DX, POTA, NA, SA, EU, AS, AF, OC, TEST, etc.)
        for (int i = 1; i < parts.size(); ++i) {
            auto m = callRx.match(parts[i]);
            if (m.hasMatch())
                return m.captured(1);
        }
        return {};
    }

    // Directed message: "MYCALL THEIRCALL report" — second word is who's calling
    if (parts.size() >= 2) {
        auto m = callRx.match(parts[1]);
        if (m.hasMatch())
            return m.captured(1);
    }

    return {};
}

// ── QDataStream helpers ─────────────────────────────────────────────────────

bool WsjtxClient::readQString(QDataStream& ds, QString& out)
{
    if (ds.atEnd()) return false;
    quint32 len;
    ds >> len;
    if (len == 0xFFFFFFFF) {
        out.clear();
        return true;
    }
    if (len > 10000) return false;  // sanity check
    QByteArray buf(static_cast<int>(len), '\0');
    if (ds.readRawData(buf.data(), static_cast<int>(len)) != static_cast<int>(len))
        return false;
    out = QString::fromUtf8(buf);
    return true;
}

bool WsjtxClient::readBool(QDataStream& ds, bool& out)
{
    if (ds.atEnd()) return false;
    quint8 v;
    ds >> v;
    out = (v != 0);
    return true;
}

} // namespace MasterSDR
