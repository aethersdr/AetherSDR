#include "EibiClient.h"

#include "AppSettings.h"
#include "LogManager.h"
#include "EibiCodeMaps.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QHash>
#include <QNetworkRequest>
#include <QScopedPointer>
#include <QStandardPaths>
#include <QRegularExpression>

namespace {
// Stall timeout for the EiBi schedule download (#4688 §6).
constexpr int kTransferTimeoutMs = 30000;
} // namespace

namespace AetherSDR {

QString EibiClient::lookupCountry(const QString& code)
{
    if (code.isEmpty()) return QString();

    const auto& map = getEibiCountryMap();
    auto it = map.find(code);
    if (it != map.end()) {
        return it.value();
    }
    return code;
}

QString EibiClient::lookupLanguage(const QString& code)
{
    if (code.isEmpty()) return QString();

    const auto& map = getEibiLangMap();
    auto it = map.find(code);
    if (it != map.end()) {
        return it.value();
    }
    return code;
}

QString EibiClient::lookupTarget(const QString& code)
{
    if (code.isEmpty()) return QString();

    static const QHash<QString, QString> kTargetMap = {
        {QStringLiteral("Af"),   QStringLiteral("Africa")},
        {QStringLiteral("Am"),   QStringLiteral("Americas")},
        {QStringLiteral("As"),   QStringLiteral("Asia")},
        {QStringLiteral("Car"),  QStringLiteral("Caribbean")},
        {QStringLiteral("Cau"),  QStringLiteral("Caucasus")},
        {QStringLiteral("CIS"),  QStringLiteral("Commonwealth of Independent States")},
        {QStringLiteral("CNA"),  QStringLiteral("Central North America")},
        {QStringLiteral("ENA"),  QStringLiteral("Eastern North America")},
        {QStringLiteral("WNA"),  QStringLiteral("Western North America")},
        {QStringLiteral("Eu"),   QStringLiteral("Europe")},
        {QStringLiteral("EEu"),  QStringLiteral("Eastern Europe")},
        {QStringLiteral("WEu"),  QStringLiteral("Western Europe")},
        {QStringLiteral("SEu"),  QStringLiteral("Southern Europe")},
        {QStringLiteral("NEu"),  QStringLiteral("Northern Europe")},
        {QStringLiteral("CEu"),  QStringLiteral("Central Europe")},
        {QStringLiteral("FE"),   QStringLiteral("Far East")},
        {QStringLiteral("Glo"),  QStringLiteral("Global")},
        {QStringLiteral("In"),   QStringLiteral("Indian Subcontinent")},
        {QStringLiteral("LAm"),  QStringLiteral("Latin America")},
        {QStringLiteral("NAm"),  QStringLiteral("North America")},
        {QStringLiteral("SAm"),  QStringLiteral("South America")},
        {QStringLiteral("CAm"),  QStringLiteral("Central America")},
        {QStringLiteral("ME"),   QStringLiteral("Middle East")},
        {QStringLiteral("NAO"),  QStringLiteral("North Atlantic Ocean")},
        {QStringLiteral("SAO"),  QStringLiteral("South Atlantic Ocean")},
        {QStringLiteral("WIO"),  QStringLiteral("Western Indian Ocean")},
        {QStringLiteral("Oc"),   QStringLiteral("Oceania / Pacific")},
        {QStringLiteral("Pac"),  QStringLiteral("Pacific Ocean")},
        {QStringLiteral("SAs"),  QStringLiteral("South Asia")},
        {QStringLiteral("CAs"),  QStringLiteral("Central Asia")},
        {QStringLiteral("EAs"),  QStringLiteral("East Asia")},
        {QStringLiteral("SEAs"), QStringLiteral("Southeast Asia")},
        {QStringLiteral("SEA"),  QStringLiteral("Southeast Asia")},
        {QStringLiteral("SEE"),  QStringLiteral("Southeast Europe")},
        {QStringLiteral("Sib"),  QStringLiteral("Siberia")},
        {QStringLiteral("Tib"),  QStringLiteral("Tibet")},
        {QStringLiteral("NAf"),  QStringLiteral("North Africa")},
        {QStringLiteral("SAf"),  QStringLiteral("South Africa")},
        {QStringLiteral("WAf"),  QStringLiteral("West Africa")},
        {QStringLiteral("EAf"),  QStringLiteral("East Africa")},
        {QStringLiteral("CAf"),  QStringLiteral("Central Africa")}
    };

    auto it = kTargetMap.find(code);
    if (it != kTargetMap.end()) {
        return it.value();
    }

    const QString country = lookupCountry(code);
    if (country != code) {
        return country;
    }

    return code;
}

QString EibiClient::lookupSite(const QString& countryCode, const QString& siteCode)
{
    if (siteCode.isEmpty()) return QString();

    const auto& map = getEibiSiteMap();
    static const QRegularExpression relayRx(QStringLiteral("^/([A-Z0-9]{1,4})(?:-([a-z0-9\\-]{1,5}))?$"));
    const auto match = relayRx.match(siteCode);
    if (match.hasMatch()) {
        const QString hostItu = match.captured(1);
        const QString hostSite = match.captured(2);
        const QString hostCountryName = lookupCountry(hostItu);
        if (!hostSite.isEmpty()) {
            const QString key = hostItu + QLatin1Char(':') + hostSite;
            auto it = map.find(key);
            if (it != map.end()) {
                return QStringLiteral("Relay: ") + hostCountryName + QStringLiteral(" (") + it.value() + QLatin1Char(')');
            }
            return QStringLiteral("Relay: ") + hostCountryName + QStringLiteral(" (") + hostSite + QLatin1Char(')');
        }
        return QStringLiteral("Relay: ") + hostCountryName;
    }

    const QString key = countryCode + QLatin1Char(':') + siteCode;
    auto it = map.find(key);
    if (it != map.end()) {
        return it.value();
    }

    return siteCode;
}

QString EibiClient::formatDays(const QString& daysCode)
{
    if (daysCode.isEmpty()) return QStringLiteral("Daily");

    // Check numerical days string (e.g. "1245")
    bool allDigits = true;
    for (const QChar& ch : daysCode) {
        if (!ch.isDigit()) {
            allDigits = false;
            break;
        }
    }

    if (allDigits && !daysCode.isEmpty()) {
        static const char* kDayNames[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
        QStringList names;
        for (const QChar& ch : daysCode) {
            const int val = ch.digitValue();
            if (val >= 1 && val <= 7) {
                names << QString::fromLatin1(kDayNames[val - 1]);
            }
        }
        if (!names.isEmpty()) {
            return names.join(QStringLiteral(", "));
        }
    }

    static const QHash<QString, QString> kDayMap = {
        {QStringLiteral("Mo-Fr"), QStringLiteral("Mon - Fri")},
        {QStringLiteral("Mo-Sa"), QStringLiteral("Mon - Sat")},
        {QStringLiteral("SaSu"),  QStringLiteral("Sat & Sun")},
        {QStringLiteral("Mo"),    QStringLiteral("Mondays")},
        {QStringLiteral("Tu"),    QStringLiteral("Tuesdays")},
        {QStringLiteral("We"),    QStringLiteral("Wednesdays")},
        {QStringLiteral("Th"),    QStringLiteral("Thursdays")},
        {QStringLiteral("Fr"),    QStringLiteral("Fridays")},
        {QStringLiteral("Sa"),    QStringLiteral("Saturdays")},
        {QStringLiteral("Su"),    QStringLiteral("Sundays")},
        {QStringLiteral("We-Mo"), QStringLiteral("Wed - Mon")},
        {QStringLiteral("1.Sa"),  QStringLiteral("1st Saturday of month")},
        {QStringLiteral("1.Su"),  QStringLiteral("1st Sunday of month")},
        {QStringLiteral("Last7"), QStringLiteral("Last Sunday of month")},
        {QStringLiteral("irr"),   QStringLiteral("Irregular schedule")},
        {QStringLiteral("alt"),   QStringLiteral("Alternative frequency")},
        {QStringLiteral("test"),  QStringLiteral("Test transmission")},
        {QStringLiteral("Ram"),   QStringLiteral("Ramadan special schedule")},
        {QStringLiteral("Haj"),   QStringLiteral("Hajj special broadcast")},
        {QStringLiteral("LSB"),   QStringLiteral("Lower Sideband (LSB)")},
        {QStringLiteral("USB"),   QStringLiteral("Upper Sideband (USB)")}
    };

    auto it = kDayMap.find(daysCode);
    if (it != kDayMap.end()) {
        return it.value();
    }

    return daysCode;
}

QString EibiClient::formatTime(int startMins, int endMins)
{
    if (startMins == 0 && endMins == 1440) {
        return QStringLiteral("24 Hours (00:00 - 24:00 UTC)");
    }

    const int startH = startMins / 60;
    const int startM = startMins % 60;
    const int endH   = endMins / 60;
    const int endM   = endMins % 60;

    QString timeStr = QStringLiteral("%1:%2 - %3:%4 UTC")
                          .arg(startH, 2, 10, QLatin1Char('0'))
                          .arg(startM, 2, 10, QLatin1Char('0'))
                          .arg(endH, 2, 10, QLatin1Char('0'))
                          .arg(endM, 2, 10, QLatin1Char('0'));

    if (startMins > endMins) {
        timeStr += QStringLiteral(" (Overnight)");
    }

    return timeStr;
}

QString EibiClient::cacheFilePath()
{
    QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    const QFileInfo fi(base);
    if (fi.fileName() == fi.dir().dirName()) {
        base = fi.absolutePath(); // macOS double-segment collapse
    }
    return base + QStringLiteral("/eibi.txt");
}

EibiClient::EibiClient(QObject* parent)
    : QObject(parent)
{
}

EibiClient::~EibiClient()
{
    if (m_updateTimer) {
        m_updateTimer->stop();
    }
}

void EibiClient::initialize()
{
    if (m_nam) return; // already initialized

    m_nam = new QNetworkAccessManager(this);
    m_nam->setTransferTimeout(kTransferTimeoutMs);
    connect(m_nam, &QNetworkAccessManager::finished,
            this, &EibiClient::onDownloadFinished);

    m_updateTimer = new QTimer(this);
    m_updateTimer->setInterval(60000); // 60s periodic update
    connect(m_updateTimer, &QTimer::timeout,
            this, &EibiClient::updateActiveSpots);

    // If auto-start enabled in settings at startup, activate
    const bool savedEnabled =
        AppSettings::instance().value("EiBiAutoStart",
        AppSettings::instance().value("EiBiSpotsEnabled", "False")).toString() == "True";
    if (savedEnabled) {
        setEnabled(true);
    }
}

void EibiClient::setEnabled(bool enabled)
{
    m_enabled = enabled;

    if (!m_nam) {
        return;
    }

    if (enabled) {
        const QString cachePath = cacheFilePath();
        const QFileInfo info(cachePath);
        const qint64 nowSec = QDateTime::currentSecsSinceEpoch();
        const qint64 fileAgeSec = info.exists() ? (nowSec - info.lastModified().toSecsSinceEpoch()) : (kMaxCacheAgeSec + 1);

        qCInfo(lcDxCluster) << "EiBi shortwave spots enabled. Local cache:" << cachePath
                            << (info.exists() ? QString("file age: %1 days").arg(fileAgeSec / 86400.0, 0, 'f', 1) : QString("file missing"));

        if (!info.exists() || fileAgeSec >= kMaxCacheAgeSec) {
            checkForUpdates();
        } else {
            loadAndParseCache();
            updateActiveSpots();
        }

        if (!m_updateTimer->isActive()) {
            m_updateTimer->start();
        }
    } else {
        qCInfo(lcDxCluster) << "EiBi shortwave spots disabled.";
        if (m_updateTimer->isActive()) {
            m_updateTimer->stop();
        }
        m_entries.clear();
        emit spotsUpdated({});
    }
}

QDateTime EibiClient::cacheLastModified() const
{
    const QFileInfo info(cacheFilePath());
    if (info.exists()) {
        return info.lastModified().toUTC();
    }
    return {};
}

QDateTime EibiClient::nextFetchTime() const
{
    const QDateTime lastMod = cacheLastModified();
    if (lastMod.isValid()) {
        return lastMod.addSecs(kMaxCacheAgeSec);
    }
    return {};
}

void EibiClient::forceUpdate()
{
    m_enabled = true;
    if (!m_nam) {
        return;
    }
    if (m_updateTimer && !m_updateTimer->isActive()) {
        m_updateTimer->start();
    }
    qCInfo(lcDxCluster) << "EiBi force update requested by user.";
    checkForUpdates(true);
}

void EibiClient::checkForUpdates(bool forceRefresh)
{
    if (m_isDownloading || !m_enabled || !m_nam) return;

    m_isDownloading = true;

    QNetworkRequest req((QUrl(QString::fromLatin1(kSourceUrl))));
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("AetherSDR/26.8 (EiBi Schedule Downloader)"));

    // Include If-Modified-Since header if local cache file exists and not a force refresh
    const QString cachePath = cacheFilePath();
    const QFileInfo cacheInfo(cachePath);
    if (!forceRefresh && cacheInfo.exists()) {
        const QDateTime lastMod = cacheInfo.lastModified().toUTC();
        qCInfo(lcDxCluster) << "Checking EiBi schedule updates at" << kSourceUrl
                            << "with If-Modified-Since:" << lastMod.toString(Qt::ISODateWithMs);
        req.setHeader(QNetworkRequest::IfModifiedSinceHeader, lastMod);
    } else {
        qCInfo(lcDxCluster) << "Downloading EiBi schedule from" << kSourceUrl
                            << (forceRefresh ? "(force refresh)" : "(no local cache file found)");
    }

    QNetworkReply* reply = m_nam->get(req);
    connect(reply, &QNetworkReply::metaDataChanged, reply, [reply] {
        const qint64 contentLength = reply->header(QNetworkRequest::ContentLengthHeader).toLongLong();
        if (contentLength > kMaxDownloadBytes) {
            qCWarning(lcDxCluster) << "EiBi schedule Content-Length header (" << contentLength << "bytes) exceeds size limit — aborting reply";
            reply->abort();
        }
    });
    connect(reply, &QNetworkReply::downloadProgress, reply, [reply](qint64 bytesReceived, qint64 bytesTotal) {
        if (bytesReceived > kMaxDownloadBytes || bytesTotal > kMaxDownloadBytes) {
            qCWarning(lcDxCluster) << "EiBi schedule download exceeded size limit (" << bytesReceived << "bytes) — aborting reply";
            reply->abort();
        }
    });
}

void EibiClient::onDownloadFinished(QNetworkReply* reply)
{
    m_isDownloading = false;
    QScopedPointer<QNetworkReply, QScopedPointerDeleteLater> replyGuard(reply);

    const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    // Handle HTTP 304 Not Modified: server file date has NOT changed
    if (httpStatus == 304) {
        qCInfo(lcDxCluster) << "EiBi schedule server file unchanged (HTTP 304 Not Modified). Re-using local cache:" << cacheFilePath();
        const QString cachePath = cacheFilePath();
        if (QFile::exists(cachePath)) {
            QFile file(cachePath);
            if (file.open(QIODevice::ReadWrite)) {
                file.setFileTime(QDateTime::currentDateTime(), QFileDevice::FileModificationTime);
                file.close();
            }
        }
        loadAndParseCache();
        updateActiveSpots();
        return;
    }

    if (reply->error() != QNetworkReply::NoError) {
        const QString errStr = reply->errorString();
        qCWarning(lcDxCluster) << "EiBi schedule download failed (HTTP" << httpStatus << "):" << errStr;
        emit fetchFailed(errStr);
        return;
    }

    const QByteArray data = reply->readAll();
    if (data.size() > kMaxDownloadBytes) {
        const QString errStr = QStringLiteral("Downloaded EiBi schedule file exceeds size limit (%1 bytes)").arg(data.size());
        qCWarning(lcDxCluster) << errStr;
        emit fetchFailed(errStr);
        return;
    }

    const QString content = QString::fromLatin1(data);
    if (!parseContent(content)) {
        const QString errStr = QStringLiteral("Failed to parse downloaded EiBi schedule format.");
        qCWarning(lcDxCluster) << errStr;
        emit fetchFailed(errStr);
        return;
    }

    // Save to cache file atomically (Principle XIV - Persist Atomically)
    const QString cachePath = cacheFilePath();
    const QFileInfo fi(cachePath);
    QDir().mkpath(fi.absolutePath());

    QSaveFile file(cachePath);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(data);
        if (file.commit()) {
            qCInfo(lcDxCluster) << "Successfully downloaded and cached EiBi schedule ("
                                << data.size() << "bytes," << m_entries.size() << "entries parsed) to" << cachePath;
        } else {
            qCWarning(lcDxCluster) << "Failed to commit EiBi schedule cache file:" << cachePath;
        }
    } else {
        qCWarning(lcDxCluster) << "Failed to open EiBi schedule cache file for writing:" << cachePath;
    }

