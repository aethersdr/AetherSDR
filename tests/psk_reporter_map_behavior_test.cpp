#include "core/PskReporterClient.h"
#include "gui/map/MapPathGeometry.h"
#include "gui/map/SolarTerminator.h"

#include <QCoreApplication>
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
