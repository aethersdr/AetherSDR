#include "MapProviderNetworkAccessManager.h"

#include <QMutexLocker>
#include <QCoreApplication>
#include <QAbstractNetworkCache>
#include <QLocale>
#include <QTimeZone>
#include <QNetworkReply>
#include <QRandomGenerator>
#include <QTimer>

#include <algorithm>

namespace AetherSDR {
namespace {
constexpr qint64 kMinimumRetryMs = 60000;
constexpr qint64 kMaximumBackoffMs = 15 * 60000;
// Bound malformed server values while allowing longer-than-backoff cooldowns.
constexpr qint64 kMaximumDelayMs = 24 * 60 * 60000;

class CooldownReply final : public QNetworkReply {
public:
    CooldownReply(const QNetworkRequest& request, QNetworkAccessManager::Operation operation,
                  qint64 delayMs, QObject* parent) : QNetworkReply(parent)
    {
        setRequest(request);
        setUrl(request.url());
        setOperation(operation);
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, 503);
        setRawHeader("Retry-After", QByteArray::number((delayMs + 999) / 1000));
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
        QTimer::singleShot(0, this, [this] {
            finish(QNetworkReply::TemporaryNetworkFailureError,
                   QCoreApplication::translate("MapProviderNetworkAccessManager",
                       "Map provider cooling down; cached imagery remains available"));
        });
    }
    void abort() override
    {
        finish(OperationCanceledError,
               QCoreApplication::translate("MapProviderNetworkAccessManager", "Canceled"));
    }
protected:
    qint64 readData(char*, qint64) override { return -1; }
private:
    void finish(NetworkError error, const QString& message)
    {
        if (isFinished()) {
            return;
        }
        setError(error, message);
        setFinished(true);
        emit errorOccurred(error);
        emit finished();
    }
};

std::shared_ptr<MapProviderRetryPolicy> sharedPolicy()
{
    static const auto policy = std::make_shared<MapProviderRetryPolicy>();
    return policy;
}
}

MapProviderRetryPolicy::MapProviderRetryPolicy(std::function<qint64()> clock)
    : m_clock(std::move(clock))
{
    m_elapsed.start();
}

qint64 MapProviderRetryPolicy::now() const
{
    return m_clock ? m_clock() : m_elapsed.elapsed();
}

QString MapProviderRetryPolicy::provider(const QUrl& url)
{
    const QString host = url.host().toLower();
    if (host == QStringLiteral("geo.weather.gc.ca") || host == QStringLiteral("api.meteogate.eu")
        || host == QStringLiteral("s3.waw3-1.cloudferro.com") || host == QStringLiteral("api.librewxr.net")) { return host; }
    if (host == QLatin1String("gibs.earthdata.nasa.gov")
        || host == QLatin1String("gibs-a.earthdata.nasa.gov")
        || host == QLatin1String("gibs-b.earthdata.nasa.gov")
        || host == QLatin1String("gibs-c.earthdata.nasa.gov")) {
        return QStringLiteral("nasa-gibs");
    }
    if (host == QLatin1String("mapservices.weather.noaa.gov")) {
        return QStringLiteral("nws-imagery");
    }
    return {};
}

qint64 MapProviderRetryPolicy::retryAfterMs(const QByteArray& value, const QDateTime& now)
{
    const QByteArray text = value.trimmed();
    if (text.isEmpty()) {
        return 0;
    }
    if (std::all_of(text.cbegin(), text.cend(), [](char c) { return c >= '0' && c <= '9'; })) {
        bool ok = false;
        const qulonglong seconds = text.toULongLong(&ok);
        if (!ok || seconds > qulonglong(kMaximumDelayMs / 1000)) {
            return kMaximumDelayMs;
        }
        return qint64(seconds) * 1000;
    }
    // HTTP uses IMF-fixdate (literal GMT); Qt's RFC2822 parser expects a
    // numeric offset on some versions. Parse the wire format in English/UTC.
    QDateTime date = QLocale::c().toDateTime(QString::fromLatin1(text),
                                           QStringLiteral("ddd, dd MMM yyyy HH:mm:ss 'GMT'"));
    if (date.isValid()) {
        date.setTimeZone(QTimeZone::UTC);
    } else {
        date = QDateTime::fromString(QString::fromLatin1(text), Qt::RFC2822Date);
    }
    return date.isValid() ? std::clamp(now.msecsTo(date), qint64(0), kMaximumDelayMs) : 0;
}

MapProviderRetryPolicy::Admission MapProviderRetryPolicy::admit(const QUrl& url)
{
    const QString key = provider(url);
    if (key.isEmpty()) {
        return {};
    }
    QMutexLocker lock(&m_mutex);
    State& state = m_states[key];
    const qint64 remaining = state.until - now();
    if (remaining > 0) {
        return {remaining, state.generation, false};
    }
    if (state.probeInFlight) {
        return {kMinimumRetryMs, state.generation, false};
    }
    const bool probe = state.failures != 0;
    state.probeInFlight = probe;
    return {0, state.generation, probe};
}