    updateActiveSpots();
}

void EibiClient::loadAndParseCache()
{
    const QString cachePath = cacheFilePath();
    QFile file(cachePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qCWarning(lcDxCluster) << "Could not open EiBi cache file for reading:" << cachePath;
        return;
    }

    const QString content = QString::fromLatin1(file.readAll());
    file.close();

    if (parseContent(content)) {
        qCInfo(lcDxCluster) << "Loaded EiBi schedule from cache:" << cachePath << "(" << m_entries.size() << "entries parsed)";
    } else {
        qCWarning(lcDxCluster) << "Failed to parse cached EiBi schedule:" << cachePath;
    }
}

bool EibiClient::parseContent(const QString& content)
{
    QVector<EibiEntry> newEntries;
    newEntries.reserve(20000);

    const QStringList lines = content.split(QLatin1Char('\n'));
    bool inDataSection = false;

    static const QRegularExpression timeRx(QStringLiteral("^(\\d{2})(\\d{2})-(\\d{2})(\\d{2})$"));

    for (int i = 0; i < lines.size(); ++i) {
        const QString line = lines.at(i);

        if (!inDataSection) {
            if (line.startsWith(QStringLiteral("===")) && i > 0 && lines.at(i - 1).startsWith(QStringLiteral("kHz"))) {
                inDataSection = true;
            }
            continue;
        }

        if (line.trimmed().isEmpty()) continue;
        if (line.length() < 35) continue;

        // Clean mid slicing - QString::mid clamps safely when length exceeds string length
        const QString khzStr  = line.mid(0, 14).trimmed();
        const QString timeStr = line.mid(14, 10).trimmed();
        const QString daysStr = line.mid(24, 6).trimmed();
        const QString ituStr  = line.mid(30, 4).trimmed();
        const QString stnStr  = line.mid(34, 23).trimmed();
        const QString langStr = line.mid(57, 6).trimmed();
        const QString tgtStr  = line.mid(63, 9).trimmed();
        const QString remStr  = line.mid(72).trimmed();

        bool ok = false;
        const double freqKhz = khzStr.toDouble(&ok);
        if (!ok || freqKhz <= 0.0) continue;

        const auto match = timeRx.match(timeStr);
        if (!match.hasMatch()) continue;

        const int startH = match.captured(1).toInt();
        const int startM = match.captured(2).toInt();
        const int endH   = match.captured(3).toInt();
        const int endM   = match.captured(4).toInt();

        EibiEntry entry;
        entry.freqMhz   = freqKhz / 1000.0;
        entry.startMins = startH * 60 + startM;
        entry.endMins   = endH * 60 + endM;
        entry.days      = daysStr;
        entry.itu       = ituStr;
        entry.station   = stnStr;
        entry.lang      = langStr;
        entry.target    = tgtStr;
        entry.remarks   = remStr;

        newEntries.append(entry);
    }

    if (newEntries.isEmpty()) {
        return false;
    }

    m_entries = std::move(newEntries);
    return true;
}

