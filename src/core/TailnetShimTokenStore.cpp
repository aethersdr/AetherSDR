#include "TailnetShimTokenStore.h"

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QTimer>

#ifdef HAVE_KEYCHAIN
#include <qt6keychain/keychain.h>
#endif

#include <utility>

namespace AetherSDR {

namespace {

QString keyFor(const QString& radioSerial)
{
    return QStringLiteral("TailnetShim/AdminToken/") + radioSerial;
}

// Session copy: always written, so a save followed at once by a load never
// races the keyring, and keychain-less builds still work until restart.
QHash<QString, QString>& sessionTokens()
{
    static QHash<QString, QString> tokens;
    return tokens;
}

#ifdef HAVE_KEYCHAIN
constexpr const char* kKeychainService = "AetherSDR";
#endif

}  // namespace

bool TailnetShimTokenStore::persistentStoreAvailable()
{
#ifdef HAVE_KEYCHAIN
    return true;
#else
    return false;
#endif
}

void TailnetShimTokenStore::save(const QString& radioSerial, const QString& token)
{
    const QString key = keyFor(radioSerial);
    if (token.isEmpty()) {
        sessionTokens().remove(key);
    } else {
        sessionTokens().insert(key, token);
    }
#ifdef HAVE_KEYCHAIN
    QKeychain::Job* job = nullptr;
    if (token.isEmpty()) {
        job = new QKeychain::DeletePasswordJob(QString::fromLatin1(kKeychainService));
    } else {
        auto* write = new QKeychain::WritePasswordJob(QString::fromLatin1(kKeychainService));
        write->setTextData(token);
        job = write;
    }
    job->setKey(key);
    job->setAutoDelete(true);
    job->start();
#endif
}

void TailnetShimTokenStore::load(const QString& radioSerial, QObject* context,
                                 std::function<void(const QString&)> callback)
{
    const QString key = keyFor(radioSerial);
    const QString session = sessionTokens().value(key);
#ifdef HAVE_KEYCHAIN
    if (session.isEmpty()) {
        auto* job = new QKeychain::ReadPasswordJob(QString::fromLatin1(kKeychainService));
        job->setKey(key);
        job->setAutoDelete(true);
        QPointer<QObject> guard(context);
        QObject::connect(job, &QKeychain::Job::finished, context,
                         [guard, key, callback = std::move(callback)](QKeychain::Job* finished) {
            QString token;
            if (finished->error() == QKeychain::NoError) {
                token = static_cast<QKeychain::ReadPasswordJob*>(finished)->textData();
                if (!token.isEmpty()) {
                    sessionTokens().insert(key, token);
                }
            }
            if (guard) {
                callback(token);
            }
        });
        job->start();
        return;
    }
#endif
    QTimer::singleShot(0, context, [session, callback = std::move(callback)] {
        callback(session);
    });
}

}  // namespace AetherSDR
