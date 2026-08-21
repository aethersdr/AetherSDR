#include "core/PskReporterClient.h"
#include "gui/map/MapPathGeometry.h"
#include "gui/map/SolarTerminator.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTimeZone>

#include <cmath>
#include <iostream>

using namespace AetherSDR;

namespace AetherSDR {

class PskReporterClientTestAccess {
public:
    static QString cacheFilePath(const PskReporterClient& client)
    {
        return client.cacheFilePath();
    }

    static void setHttpDiagnostics(PskReporterClient& client,
                                   qint64 lastRequestMs,
                                   qint64 nextAllowedMs,
                                   int status,
                                   const QString& error)
    {
        client.m_lastHttpRequestEpochMs = lastRequestMs;
        client.m_nextHttpAllowedEpochMs = nextAllowedMs;
        client.m_lastHttpStatus = status;
        client.m_lastHttpError = error;
    }

    static qint64 rateLimitBackoffMs(int count)
    {
        return PskReporterClient::rateLimitBackoffMs(count);
    }

    static void handleQueryReply(PskReporterClient& client,
                                 const QByteArray& xml,
                                 PskReporterClient::QueryScope responseScope)
    {
        client.handleQueryReply(xml, responseScope);
    }

    static void setInterval(PskReporterClient& client, int intervalMs)
    {
        client.m_intervalMs = intervalMs;
    }

    static PskReporterClient::QueryScope httpQueryScope(
        const PskReporterClient& client)
    {
        return client.httpQueryScope();
    }

    static void loadCache(PskReporterClient& client)
    {
        client.loadCache();
    }

    static int maxSpots()
    {
        return PskReporterClient::kMaxSpots;
    }

    static int maxMonitors()
    {
        return PskReporterClient::kMaxMonitors;
    }

    static qint64 maxHttpResponseBytes()
    {
        return PskReporterClient::kMaxHttpResponseBytes;
    }

    static bool appendHttpResponseChunk(QByteArray& response,
                                        const QByteArray& chunk)
    {
        return PskReporterClient::appendHttpResponseChunk(response, chunk);
    }

    static void restoreCachedMonitors(PskReporterClient& client,
                                      const QJsonArray& monitors)
    {
        client.restoreCachedMonitors(monitors);
    }

    static void discardPendingCacheWrite(PskReporterClient& client)
    {
        client.m_cacheDirty = false;
    }

    static void setCacheDirectory(PskReporterClient& client,
                                  const QString& directory)
    {
        client.m_cacheDirectoryOverride = directory;
    }
};

} // namespace AetherSDR

namespace {

bool check(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
    }
    return condition;
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(
        QStringLiteral("AetherSDR-tests"));
    QCoreApplication::setApplicationName(
        QStringLiteral("psk-reporter-map-behavior-test"));
    bool ok = true;

    PskReporterClient client;
    client.setCallsign(QStringLiteral("  k1abc  "));
    ok &= check(client.callsign() == QStringLiteral("K1ABC"),
                "callsign is normalized for a map-only query");
    ok &= check(client.queryScope()
                    == PskReporterClient::QueryScope::Callsign,
                "non-empty callsign selects callsign scope");

    client.setCallsign(QString());
    ok &= check(client.callsign().isEmpty(),
                "clearing the map query preserves an empty value");
    ok &= check(client.queryScope() == PskReporterClient::QueryScope::Anyone,
                "empty callsign selects anyone scope");
    const QString anyoneCache =
        PskReporterClientTestAccess::cacheFilePath(client);
    client.setCallsign(QStringLiteral("K1ABC"));
    const QString callsignCache =
        PskReporterClientTestAccess::cacheFilePath(client);
    client.setCallsign(QStringLiteral("W1AW"));
    const QString otherCallsignCache =
        PskReporterClientTestAccess::cacheFilePath(client);
    ok &= check(anyoneCache != callsignCache,
                "Anyone and callsign snapshots use separate cache files");
    ok &= check(callsignCache != otherCallsignCache,
                "each callsign snapshot has an independent cache file");

