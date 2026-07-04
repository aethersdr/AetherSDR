#include "CallsignLookupService.h"

#include "CallsignUtils.h"
#include "CtyDatParser.h"
#include "LogManager.h"
#include "MaidenheadLocator.h"
#include "QrzClient.h"
#include "QrzLookupSettings.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStandardPaths>
#include <QUrl>

#ifdef HAVE_KEYCHAIN
#include <qt6keychain/keychain.h>
#endif

namespace AetherSDR {

namespace {
constexpr const char* kKeychainService = "AetherSDR";
constexpr const char* kKeychainKey     = "qrz_password";
constexpr int  kSaveDebounceMs   = 2000;
constexpr int  kMaxCacheEntries  = 1000;   // prune oldest beyond this
constexpr int  kPhotoTimeoutMs   = 20000;
} // namespace

CallsignLookupService& CallsignLookupService::instance()
{
    static CallsignLookupService s;
    return s;
}

CallsignLookupService::CallsignLookupService(QObject* parent)
    : QObject(parent)
{
    m_client = new QrzClient(this);

    connect(m_client, &QrzClient::lookupSucceeded, this,
            [this](const QString& call, const CallsignInfo& info) {
        // Key on the queried form (call), not QRZ's base info.call — they
        // differ for portable/prefixed queries, and keying on info.call leaks
        // the in-flight entry (dead lookup) and misses the cache (#3990).
        m_inFlight.remove(call);
        m_fallbackShown.remove(call);
        m_cache.insert(call, info);
        m_cacheDirty = true;
        scheduleCacheSave();
        if (info.call == m_ownCallsign)
            maybeAdoptOwnLocation(info);
        CallsignInfo out = info;
        stampGeo(out);
        emit infoReady(out, /*fromCache=*/false);
        fetchPhoto(info);
    });

    connect(m_client, &QrzClient::lookupFailed, this,
            [this](const QString& call, QrzClient::Error, const QString& message) {
        m_inFlight.remove(call);
        // Serve a stale cache entry rather than nothing — a 9-day-old
        // name and city beat an error toast when the network is down.
        const auto it = m_cache.constFind(call);
        if (it != m_cache.constEnd()) {
            qCDebug(lcQrz) << "lookup failed for" << call << "— serving stale cache entry";
            m_fallbackShown.remove(call);
            CallsignInfo out = it.value();
            stampGeo(out);
            emit infoReady(out, /*fromCache=*/true);
            return;
        }
        // No cache either → country-level card from cty.dat prefix data.
        if (emitPrefixFallback(call))
            return;
        emit lookupFailed(call, message);
    });

    connect(m_client, &QrzClient::loginTestFinished,
            this, &CallsignLookupService::loginTestFinished);

    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(kSaveDebounceMs);
    connect(&m_saveTimer, &QTimer::timeout, this, &CallsignLookupService::saveCacheNow);

    reloadConfiguration();
}

CallsignLookupService::~CallsignLookupService()
{
    saveCacheNow();
}

bool CallsignLookupService::hasCredentials() const
{
    return m_client->hasCredentials();
}

void CallsignLookupService::reloadConfiguration()
{
    m_enabled = QrzLookupSettings::enabled();
    loadPasswordFromKeychain();
}

void CallsignLookupService::loadPasswordFromKeychain()
{
    const QString username = QrzLookupSettings::username();
    if (username.isEmpty()) {
        m_client->setCredentials({}, {});
        emit configurationChanged();
        return;
    }
#ifdef HAVE_KEYCHAIN
    auto* job = new QKeychain::ReadPasswordJob(QLatin1String(kKeychainService));
    job->setAutoDelete(true);
    job->setKey(QLatin1String(kKeychainKey));
    connect(job, &QKeychain::Job::finished, this, [this, username](QKeychain::Job* j) {
        auto* readJob = static_cast<QKeychain::ReadPasswordJob*>(j);
        if (j->error() != QKeychain::NoError || readJob->textData().isEmpty()) {
            qCDebug(lcQrz) << "no stored QRZ password";
            m_client->setCredentials(username, {});
        } else {
            m_client->setCredentials(username, readJob->textData());
        }
        emit configurationChanged();
    });
    job->start();
#else
    qCWarning(lcQrz) << "QRZ password persistence unavailable: built without QtKeychain";
    m_client->setCredentials(username, {});
    emit configurationChanged();
#endif
}

void CallsignLookupService::savePassword(const QString& password)
{
#ifdef HAVE_KEYCHAIN
    if (password.isEmpty()) {
        auto* job = new QKeychain::DeletePasswordJob(QLatin1String(kKeychainService));
        job->setAutoDelete(true);
        job->setKey(QLatin1String(kKeychainKey));
        connect(job, &QKeychain::Job::finished, this, [this](QKeychain::Job*) {
            loadPasswordFromKeychain();
        });
        job->start();
        return;
    }
    auto* job = new QKeychain::WritePasswordJob(QLatin1String(kKeychainService));
    job->setAutoDelete(true);
    job->setKey(QLatin1String(kKeychainKey));
    job->setTextData(password);
    connect(job, &QKeychain::Job::finished, this, [this](QKeychain::Job* j) {
        if (j->error() != QKeychain::NoError)
            qCWarning(lcQrz) << "keychain save failed:" << j->errorString();
        loadPasswordFromKeychain();
    });
    job->start();
#else
    Q_UNUSED(password);
    qCWarning(lcQrz) << "cannot save QRZ password: built without QtKeychain";
#endif
}

void CallsignLookupService::readPassword(std::function<void(const QString&)> callback)
{
#ifdef HAVE_KEYCHAIN
    auto* job = new QKeychain::ReadPasswordJob(QLatin1String(kKeychainService));
    job->setAutoDelete(true);
    job->setKey(QLatin1String(kKeychainKey));
    connect(job, &QKeychain::Job::finished, this,
            [callback = std::move(callback)](QKeychain::Job* j) {
        auto* readJob = static_cast<QKeychain::ReadPasswordJob*>(j);
        callback(j->error() == QKeychain::NoError ? readJob->textData() : QString());
    });
    job->start();
#else
    callback({});
#endif
}

void CallsignLookupService::lookup(const QString& call, bool forceRefresh)
{
    const QString norm = Callsigns::normalized(call);
    if (norm.isEmpty())
        return;
    if (!m_enabled) {
        emit lookupFailed(norm, QStringLiteral("QRZ lookups are disabled"));
        return;
    }

    ensureCacheLoaded();

    const auto it = m_cache.constFind(norm);
    const bool fresh = it != m_cache.constEnd() && !it.value().isOlderThan(kCacheTtlSec);
    if (fresh && !forceRefresh) {
        CallsignInfo info = it.value();
        stampGeo(info);
        // Queued so callers connecting right before lookup() still get it.
        QMetaObject::invokeMethod(this, [this, info] {
            emit infoReady(info, /*fromCache=*/true);
            const QString photo = photoPathFor(info.call);
            if (!photo.isEmpty())
                emit photoReady(info.call, photo);
            else
                fetchPhoto(info);
        }, Qt::QueuedConnection);
        return;
    }

    if (m_inFlight.contains(norm))
        return;  // coalesce — a busy net pileup is one request
    m_inFlight.insert(norm);
    m_fallbackShown.remove(norm);   // new attempt → fallback allowed again
    qCDebug(lcQrz) << "QRZ lookup" << norm << (fresh ? "(forced refresh)" : "(cache miss/stale)");

    // Slow-network stopgap: if QRZ hasn't answered in kPrefixFallbackMs,
    // show the country-level prefix card now; the full card replaces it
    // whenever the real answer lands.
    QTimer::singleShot(kPrefixFallbackMs, this, [this, norm] {
        if (m_inFlight.contains(norm))
            emitPrefixFallback(norm);
    });

    m_client->lookup(norm);
}

bool CallsignLookupService::canResolve(const QString& call)
{
    if (hasCredentials() || hasCachedEntry(call))
        return true;
    return m_ctyParser && m_ctyParser->entityForCallsign(Callsigns::normalized(call));
}

void CallsignLookupService::setOwnLocation(double lat, double lon)
{
    m_ownLat = lat;
    m_ownLon = lon;
    m_hasOwnLocation = true;
    m_ownLocationFromGps = true;
}

void CallsignLookupService::setOwnCallsign(const QString& call)
{
    const QString norm = Callsigns::normalized(call);
    if (norm.isEmpty() || norm == m_ownCallsign)
        return;
    m_ownCallsign = norm;
    if (m_ownLocationFromGps)
        return;  // GPS already supplied a better position

    ensureCacheLoaded();
    const auto it = m_cache.constFind(norm);
    if (it != m_cache.constEnd()) {
        maybeAdoptOwnLocation(it.value());
        return;
    }
    // Not cached — fetch quietly; the lookupSucceeded path adopts it.
    // (No card is pending for this call, so nothing pops visually.)
    if (m_enabled && hasCredentials())
        lookup(norm);
}

void CallsignLookupService::maybeAdoptOwnLocation(const CallsignInfo& info)
{
    if (m_ownLocationFromGps || info.prefixOnly)
        return;  // GPS wins; a country center is useless as a home position
    double lat = 0.0, lon = 0.0;
    if (info.hasLatLon) {
        lat = info.latitude;
        lon = info.longitude;
    } else if (info.grid.isEmpty()
               || !MaidenheadLocator::toLatLon(info.grid, lat, lon)) {
        return;
    }
    m_ownLat = lat;
    m_ownLon = lon;
    m_hasOwnLocation = true;
    qCDebug(lcQrz) << "own position adopted from" << info.call << "QRZ record";
}

CallsignInfo CallsignLookupService::prefixInfo(const QString& call) const
{
    CallsignInfo info;
    if (!m_ctyParser)
        return info;
    const DxccEntity* entity = m_ctyParser->entityForCallsign(call);
    if (!entity)
        return info;
    info.call       = call;
    info.country    = entity->name;
    info.continent  = entity->continent;
    info.cqZone     = entity->cqZone;
    info.prefixOnly = true;
    if (entity->hasLatLon) {
        info.latitude  = entity->latitude;
        info.longitude = entity->longitude;
        info.hasLatLon = true;
    }
    info.fetchedUtc = QDateTime::currentSecsSinceEpoch();
    return info;
}

bool CallsignLookupService::emitPrefixFallback(const QString& call)
{
    if (m_fallbackShown.contains(call))
        return true;   // already showing the prefix card for this attempt
    CallsignInfo info = prefixInfo(call);
    if (!info.isValid())
        return false;
    m_fallbackShown.insert(call);
    stampGeo(info);
    qCDebug(lcQrz) << "prefix fallback for" << call << "→" << info.country;
    // Deliberately NOT cached: a country-level stand-in must never mask a
    // future full QRZ lookup.
    emit infoReady(info, /*fromCache=*/false);
    return true;
}

void CallsignLookupService::stampGeo(CallsignInfo& info) const
{
    info.distanceKm = -1.0;
    info.bearingDeg = -1.0;
    info.distanceApprox = false;
    if (!m_hasOwnLocation)
        return;

    // Best available station position: exact lat/lon → grid center →
    // (already folded into hasLatLon for prefix cards) country center.
    double lat = 0.0, lon = 0.0;
    bool have = false;
    if (info.hasLatLon) {
        lat = info.latitude;
        lon = info.longitude;
        have = true;
        info.distanceApprox = info.prefixOnly;   // country center is coarse
    } else if (!info.grid.isEmpty()
               && MaidenheadLocator::toLatLon(info.grid, lat, lon)) {
        have = true;
    }
    if (!have)
        return;

    info.distanceKm = MaidenheadLocator::distanceKm(m_ownLat, m_ownLon, lat, lon);
    info.bearingDeg = MaidenheadLocator::bearingDeg(m_ownLat, m_ownLon, lat, lon);
}

void CallsignLookupService::testLogin(const QString& username, const QString& password)
{
    m_client->testLogin(username, password);
}

// ---------------------------------------------------------------- cache --

QString CallsignLookupService::cacheFilePath() const
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    return dir + QDir::separator() + QStringLiteral("qrz-callsign-cache.json");
}

