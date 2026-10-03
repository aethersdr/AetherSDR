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
    std::function<void(bool)> callback;
};
#endif
struct Entry {
    QString code;
    QString endpoint;
    PeripheralAuthStore::LoadStatus status{PeripheralAuthStore::LoadStatus::Missing};
    bool loaded{false};
    bool loading{false};
    std::vector<PendingLoad> pending;
#ifdef HAVE_KEYCHAIN
    std::deque<PendingWrite> writes;
    bool writing{false};
    quint64 saveRevision{0};
#endif
};
std::array<Entry, 3> g_entries;

size_t indexOf(PeripheralAuthStore::Device device)
{
    return static_cast<size_t>(device);
}

QString keyFor(PeripheralAuthStore::Device device)
{
    return QLatin1String(kKeys[indexOf(device)]);
}

PeripheralAuthStore::LoadResult resultFor(const Entry& entry, const QString& endpoint)
{
    if (entry.status == PeripheralAuthStore::LoadStatus::Unavailable) {
        return {{}, entry.status};
    }
    if (!endpoint.isEmpty() && endpoint == entry.endpoint && !entry.code.isEmpty()) {
        return {entry.code, PeripheralAuthStore::LoadStatus::Found};
    }
    return {{}, PeripheralAuthStore::LoadStatus::Missing};
}

void deliver(PendingLoad request, const Entry& entry)
{
    if (request.context) {
        const PeripheralAuthStore::LoadResult result = resultFor(entry, request.endpoint);
        QTimer::singleShot(0, request.context,
                           [callback = std::move(request.callback), result]() {
            callback(result);
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
        // Keep the cached credential until deletion is confirmed. A later
        // save owns the cache and must survive an older delete completion.
        if (ok && deleting && request.saveRevision == result.saveRevision) {
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
            request.callback(ok);
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

void PeripheralAuthStore::load(Device device, const QString& endpoint, QObject* context,
                               std::function<void(const LoadResult&)> callback)
{
    if (!context) {
        return;
    }
    Entry& entry = g_entries[indexOf(device)];
    if (entry.loaded) {
        deliver({context, endpoint, std::move(callback)}, entry);
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
            deliver(std::move(request), result);
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
        deliver(std::move(request), entry);
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
        entry.code = code;
        entry.endpoint = code.isEmpty() ? QString() : endpoint;
        entry.status = code.isEmpty() ? LoadStatus::Missing : LoadStatus::Found;
        entry.loaded = true;
    }

#ifdef HAVE_KEYCHAIN
    entry.writes.push_back({endpoint, code, entry.saveRevision, context, std::move(callback)});
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
