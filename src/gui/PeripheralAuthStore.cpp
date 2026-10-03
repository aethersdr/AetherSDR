#include "PeripheralAuthStore.h"
#include "core/AppSettings.h"
#include "core/PeripheralAuthCodeValidation.h"

#include <QLoggingCategory>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <array>
#include <deque>
#include <utility>
#include <vector>

#ifdef HAVE_KEYCHAIN
#include <qt6keychain/keychain.h>
#endif

namespace AetherSDR {
namespace {

Q_LOGGING_CATEGORY(lcPeripheralAuth, "aether.peripheral.auth")

#ifdef HAVE_KEYCHAIN
constexpr const char* kService = "AetherSDR";
#endif
constexpr const char* kKeys[] = {
    "tgxl_auth_code", "pgxl_auth_code", "antenna_genius_auth_code"
};

struct PendingLoad {
    QPointer<QObject> context;
    QString endpoint;
    std::function<void(const PeripheralAuthStore::LoadResult&)> callback;
};
#ifdef HAVE_KEYCHAIN
struct PendingWrite {
    QString endpoint;
    QString code;
    quint64 saveRevision;
    QPointer<QObject> context;
    std::function<void(bool, bool)> callback;
    bool matchEndpoint{false};
};
#endif
struct Entry {
    QString code;
    QString endpoint;
    PeripheralAuthStore::LoadStatus status{PeripheralAuthStore::LoadStatus::Missing};
    bool loaded{false};
    bool persistent{false};
    bool loading{false};
    std::vector<PendingLoad> pending;
#ifdef HAVE_KEYCHAIN
    QKeychain::Error readError{QKeychain::NoError};
    std::deque<PendingWrite> writes;
    bool writing{false};
    quint64 saveRevision{0};
#endif
};
std::array<Entry, 3> g_entries;
#ifdef HAVE_KEYCHAIN
bool backendUnavailable(QKeychain::Error error)
{
    return error == QKeychain::NoBackendAvailable || error == QKeychain::NotImplemented;
}
#endif

size_t indexOf(PeripheralAuthStore::Device device)
{
    return static_cast<size_t>(device);
}

QString keyFor(PeripheralAuthStore::Device device)
{
    return QLatin1String(kKeys[indexOf(device)]);
}

bool deletionPending(const Entry& entry)
{
#ifdef HAVE_KEYCHAIN
    for (const PendingWrite& request : entry.writes) {
        if (request.code.isEmpty() && request.saveRevision == entry.saveRevision) {
            return true;
        }
    }
#else
    Q_UNUSED(entry);
#endif
    return false;
}

PeripheralAuthStore::LoadResult resultFor(const Entry& entry, const QString& endpoint)
{
    // Retain the cache for a failed delete, but stop serving it as soon as
    // deletion is requested, including while queued behind another write.
    if (deletionPending(entry)) {
        return {{}, PeripheralAuthStore::LoadStatus::Missing};
    }
    if (entry.status == PeripheralAuthStore::LoadStatus::Unavailable) {
        return {{}, entry.status};
    }
    if (!endpoint.isEmpty() && endpoint == entry.endpoint && !entry.code.isEmpty()) {
        return {entry.code, PeripheralAuthStore::LoadStatus::Found};
    }
    return {{}, PeripheralAuthStore::LoadStatus::Missing};
}

void deliver(PendingLoad request, PeripheralAuthStore::Device device)
{
    if (request.context) {
        QTimer::singleShot(0, request.context,
                           [callback = std::move(request.callback), device,
                            endpoint = std::move(request.endpoint)]() {
            // Clear/Remove may have run since this callback was queued.
            callback(resultFor(g_entries[indexOf(device)], endpoint));
        });
    }
}

#ifdef HAVE_KEYCHAIN
void startNextWrite(PeripheralAuthStore::Device device)
{
    Entry& entry = g_entries[indexOf(device)];
    if (entry.writing || entry.writes.empty()) {
        return;
    }
    while (!entry.writes.empty() && entry.writes.front().matchEndpoint
           && (entry.writes.front().endpoint != entry.endpoint
               || entry.writes.front().saveRevision != entry.saveRevision)) {
        PendingWrite stale = std::move(entry.writes.front());
        entry.writes.pop_front();
        if (stale.context && stale.callback) {
            stale.callback(true, false); // A newer/different owner must survive.
        }
        if (entry.writing) {
            return; // A reentrant callback started the next write.
        }
    }
    if (entry.writes.empty()) {
        return;
    }
    entry.writing = true;
    const PendingWrite& request = entry.writes.front();
    const bool deleting = request.code.isEmpty();
    QKeychain::Job* job = nullptr;
    if (deleting) {
        job = new QKeychain::DeletePasswordJob(QLatin1String(kService));
    } else {
        auto* write = new QKeychain::WritePasswordJob(QLatin1String(kService));
        const QJsonObject object{{QStringLiteral("version"), 1},
                                 {QStringLiteral("endpoint"), request.endpoint},
                                 {QStringLiteral("code"), request.code}};
        write->setTextData(QString::fromUtf8(
            QJsonDocument(object).toJson(QJsonDocument::Compact)));
        job = write;
    }
    job->setAutoDelete(true);
    job->setInsecureFallback(false);
    job->setKey(keyFor(device));
    QObject::connect(job, &QKeychain::Job::finished, job,
                     [device, deleting](QKeychain::Job* finished) {
        Entry& result = g_entries[indexOf(device)];
        PendingWrite request = std::move(result.writes.front());
        result.writes.pop_front();
        result.writing = false;
        const bool ok = finished->error() == QKeychain::NoError
                     || (deleting && finished->error() == QKeychain::EntryNotFound);
        const bool unavailable = backendUnavailable(finished->error());
        // Denied deletion retains the cache. An unavailable backend permits
        // session cleanup only; the caller must report unconfirmed persistence.
        // A later save owns the cache and survives an older delete completion.
        if (ok && request.saveRevision == result.saveRevision) {
            result.persistent = !deleting;
        }
        if ((ok || unavailable) && deleting && request.saveRevision == result.saveRevision) {
            result.persistent = false;
            result.code.clear();
            result.endpoint.clear();
            result.status = PeripheralAuthStore::LoadStatus::Missing;
            result.loaded = true;
        }
        if (!ok) {
            qCWarning(lcPeripheralAuth) << "keychain write failed:"
                                        << finished->errorString();
        }
        if (request.context && request.callback) {
            request.callback(ok, unavailable);
        }
        startNextWrite(device);
    });
    job->start();
}
#endif

} // namespace

QString PeripheralAuthStore::endpoint(const QString& configuredHost,
                                      const QString& peerAddress, quint16 port)
{
    QHostAddress peer;
    if (port == 0 || !peer.setAddress(peerAddress)) {
        return {};
    }
    const QString host = configuredHost.trimmed();
    QHostAddress literal;
    if (host.isEmpty() || literal.setAddress(host)) {
        return peer.toString() + QLatin1Char(':') + QString::number(port);
    }
    return QStringLiteral("host:") + host.toLower() + QLatin1Char(':') + QString::number(port);
}

QString PeripheralAuthStore::configuredEndpoint(const QString& configuredHost, quint16 port)
{
    const QString host = configuredHost.trimmed();
    if (port == 0 || host.isEmpty()) {
        return {};
    }
    QHostAddress literal;
    if (literal.setAddress(host)) {
        return literal.toString() + QLatin1Char(':') + QString::number(port);
    }
    return QStringLiteral("host:") + host.toLower() + QLatin1Char(':') + QString::number(port);
}

void PeripheralAuthStore::load(Device device, const QString& endpoint, QObject* context,
                               std::function<void(const LoadResult&)> callback)
{
    if (!context) {
        return;
    }
    Entry& entry = g_entries[indexOf(device)];
    if (entry.loaded || deletionPending(entry)) {
        deliver({context, endpoint, std::move(callback)}, device);
        return;
    }
    entry.pending.push_back({context, endpoint, std::move(callback)});
    if (entry.loading) {
        return;
    }
    entry.loading = true;

#ifdef HAVE_KEYCHAIN
    auto* job = new QKeychain::ReadPasswordJob(QLatin1String(kService));
    job->setAutoDelete(true);
    job->setInsecureFallback(false);
    job->setKey(keyFor(device));
    QObject::connect(job, &QKeychain::Job::finished, job,
                     [device](QKeychain::Job* finished) {
        Entry& result = g_entries[indexOf(device)];
        // A save while the OS prompt was open is newer than this read.
        if (!result.loaded) {
            result.readError = finished->error();
            if (finished->error() == QKeychain::NoError) {
                const QByteArray data = static_cast<QKeychain::ReadPasswordJob*>(finished)
                                            ->textData().toUtf8();
                const QJsonDocument document = QJsonDocument::fromJson(data);
                const QJsonObject object = document.object();
                result.endpoint = object.value(QStringLiteral("endpoint")).toString();
                result.code = object.value(QStringLiteral("code")).toString();
                if (object.value(QStringLiteral("version")).toInt() != 1
                    || result.endpoint.isEmpty() || !validPeripheralAuthCode(result.code)) {
                    // A legacy unbound code must never be sent automatically.
                    result.endpoint.clear();
                    result.code.clear();
                }
                result.status = result.code.isEmpty() ? LoadStatus::Missing : LoadStatus::Found;
                result.persistent = !result.code.isEmpty();
            } else if (finished->error() == QKeychain::EntryNotFound) {
                result.code.clear();
                result.endpoint.clear();
                result.status = LoadStatus::Missing;
            } else {
                result.code.clear();
                result.endpoint.clear();
                result.status = LoadStatus::Unavailable;
                qCWarning(lcPeripheralAuth) << "keychain read failed:"
                                            << finished->errorString();
            }
            // A denied or temporarily unavailable vault can be retried on a
            // later manual connection. EntryNotFound is a definitive absence.
            result.loaded = finished->error() == QKeychain::NoError
                         || finished->error() == QKeychain::EntryNotFound;
        }
        result.loading = false;
        std::vector<PendingLoad> pending = std::move(result.pending);
        result.pending.clear();
        for (PendingLoad& request : pending) {
            deliver(std::move(request), device);
        }
    });
    job->start();
#else
    const QJsonDocument document = QJsonDocument::fromJson(
        AppSettings::instance().takeSessionCredential(keyFor(device)).toUtf8());
    const QJsonObject object = document.object();
    entry.endpoint = object.value(QStringLiteral("endpoint")).toString();
    entry.code = object.value(QStringLiteral("code")).toString();
    if (object.value(QStringLiteral("version")).toInt() != 1
        || entry.endpoint.isEmpty() || !validPeripheralAuthCode(entry.code)) {
        entry.endpoint.clear();
        entry.code.clear();
    }
    entry.status = entry.code.isEmpty() ? LoadStatus::Missing : LoadStatus::Found;
    if (!entry.code.isEmpty()) {
        const QJsonObject restored{{QStringLiteral("version"), 1},
                                   {QStringLiteral("endpoint"), entry.endpoint},
                                   {QStringLiteral("code"), entry.code}};
        AppSettings::instance().setSessionCredential(keyFor(device),
            QString::fromUtf8(QJsonDocument(restored).toJson(QJsonDocument::Compact)));
    }
    entry.loaded = true;
    entry.loading = false;
    std::vector<PendingLoad> pending = std::move(entry.pending);
    entry.pending.clear();
    for (PendingLoad& request : pending) {
        deliver(std::move(request), device);
    }
#endif
}

void PeripheralAuthStore::save(Device device, const QString& endpoint, const QString& code,
                               QObject* context, std::function<void(bool)> callback)
{
    if (!code.isEmpty() && (endpoint.isEmpty() || !validPeripheralAuthCode(code))) {
        if (context && callback) {
            QTimer::singleShot(0, context, [callback = std::move(callback)]() { callback(false); });
        }
        return;
    }
    Entry& entry = g_entries[indexOf(device)];
#ifdef HAVE_KEYCHAIN
    if (!code.isEmpty()) {
        ++entry.saveRevision;
#else
    {
#endif
        entry.persistent = false;
        entry.code = code;
        entry.endpoint = code.isEmpty() ? QString() : endpoint;
        entry.status = code.isEmpty() ? LoadStatus::Missing : LoadStatus::Found;
        entry.loaded = true;
    }

#ifdef HAVE_KEYCHAIN
    entry.writes.push_back({endpoint, code, entry.saveRevision, context,
        [callback = std::move(callback)](bool ok, bool) {
            if (callback) {
                callback(ok);
            }
        }});
    startNextWrite(device);
#else
    const QJsonObject object{{QStringLiteral("version"), 1},
                             {QStringLiteral("endpoint"), endpoint},
                             {QStringLiteral("code"), code}};
    AppSettings::instance().setSessionCredential(keyFor(device), code.isEmpty() ? QString()
        : QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)));
    if (context && callback) {
        QTimer::singleShot(0, context, [callback = std::move(callback), code]() {
            callback(code.isEmpty());
        });
    }
#endif
}