QString CallsignLookupService::photoDirPath() const
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    return dir + QDir::separator() + QStringLiteral("qrz-photos");
}

void CallsignLookupService::ensureCacheLoaded()
{
    if (m_cacheLoaded)
        return;
    m_cacheLoaded = true;

    QFile f(cacheFilePath());
    if (!f.open(QIODevice::ReadOnly))
        return;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    const QJsonObject entries = doc.object().value(QLatin1String("entries")).toObject();
    for (auto it = entries.constBegin(); it != entries.constEnd(); ++it) {
        const CallsignInfo info = CallsignInfo::fromJson(it.value().toObject());
        if (info.isValid())
            m_cache.insert(info.call, info);
    }
    qCDebug(lcQrz) << "loaded" << m_cache.size() << "cached callsigns";
}

void CallsignLookupService::scheduleCacheSave()
{
    m_saveTimer.start();
}

void CallsignLookupService::saveCacheNow()
{
    if (!m_cacheDirty)
        return;
    m_cacheDirty = false;

    // Prune far-expired entries (2× TTL) and cap total size by age so the
    // cache file can't grow without bound on a contest weekend.
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    QList<CallsignInfo> entries;
    entries.reserve(m_cache.size());
    for (const CallsignInfo& info : std::as_const(m_cache)) {
        if (!info.isOlderThan(2 * kCacheTtlSec, now))
            entries.append(info);
    }
    if (entries.size() > kMaxCacheEntries) {
        std::sort(entries.begin(), entries.end(),
                  [](const CallsignInfo& a, const CallsignInfo& b) {
                      return a.fetchedUtc > b.fetchedUtc;
                  });
        entries = entries.mid(0, kMaxCacheEntries);
    }

    QJsonObject obj;
    for (const CallsignInfo& info : std::as_const(entries))
        obj.insert(info.call, info.toJson());
    QJsonObject root;
    root[QStringLiteral("saved")] = static_cast<double>(now);
    root[QStringLiteral("entries")] = obj;

    QDir().mkpath(QFileInfo(cacheFilePath()).absolutePath());
    QFile f(cacheFilePath());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qCWarning(lcQrz) << "cannot write callsign cache:" << f.errorString();
        return;
    }
    f.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