    const qint64 requestMs = QDateTime::currentMSecsSinceEpoch();
    PskReporterClientTestAccess::setHttpDiagnostics(
        client, requestMs, requestMs + PskReporterClient::kMinPollMs,
        429, QStringLiteral("Too Many Requests"));
    ok &= check(client.lastHttpRequestAt().toMSecsSinceEpoch() == requestMs,
                "HTTP diagnostics expose the last request time");
    ok &= check(client.nextHttpRequestAt().toMSecsSinceEpoch()
                    == requestMs + PskReporterClient::kMinPollMs,
                "HTTP diagnostics expose the throttle deadline");
    ok &= check(client.lastHttpStatus() == 429
                    && client.lastHttpError()
                           == QStringLiteral("Too Many Requests"),
                "HTTP diagnostics expose status and error details");
    ok &= check(PskReporterClientTestAccess::rateLimitBackoffMs(1)
                    == 15 * 60 * 1000,
                "first rate limit backs off for 15 minutes");
    ok &= check(PskReporterClientTestAccess::rateLimitBackoffMs(2)
                    == 30 * 60 * 1000,
                "repeated rate limits back off exponentially");
    ok &= check(PskReporterClientTestAccess::rateLimitBackoffMs(20)
                    == 2LL * 60 * 60 * 1000,
                "rate-limit backoff is capped at two hours");

    QByteArray boundedResponse(
        PskReporterClientTestAccess::maxHttpResponseBytes() - 1, 'x');
    ok &= check(PskReporterClientTestAccess::appendHttpResponseChunk(
                    boundedResponse, QByteArray(1, 'x'))
                    && boundedResponse.size()
                           == PskReporterClientTestAccess::maxHttpResponseBytes(),
                "HTTP response accepts bytes through the safety limit");
    ok &= check(!PskReporterClientTestAccess::appendHttpResponseChunk(
                    boundedResponse, QByteArray(1, 'x'))
                    && boundedResponse.size()
                           == PskReporterClientTestAccess::maxHttpResponseBytes(),
                "HTTP response rejects bytes beyond the safety limit");

    PskReporterClient parserClient;
    parserClient.setCallsign(QString());
    QByteArray xml("<receptionReports>");
    xml += "<activeReceiver callsign=' k1abc ' locator=' fn42 ' "
           "mode='FT8' decoderSoftware='Decoder 1' frequency='14074000'/>";
    xml += "<activeReceiver callsign='K1ABC' locator='FN42' mode='FT4'/>";
    xml += "<activeReceiver callsign='INVALID'/>";
    for (int i = 0; i < PskReporterClientTestAccess::maxMonitors() + 10; ++i) {
        xml += QStringLiteral(
            "<activeReceiver callsign='M%1' locator='L%1' mode='FT8'/>")
                   .arg(i).toUtf8();
    }
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    for (int i = 0; i < PskReporterClientTestAccess::maxSpots() + 1; ++i) {
        xml += QStringLiteral(
            "<receptionReport receiverCallsign='R%1' receiverLocator='FN42' "
            "senderCallsign='S%1' senderLocator='CN85' mode='FT8' "
            "frequency='14074000' flowStartSeconds='%2'/>")
                   .arg(i).arg(now).toUtf8();
    }
    xml += "</receptionReports>";
    PskReporterClientTestAccess::handleQueryReply(
        parserClient, xml, PskReporterClient::QueryScope::Anyone);
    ok &= check(parserClient.monitors().size()
                    == PskReporterClientTestAccess::maxMonitors(),
                "activeReceiver parsing enforces the monitor cap");
    ok &= check(parserClient.monitors().first().callsign
                    == QStringLiteral("K1ABC")
                    && parserClient.monitors().first().locator
                           == QStringLiteral("FN42")
                    && parserClient.monitors().first().frequencyHz == 14074000,
                "activeReceiver fields are normalized and parsed");
    int normalizedMonitorCount = 0;
    for (const PskReporterMonitor& monitor : parserClient.monitors()) {
        if (monitor.callsign == QStringLiteral("K1ABC")
            && monitor.locator == QStringLiteral("FN42")) {
            ++normalizedMonitorCount;
        }
    }
    ok &= check(normalizedMonitorCount == 1,
                "activeReceiver parsing deduplicates callsign and locator");
    ok &= check(parserClient.spots().size()
                    == PskReporterClientTestAccess::maxSpots()
                    && parserClient.resultsLimited(),
                "receptionReport parsing enforces and surfaces the request cap");