void MapProviderRetryPolicy::complete(const QUrl& url, Admission admission,
    bool failed, bool canceled, const QByteArray& retryAfter, const QDateTime& wallNow)
{
    const QString key = provider(url);
    if (key.isEmpty()) {
        return;
    }
    QMutexLocker lock(&m_mutex);
    State& state = m_states[key];
    const qint64 time = now();
    if (admission.probe && admission.generation == state.generation) {
        state.probeInFlight = false;
    }
    if (canceled) {
        return; // Panning, disabling or closing is not a provider failure.
    }
    if (!failed) {
        if (admission.generation == state.generation && time >= state.until) {
            state.failures = 0;
        }
        return; // An older in-flight success cannot erase a newer failure.
    }
    if (time >= state.until) {
        state.failures = std::min(state.failures + 1, 5U);
        ++state.generation;
        state.probeInFlight = false; // A newer failure invalidates any older probe.
        const qint64 backoff = std::min(kMaximumBackoffMs,
            kMinimumRetryMs * (qint64(1) << (state.failures - 1)));
        // Positive jitter never undercuts NWS's one-minute outage guidance.
        const qint64 jitter = QRandomGenerator::global()->bounded(6001);
        state.until = time + backoff + jitter;
    }
    // Even an older failed request may carry a newer server deadline. Honor it
    // conservatively (within the one-day cap); only successes are generation-gated.
    state.until = std::max(state.until, time + retryAfterMs(retryAfter, wallNow));
}

MapProviderNetworkAccessManager::MapProviderNetworkAccessManager(QObject* parent,
    std::shared_ptr<MapProviderRetryPolicy> policy)
    : QNetworkAccessManager(parent), m_policy(policy ? std::move(policy) : sharedPolicy())
{
}

QNetworkReply* MapProviderNetworkAccessManager::sendRequest(Operation operation,
    const QNetworkRequest& request, QIODevice* outgoingData)
{
    return QNetworkAccessManager::createRequest(operation, request, outgoingData);
}

QNetworkReply* MapProviderNetworkAccessManager::createRequest(Operation operation,
    const QNetworkRequest& request, QIODevice* outgoingData)
{
    if (operation == GetOperation && (request.url().scheme() == QStringLiteral("libre-radar") || request.url().scheme() == QStringLiteral("opera-radar") || request.url().scheme() == QStringLiteral("radar-composite")) && radarRequestHandler) {
        if (QNetworkReply* reply = radarRequestHandler(request, this)) { return reply; }
    }
    if (operation != GetOperation || MapProviderRetryPolicy::provider(request.url()).isEmpty()) {
        return sendRequest(operation, request, outgoingData);
    }
    const MapProviderRetryPolicy::Admission admission = m_policy->admit(request.url());
    if (admission.delayMs > 0) {
        const int control = request.attribute(QNetworkRequest::CacheLoadControlAttribute,
                                              QNetworkRequest::PreferNetwork).toInt();
        if (cache() != nullptr && control != QNetworkRequest::AlwaysNetwork
            && cache()->metaData(request.url()).isValid()) {
            // PreferCache can go to the wire on a miss. AlwaysCache cannot,
            // even if the entry disappears between this lookup and Qt's read.
            QNetworkRequest cached(request);
            cached.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                                QNetworkRequest::AlwaysCache);
            return sendRequest(operation, cached, outgoingData);
        }
        return new CooldownReply(request, operation, admission.delayMs, this);
    }
    QNetworkReply* reply = sendRequest(operation, request, outgoingData);
    const auto policy = m_policy;
    const QUrl url = request.url();
    const auto completed = std::make_shared<bool>(false);
    connect(reply, &QNetworkReply::finished, reply, [reply, policy, url, admission, completed] {
        *completed = true;
        const bool canceled = reply->error() == QNetworkReply::OperationCanceledError;
        const bool failed = reply->error() != QNetworkReply::NoError
            || reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() >= 400;
        // A disk-cache hit is not evidence that the remote service recovered.
        const bool cachedSuccess = !failed
            && reply->attribute(QNetworkRequest::SourceIsFromCacheAttribute).toBool();
        policy->complete(url, admission, failed, canceled || cachedSuccess,
                         reply->rawHeader("Retry-After"));
    });
    connect(reply, &QObject::destroyed, [policy, url, admission, completed] {
        if (!*completed) {
            policy->complete(url, admission, false, true);
        }
    });
    return reply;
}

} // namespace AetherSDR