int CallsignLookupService::cacheEntryCount()
{
    ensureCacheLoaded();
    return m_cache.size();
}

bool CallsignLookupService::hasCachedEntry(const QString& call)
{
    ensureCacheLoaded();
    return m_cache.contains(Callsigns::normalized(call));
}

CallsignInfo CallsignLookupService::cachedEntry(const QString& call)
{
    ensureCacheLoaded();
    return m_cache.value(Callsigns::normalized(call));
}

void CallsignLookupService::clearCache()
{
    ensureCacheLoaded();
    m_cache.clear();
    m_cacheDirty = true;
    saveCacheNow();
    QDir photos(photoDirPath());
    if (photos.exists())
        photos.removeRecursively();
    qCDebug(lcQrz) << "callsign cache cleared";
}

// --------------------------------------------------------------- photos --

QString CallsignLookupService::photoPathFor(const QString& call) const
{
    const QString norm = Callsigns::normalized(call);
    if (norm.isEmpty())
        return {};
    // Portable suffixes contain '/', which can't appear in a filename.
    QString base = norm;
    base.replace(QLatin1Char('/'), QLatin1Char('_'));
    const QDir dir(photoDirPath());
    for (const char* ext : {"jpg", "png", "gif"}) {
        const QString path = dir.filePath(base + QLatin1Char('.') + QLatin1String(ext));
        if (QFile::exists(path))
            return path;
    }
    return {};
}