    PskReporterClient cacheClient;
    QJsonArray cachedMonitors;
    cachedMonitors.append(QJsonObject{
        {QStringLiteral("c"), QStringLiteral("K1ABC")},
        {QStringLiteral("l"), QStringLiteral("FN42")}});
    cachedMonitors.append(QJsonObject{
        {QStringLiteral("c"), QStringLiteral("W1AW")},
        {QStringLiteral("l"), QStringLiteral("FN31")}});
    cachedMonitors.append(QJsonObject{
        {QStringLiteral("c"), QStringLiteral("K1ABC")},
        {QStringLiteral("l"), QStringLiteral("FN42")}});
    PskReporterClientTestAccess::restoreCachedMonitors(
        cacheClient, cachedMonitors);
    const int cachedMonitorCount = cacheClient.monitors().size();
    PskReporterClientTestAccess::restoreCachedMonitors(
        cacheClient, cachedMonitors);
    ok &= check(cachedMonitorCount == 2
                    && cacheClient.monitors().size() == cachedMonitorCount,
                "reloading a cache does not duplicate active monitors");

    PskReporterClient liveClient;
    QTemporaryDir cacheDirectory;
    ok &= check(cacheDirectory.isValid(),
                "temporary cache directory is available");
    PskReporterClientTestAccess::setCacheDirectory(
        liveClient, cacheDirectory.path());
    liveClient.setCallsign(QStringLiteral("K1ABC"));
    PskReporterClientTestAccess::setInterval(
        liveClient, PskReporterClient::kLiveMqtt);
    ok &= check(PskReporterClientTestAccess::httpQueryScope(liveClient)
                    == PskReporterClient::QueryScope::Anyone,
                "live callsign mode reserves HTTP for the global snapshot");
    QByteArray globalXml("<receptionReports>");
    globalXml += QStringLiteral(
        "<activeReceiver callsign='W1AW' locator='FN31' mode='FT8'/>"
        "<receptionReport receiverCallsign='R1' receiverLocator='FN42' "
        "senderCallsign='K1ABC' senderLocator='CN85' mode='FT8' "
        "frequency='14074000' flowStartSeconds='%1'/>"
        "<receptionReport receiverCallsign='R2' receiverLocator='EM10' "
        "senderCallsign='W1AW' senderLocator='FN31' mode='FT8' "
        "frequency='14074000' flowStartSeconds='%1'/>"
        "</receptionReports>").arg(now).toUtf8();
    PskReporterClientTestAccess::handleQueryReply(
        liveClient, globalXml, PskReporterClient::QueryScope::Anyone);
    ok &= check(liveClient.spots().size() == 1
                    && liveClient.spots().first().senderCallsign
                           == QStringLiteral("K1ABC"),
                "global HTTP backfill seeds only the selected live callsign");
    liveClient.setCallsign(QString());
    PskReporterClientTestAccess::loadCache(liveClient);
    ok &= check(liveClient.spots().size() == 2
                    && liveClient.monitors().size() == 1,
                "clearing a live callsign immediately restores the cached "
                "global snapshot");
    PskReporterClientTestAccess::discardPendingCacheWrite(parserClient);

    constexpr double worldWidth = 360.0;
    ok &= check(std::abs(MapPathGeometry::unwrapX(
                            -179.0, 179.0, worldWidth) - 181.0) < 1e-9,
                "eastbound path stays continuous across the antimeridian");
    ok &= check(std::abs(MapPathGeometry::unwrapX(
                            179.0, -179.0, worldWidth) + 181.0) < 1e-9,
                "westbound path stays continuous across the antimeridian");
    ok &= check(std::abs(MapPathGeometry::unwrapX(
                            42.0, 40.0, worldWidth) - 42.0) < 1e-9,
                "ordinary path samples stay in their canonical world");

    const QDateTime marchEquinox(
        QDate(2026, 3, 20), QTime(14, 46), QTimeZone::UTC);
    const SolarTerminator::Position sun =
        SolarTerminator::positionAt(marchEquinox);
    ok &= check(std::abs(qRadiansToDegrees(sun.declinationRad)) < 0.5,
                "solar declination is near zero at the March equinox");
    const double subsolarLon = qRadiansToDegrees(sun.subsolarLonRad);
    ok &= check(!SolarTerminator::isNight(0.0, subsolarLon, sun),
                "subsolar point is in daylight");
    ok &= check(SolarTerminator::isNight(
                    0.0, SolarTerminator::normalizeDegrees(subsolarLon + 180.0),
                    sun),
                "antipode of subsolar point is at night");

    return ok ? 0 : 1;
}