void PeripheralAuthStore::clear(Device device, QObject* context,
                                 std::function<void(ClearResult)> callback)
{
#ifdef HAVE_KEYCHAIN
    Entry& entry = g_entries[indexOf(device)];
    entry.writes.push_back({{}, {}, entry.saveRevision, context,
        [callback = std::move(callback)](bool ok, bool unavailable) {
            if (callback) {
                callback(ok ? ClearResult::Cleared : unavailable
                    ? ClearResult::SessionCleared : ClearResult::Failed);
            }
        }});
    startNextWrite(device);
#else
    save(device, {}, {}, context, [callback = std::move(callback)](bool ok) {
        if (callback) {
            callback(ok ? ClearResult::Cleared : ClearResult::Failed);
        }
    });
#endif
}

void PeripheralAuthStore::clearForEndpoint(Device device, const QString& endpoint,
                                           QObject* context,
                                           std::function<void(ClearResult)> callback)
{
    load(device, endpoint, context,
         [device, endpoint, context = QPointer<QObject>(context), callback = std::move(callback)]
         (const LoadResult&) {
        Entry& entry = g_entries[indexOf(device)];
#ifdef HAVE_KEYCHAIN
        if (!entry.loaded && entry.status == LoadStatus::Unavailable
            && backendUnavailable(entry.readError)) {
            // No OS vault exists to hold a record: clear the session value and
            // leave `loaded` false so a later load retries the vault.
            entry.code.clear();
            entry.endpoint.clear();
            entry.persistent = false;
            callback(ClearResult::SessionCleared);
            return;
        }
#endif
        if (!entry.loaded || entry.status == LoadStatus::Unavailable) {
            callback(ClearResult::Failed);
            return;
        }
        if (endpoint.isEmpty() && !entry.code.isEmpty()) {
            callback(ClearResult::UnknownOwner);
            return;
        }
        if (!entry.code.isEmpty() && entry.endpoint != endpoint) {
            callback(ClearResult::Cleared); // Nothing owned by the removed endpoint.
            return;
        }
#ifdef HAVE_KEYCHAIN
        entry.writes.push_back({entry.endpoint, {}, entry.saveRevision, context,
            [callback](bool ok, bool unavailable) {
                callback(ok ? ClearResult::Cleared : unavailable
                    ? ClearResult::SessionCleared : ClearResult::Failed);
            }, true});
        startNextWrite(device);
#else
        clear(device, context, callback);
#endif
    });
}

std::optional<PeripheralAuthStore::CodeAvailability> PeripheralAuthStore::cachedStatus(
    Device device, const QString& endpoint)
{
    const Entry& entry = g_entries[indexOf(device)];
    if ((!entry.loaded && entry.status != LoadStatus::Unavailable && !deletionPending(entry))
        || endpoint.isEmpty()) {
        return std::nullopt;
    }
    return CodeAvailability{resultFor(entry, endpoint).status, entry.persistent};
}

bool PeripheralAuthStore::persistentStoreAvailable()
{
#ifdef HAVE_KEYCHAIN
    return true;
#else
    return false;
#endif
}

bool PeripheralAuthStore::validCode(const QString& code)
{
    return validPeripheralAuthCode(code);
}

} // namespace AetherSDR