bool EibiClient::isDayActive(const QString& daysStr, int dayOfWeek) const
{
    if (daysStr.isEmpty()) return true;

    // Numerical days 1..7 (e.g. "1245")
    bool hasDigit = false;
    for (const QChar& ch : daysStr) {
        if (ch.isDigit()) {
            hasDigit = true;
            if (ch.digitValue() == dayOfWeek) return true;
        }
    }
    if (hasDigit) return false;

    // Standard day range abbreviations
    if (daysStr == QStringLiteral("Mo-Fr")) return dayOfWeek >= 1 && dayOfWeek <= 5;
    if (daysStr == QStringLiteral("Mo-Sa")) return dayOfWeek >= 1 && dayOfWeek <= 6;
    if (daysStr == QStringLiteral("SaSu"))  return dayOfWeek == 6 || dayOfWeek == 7;
    if (daysStr == QStringLiteral("Mo"))    return dayOfWeek == 1;
    if (daysStr == QStringLiteral("Tu"))    return dayOfWeek == 2;
    if (daysStr == QStringLiteral("We"))    return dayOfWeek == 3;
    if (daysStr == QStringLiteral("Th"))    return dayOfWeek == 4;
    if (daysStr == QStringLiteral("Fr"))    return dayOfWeek == 5;
    if (daysStr == QStringLiteral("Sa"))    return dayOfWeek == 6;
    if (daysStr == QStringLiteral("Su"))    return dayOfWeek == 7;
    if (daysStr == QStringLiteral("We-Mo")) return dayOfWeek != 2; // Wed through Mon

    // Fallback for special codes like "irr", "alt", etc. -> treat as active
    return true;
}