void CallsignLookupService::fetchPhoto(const CallsignInfo& info)
{
    if (info.imageUrl.isEmpty() || m_photoInFlight.contains(info.call))
        return;
    if (!photoPathFor(info.call).isEmpty()) {
        emit photoReady(info.call, photoPathFor(info.call));
        return;
    }

    const QUrl url(info.imageUrl);
    // https only — the <image> URL is untrusted; permitting http (with the
    // NoLessSafeRedirectPolicy below allowing cross-host) would let a crafted
    // profile reach an internal host over cleartext (SSRF-lite). (#3990)
    if (!url.isValid() || url.scheme() != QLatin1String("https"))
        return;

    m_photoInFlight.insert(info.call);
    QNetworkRequest req{url};
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  QStringLiteral("AetherSDR/%1").arg(QCoreApplication::applicationVersion()));
    req.setTransferTimeout(kPhotoTimeoutMs);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);

    auto* reply = m_photoNam.get(req);
    // Cap the untrusted download so a decompression-bomb / huge image can't
    // exhaust memory: abort as soon as the body (or advertised size) exceeds
    // the cap; the abort trips reply->error() and the finished handler drops it. (#3990)
    constexpr qint64 kMaxPhotoBytes = 8 * 1024 * 1024;
    connect(reply, &QNetworkReply::downloadProgress, reply,
            [reply](qint64 received, qint64 total) {
        if (received > kMaxPhotoBytes || total > kMaxPhotoBytes)
            reply->abort();
    });
    const QString call = info.call;
    connect(reply, &QNetworkReply::finished, this, [this, reply, call, url] {
        reply->deleteLater();
        m_photoInFlight.remove(call);
        if (reply->error() != QNetworkReply::NoError) {
            qCDebug(lcQrz) << "photo fetch failed for" << call << reply->errorString();
            return;
        }
        const QByteArray data = reply->readAll();
        if (data.isEmpty())
            return;

        QString ext = QFileInfo(url.path()).suffix().toLower();
        if (ext != QLatin1String("jpg") && ext != QLatin1String("png")
            && ext != QLatin1String("gif"))
            ext = QStringLiteral("jpg");
        QString base = call;
        base.replace(QLatin1Char('/'), QLatin1Char('_'));

        QDir().mkpath(photoDirPath());
        QFile f(QDir(photoDirPath()).filePath(base + QLatin1Char('.') + ext));
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            qCWarning(lcQrz) << "cannot write photo cache:" << f.errorString();
            return;
        }
        f.write(data);
        f.close();
        emit photoReady(call, f.fileName());
    });
}

} // namespace AetherSDR
