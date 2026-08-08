#include "core/backends/icom/IcomCredentials.h"

#include <QLoggingCategory>
#include <QtGlobal>
#include <QMutex>
#include <QMutexLocker>

#ifdef HAVE_KEYCHAIN
#include <qt6keychain/keychain.h>
#endif

namespace AetherSDR {
namespace {

Q_LOGGING_CATEGORY(lcIcomCred, "aether.icom.cred")

// Same service every other AetherSDR credential uses, so one keychain entry
// group holds them all.
constexpr const char* kKeychainService = "AetherSDR";
constexpr const char* kKeychainKey     = "icom_password";

// Guarded because the connect path reads it from RadioModel's thread while the
// keychain callback writes it from wherever the job completed.
QMutex g_mutex;
QString g_sessionPassword;

}  // namespace

void IcomCredentials::load(QObject* context, std::function<void(const QString&)> callback)
{
#ifdef HAVE_KEYCHAIN
    auto* job = new QKeychain::ReadPasswordJob(QLatin1String(kKeychainService));
    job->setAutoDelete(true);
    job->setKey(QLatin1String(kKeychainKey));
    QObject::connect(job, &QKeychain::Job::finished, context,
                     [callback = std::move(callback)](QKeychain::Job* j) {
        QString value;
        if (j->error() == QKeychain::NoError) {
            value = static_cast<QKeychain::ReadPasswordJob*>(j)->textData();
            setSessionPassword(value);
        } else if (j->error() != QKeychain::EntryNotFound) {
            // EntryNotFound is the ordinary first-run case and not worth a
            // warning; anything else means the keyring is there and refused us,
            // which the operator may need to act on.
            qCWarning(lcIcomCred) << "keychain read failed:" << j->errorString();
        }
        callback(value);
    });
    job->start();
#else
    Q_UNUSED(context);
    callback(sessionPassword());
#endif
}

void IcomCredentials::save(const QString& password)
{
    // Prime the cache FIRST, unconditionally. The keychain job is async, and a
    // connect issued in the same breath as the save would otherwise race it and
    // authenticate with an empty password.
    setSessionPassword(password);

#ifdef HAVE_KEYCHAIN
    if (password.isEmpty()) {
        // DELETE rather than storing an empty string: "the operator cleared the
        // field" and "the operator has a blank password" are different states,
        // and storing the latter would silently re-supply a blank on next load.
        auto* job = new QKeychain::DeletePasswordJob(QLatin1String(kKeychainService));
        job->setAutoDelete(true);
        job->setKey(QLatin1String(kKeychainKey));
        job->start();
        return;
    }
    auto* job = new QKeychain::WritePasswordJob(QLatin1String(kKeychainService));
    job->setAutoDelete(true);
    job->setKey(QLatin1String(kKeychainKey));
    job->setTextData(password);
    QObject::connect(job, &QKeychain::Job::finished, job, [](QKeychain::Job* j) {
        if (j->error() != QKeychain::NoError)
            qCWarning(lcIcomCred) << "keychain save failed:" << j->errorString();
    });
    job->start();
#else
    qCWarning(lcIcomCred)
        << "Icom password kept for this session only: built without QtKeychain";
#endif
}

QString IcomCredentials::sessionPassword()
{
    {
        QMutexLocker lock(&g_mutex);
        if (!g_sessionPassword.isEmpty())
            return g_sessionPassword;
    }
    // HEADLESS FALLBACK. An automation or CI launch has no dialog to type into
    // and, on a build without QtKeychain, nothing persisted either — so the
    // bridge's connect verb had no way to authenticate and `connect ip <host>
    // icom` could only ever fail.
    //
    // An environment variable rather than a bridge argument on purpose: bridge
    // commands are echoed into the app log ring, and a password in the log is
    // exactly the exposure the keychain policy exists to avoid.
    return qEnvironmentVariable("AETHER_ICOM_PASSWORD");
}

void IcomCredentials::setSessionPassword(const QString& password)
{
    QMutexLocker lock(&g_mutex);
    g_sessionPassword = password;
}

void IcomCredentials::clearSession()
{
    QMutexLocker lock(&g_mutex);
    g_sessionPassword.clear();
}

bool IcomCredentials::persistentStoreAvailable()
{
#ifdef HAVE_KEYCHAIN
    return true;
#else
    return false;
#endif
}

}  // namespace AetherSDR