bool EibiClient::isEntryActive(const EibiEntry& entry, const QDateTime& utcNow) const
{
    if (!isDayActive(entry.days, utcNow.date().dayOfWeek())) {
        return false;
    }

    const int curMins = utcNow.time().hour() * 60 + utcNow.time().minute();

    if (entry.startMins <= entry.endMins) {
        return (curMins >= entry.startMins && curMins <= entry.endMins);
    } else {
        // Midnight wrap e.g. 2230-0200
        return (curMins >= entry.startMins || curMins <= entry.endMins);
    }
}

void EibiClient::updateActiveSpots()
{
    if (!m_enabled) return;

    // Check if cache expired after 7 days during long sessions
    const QString cachePath = cacheFilePath();
    const QFileInfo cacheInfo(cachePath);
    if (cacheInfo.exists()) {
        const qint64 nowSec = QDateTime::currentSecsSinceEpoch();
        const qint64 fileAgeSec = nowSec - cacheInfo.lastModified().toSecsSinceEpoch();
        if (fileAgeSec >= kMaxCacheAgeSec && !m_isDownloading) {
            checkForUpdates();
        }
    }

    if (m_entries.isEmpty()) {
        emit spotsUpdated({});
        return;
    }

    const QDateTime utcNow = QDateTime::currentDateTimeUtc();
    const QTime time = utcNow.time();

    QVector<DxSpot> activeSpots;
    activeSpots.reserve(m_entries.size() / 4);

    for (const auto& entry : m_entries) {
        if (!isEntryActive(entry, utcNow)) continue;

        DxSpot spot;
        spot.dxCall      = entry.station;
        spot.freqMhz     = entry.freqMhz;
        spot.source      = QStringLiteral("EiBi");
        spot.spotterCall = QStringLiteral("EiBi");
        spot.utcTime     = time;
        spot.lifetimeSec = 3600;

        // Structured comment encoding with human-readable schedule, country, language, target, and site names
        QStringList parts;
        const QString timeStr = formatTime(entry.startMins, entry.endMins);
        const QString daysStr = formatDays(entry.days);
        const QString cName   = lookupCountry(entry.itu);
        const QString lName   = lookupLanguage(entry.lang);
        const QString tName   = lookupTarget(entry.target);
        const QString sName   = lookupSite(entry.itu, entry.remarks);

        if (!timeStr.isEmpty())        parts << QStringLiteral("Schedule: ") + timeStr;
        if (!daysStr.isEmpty())        parts << QStringLiteral("Days: ") + daysStr;
        if (!cName.isEmpty())          parts << QStringLiteral("Origin: ") + cName;
        if (!lName.isEmpty())          parts << QStringLiteral("Lang: ") + lName;
        if (!tName.isEmpty())          parts << QStringLiteral("Target: ") + tName;
        if (!sName.isEmpty())          parts << QStringLiteral("Site: ") + sName;

        spot.comment = parts.join(QStringLiteral(" | "));

        activeSpots.append(spot);
    }

    qCDebug(lcDxCluster) << "Updated active EiBi spots:" << activeSpots.size() << "spots active at UTC" << time.toString("HH:mm:ss");

    emit spotsUpdated(activeSpots);
}

} // namespace AetherSDR
