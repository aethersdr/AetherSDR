#include "PotaClient.h"
#include "LogManager.h"
#include "AppSettings.h"

#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkReply>
#include <QStandardPaths>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>

namespace MasterSDR {

PotaClient::PotaClient(QObject* parent)
    : QObject(parent)
{
    // QNAM and poll timer are created in initialize() on the SpotClients thread (#1929).
}

void PotaClient::initialize()
{
    if (m_nam) return;
    m_nam = new QNetworkAccessManager(this);
    m_pollTimer = new QTimer(this);
    m_pollTimer->setSingleShot(false);
    connect(m_pollTimer, &QTimer::timeout, this, &PotaClient::onPollTimer);
}

PotaClient::~PotaClient()
{
    stopPolling();
    m_logFile.close();
}

QString PotaClient::logFilePath() const
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
           + "/MasterSDR/pota.log";
}

void PotaClient::startPolling(int intervalSec)
{
    if (m_polling) return;

    qCDebug(lcDxCluster) << "PotaClient: starting polling every" << intervalSec << "sec";

    // Open log file (truncate on each start)
    m_logFile.close();
    m_logFile.setFileName(logFilePath());
    QDir().mkpath(QFileInfo(m_logFile).absolutePath());
    if (m_logFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        m_logFile.write(QString("--- POTA polling started at %1 (every %2s) ---\n")
            .arg(QDateTime::currentDateTimeUtc().toString("yyyy-MM-dd HH:mm:ss UTC"))
            .arg(intervalSec).toUtf8());
        m_logFile.flush();
    }

    m_seenSpotIds.clear();
    m_polling = true;
    m_pollTimer->start(intervalSec * 1000);
    onPollTimer();  // immediate first poll
    emit started();
}

void PotaClient::stopPolling()
{
    if (!m_polling) return;
    m_pollTimer->stop();
    m_polling = false;
    emit stopped();
}

void PotaClient::onPollTimer()
{
    QNetworkRequest req{QUrl{ApiUrl}};
    req.setHeader(QNetworkRequest::UserAgentHeader, "MasterSDR");
    auto* reply = m_nam->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            QString err = reply->errorString();
            qCWarning(lcDxCluster) << "PotaClient: poll failed:" << err;
            emit pollError(err);
            return;
        }

        QByteArray data = reply->readAll();
        QJsonDocument doc = QJsonDocument::fromJson(data);
        if (!doc.isArray()) {
            emit pollError("Invalid JSON response");
            return;
        }

        QJsonArray arr = doc.array();
        int total = arr.size();
        int newCount = 0;

        for (const auto& val : arr) {
            QJsonObject obj = val.toObject();

            int spotId = obj.value("spotId").toInt();
            if (m_seenSpotIds.contains(spotId))
                continue;
            m_seenSpotIds.insert(spotId);
            newCount++;

            DxSpot spot;
            spot.dxCall      = obj.value("activator").toString();
            spot.spotterCall = obj.value("spotter").toString();
            spot.source      = "POTA";

            // Frequency in kHz → MHz
            QString freqStr = obj.value("frequency").toString();
            spot.freqMhz = freqStr.toDouble() / 1000.0;

            // Use API expire field for lifetime (seconds remaining)
            int expire = obj.value("expire").toInt();
            spot.lifetimeSec = (expire > 0) ? expire : 600;

            // Apply POTA spot color (#RRGGBB → #FFRRGGBB for radio)
            QString potaColor = AppSettings::instance().value("PotaSpotColor", "#FFFF00").toString();
            if (potaColor.length() == 7)
                potaColor = "#FF" + potaColor.mid(1);
            spot.color = potaColor;

            // Build comment: park reference + park name + mode
            QString ref  = obj.value("reference").toString();
            QString park = obj.value("name").toString();
            QString mode = obj.value("mode").toString();
            spot.comment = ref;
            if (!park.isEmpty())
                spot.comment += " " + park;
            if (!mode.isEmpty())
                spot.comment += " " + mode;

            // Parse spot time
            QString timeStr = obj.value("spotTime").toString();
            QDateTime dt = QDateTime::fromString(timeStr, Qt::ISODate);
            if (dt.isValid())
                spot.utcTime = dt.toUTC().time();
            else
                spot.utcTime = QDateTime::currentDateTimeUtc().time();

            if (spot.freqMhz <= 0.0 || spot.dxCall.isEmpty())
                continue;

            // Log
            QString logLine = QString("%1  %2  %3 kHz  %4  %5")
                .arg(spot.utcTime.toString("HH:mm"),
                     spot.dxCall,
                     QString::number(spot.freqMhz * 1000.0, 'f', 1),
                     ref, mode);
            if (m_logFile.isOpen()) {
                m_logFile.write((logLine + "\n").toUtf8());
                m_logFile.flush();
            }
            emit rawLineReceived(logLine);
            emit spotReceived(spot);
        }

        emit pollComplete(total, newCount);
    });
}

} // namespace MasterSDR
