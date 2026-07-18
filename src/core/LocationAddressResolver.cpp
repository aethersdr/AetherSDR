#include "LocationAddressResolver.h"

#include "LogManager.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrlQuery>

#include <cmath>

namespace AetherSDR {

namespace {

constexpr int kMinimumRequestIntervalMs = 1000;
constexpr int kRequestTimeoutMs = 10000;
constexpr qint64 kMaximumResponseBytes = 256 * 1024;
constexpr int kMaximumAddressCharacters = 1024;
constexpr const char* kDefaultBaseUrl = "https://nominatim.openstreetmap.org/reverse";

bool validCoordinate(double latitude, double longitude)
{
    return std::isfinite(latitude) && std::isfinite(longitude)
        && latitude >= -90.0 && latitude <= 90.0
        && longitude >= -180.0 && longitude <= 180.0;
}

} // namespace

LocationAddressResolver::LocationAddressResolver(QObject* parent)
    : QObject(parent)
    , m_network(this)
{
    m_rateLimitTimer.setSingleShot(true);
    connect(&m_rateLimitTimer, &QTimer::timeout,
            this, &LocationAddressResolver::schedulePending);
}

QString LocationAddressResolver::cacheKey(double latitude, double longitude)
{
    // Four decimals are roughly 11 m of latitude: precise enough to avoid a
    // visibly wrong nearby address while coalescing normal stationary GPS
    // jitter into a single lookup.
    return QStringLiteral("%1,%2")
        .arg(latitude, 0, 'f', 4)
        .arg(longitude, 0, 'f', 4);
}

void LocationAddressResolver::resolve(double latitude, double longitude)
{
    if (!validCoordinate(latitude, longitude)) {
        emit failed(latitude, longitude, tr("Invalid GPS coordinates"));
        return;
    }

    const QString key = cacheKey(latitude, longitude);
    const auto cached = m_cache.constFind(key);
    if (cached != m_cache.cend()) {
        emit resolved(latitude, longitude, cached.value());
        return;
    }

    m_pendingLatitude = latitude;
    m_pendingLongitude = longitude;
    m_hasPending = true;

    if (m_reply != nullptr) {
        return;
    }

    if (m_lastRequest.isValid()
        && m_lastRequest.elapsed() < kMinimumRequestIntervalMs) {
        const int remaining = kMinimumRequestIntervalMs
            - static_cast<int>(m_lastRequest.elapsed());
        m_rateLimitTimer.start(remaining);
        return;
    }

    schedulePending();
}

void LocationAddressResolver::schedulePending()
{
    if (!m_hasPending || m_reply != nullptr) {
        return;
    }
    const double latitude = m_pendingLatitude;
    const double longitude = m_pendingLongitude;
    m_hasPending = false;
    startRequest(latitude, longitude);
}

void LocationAddressResolver::startRequest(double latitude, double longitude)
{
    QString baseUrl = qEnvironmentVariable("AETHER_NOMINATIM_BASE_URL").trimmed();
    if (baseUrl.isEmpty()) {
        baseUrl = QString::fromLatin1(kDefaultBaseUrl);
    }

    QUrl url(baseUrl);
    if (!url.isValid() || url.scheme() != QLatin1String("https")) {
        emit failed(latitude, longitude,
                    tr("Reverse-geocoding service URL must use HTTPS"));
        return;
    }

    QUrlQuery query(url);
    query.addQueryItem(QStringLiteral("format"), QStringLiteral("jsonv2"));
    query.addQueryItem(QStringLiteral("lat"), QString::number(latitude, 'f', 7));
    query.addQueryItem(QStringLiteral("lon"), QString::number(longitude, 'f', 7));
    query.addQueryItem(QStringLiteral("zoom"), QStringLiteral("18"));
    query.addQueryItem(QStringLiteral("addressdetails"), QStringLiteral("1"));
    query.addQueryItem(QStringLiteral("layer"), QStringLiteral("address"));
    url.setQuery(query);

    QNetworkRequest request(url);
    request.setRawHeader(
        "User-Agent",
        QStringLiteral("AetherSDR/%1 (https://github.com/aethersdr/AetherSDR)")
            .arg(QCoreApplication::applicationVersion())
            .toUtf8());
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("Accept-Language", qgetenv("LANG"));
    request.setTransferTimeout(kRequestTimeoutMs);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);

    m_lastRequest.restart();
    QNetworkReply* reply = m_network.get(request);
    m_reply = reply;
    reply->setProperty("gpsLatitude", latitude);
    reply->setProperty("gpsLongitude", longitude);

    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        const double latitude = reply->property("gpsLatitude").toDouble();
        const double longitude = reply->property("gpsLongitude").toDouble();
        if (m_reply == reply) {
            m_reply = nullptr;
        }

        QString failure;
        const QVariant contentLength = reply->header(QNetworkRequest::ContentLengthHeader);
        if (contentLength.isValid()
            && contentLength.toLongLong() > kMaximumResponseBytes) {
            failure = tr("Address response was unexpectedly large");
        } else if (reply->error() != QNetworkReply::NoError) {
            failure = tr("Address lookup failed: %1").arg(reply->errorString());
        }

        QByteArray payload;
        if (failure.isEmpty()) {
            payload = reply->read(kMaximumResponseBytes + 1);
            if (payload.size() > kMaximumResponseBytes) {
                failure = tr("Address response was unexpectedly large");
            }
        }

        if (failure.isEmpty()) {
            const QString address = parseDisplayName(payload);
            if (address.isEmpty()) {
                failure = tr("No mapped address was found for this location");
            } else {
                m_cache.insert(cacheKey(latitude, longitude), address);
                emit resolved(latitude, longitude, address);
            }
        }

        if (!failure.isEmpty()) {
            qCWarning(lcGui) << "GPS address lookup:" << failure;
            emit failed(latitude, longitude, failure);
        }

        reply->deleteLater();
        if (m_hasPending) {
            resolve(m_pendingLatitude, m_pendingLongitude);
        }
    });
}

QString LocationAddressResolver::parseDisplayName(const QByteArray& payload)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return {};
    }

    QString address = document.object().value(QStringLiteral("display_name")).toString();
    address = address.simplified();
    if (address.isEmpty()) {
        return {};
    }
    if (address.size() > kMaximumAddressCharacters) {
        address.truncate(kMaximumAddressCharacters);
    }
    return address;
}

} // namespace AetherSDR
