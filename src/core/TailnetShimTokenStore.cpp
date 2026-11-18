#include "TailnetShimTokenStore.h"
#include "SettingsCredentialPolicy.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDebug>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QTimer>

#ifdef HAVE_KEYCHAIN
#include "TailnetShimCredentialPersistence.h"
#endif

#include <utility>

namespace AetherSDR {

namespace {

QString keyFor(const QString& radioSerial, TailnetShimTokenStore::Entry entry)
{
    if (entry == TailnetShimTokenStore::Entry::AetherPDFirst) {
        return SettingsCredentialPolicy::scopedCredentialKey(SettingsCredentialPolicy::kAetherPDFirstCredential,
            QString::fromLatin1(QCryptographicHash::hash(radioSerial.toUtf8(), QCryptographicHash::Sha256).toHex()));
    }
    return (entry == TailnetShimTokenStore::Entry::Wspr ? QStringLiteral("AetherWSPR/AdminToken/")
                                                        : QStringLiteral("TailnetShim/AdminToken/"))
        + radioSerial;
}

// Session copy: always written, so a save followed at once by a load never
// races the keyring, and keychain-less builds still work until restart.
QHash<QString, QString>& sessionTokens()
{
    static QHash<QString, QString> tokens;
    return tokens;
}

// Bumped by every save, per entry: a keychain read that started before a
// save finishes after it, and must not put the older token back.
QHash<QString, quint64>& saveGenerations()
{
    static QHash<QString, quint64> generations;
    return generations;
}

QHash<QString, quint64>& credentialRevisions()
{
    static QHash<QString, quint64> revisions;
    return revisions;
}

#ifdef HAVE_KEYCHAIN
// A pending or failed write retains its session value. Once persistence is
// confirmed, each load consults the backend so other processes' changes count.
QHash<QString, bool>& sessionOverrides()
{
    static QHash<QString, bool> overrides;
    return overrides;
}

QHash<QString, QByteArray>& persistedVersions()
{
    static QHash<QString, QByteArray> versions;
    return versions;
}
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

void TailnetShimTokenStore::save(const QString& radioSerial, const QString& token,
                                 QObject* context,
                                 std::function<void(bool persisted, const QString& error)> done,
                                 Entry entry)
{
    // Without a serial every radio would share one entry, and one radio's
    // token would be offered to another. Refuse, and say why.
    if (radioSerial.trimmed().isEmpty()) {
        if (done) {
            QTimer::singleShot(0, context ? context : QCoreApplication::instance(),
                               [done = std::move(done)] {
                done(false, QStringLiteral("the radio's serial number isn't known yet"));
            });
        }
        return;
    }
    const QString key = keyFor(radioSerial, entry);
    ++saveGenerations()[key];
    ++credentialRevisions()[key];
    if (token.isEmpty()) {
        sessionTokens().remove(key);
    } else {
        sessionTokens().insert(key, token);
    }
#ifdef HAVE_KEYCHAIN
    sessionOverrides().insert(key, true);
    const quint64 generation = saveGenerations().value(key);
    QPointer<QObject> guard(context);
    tailnetshim::CredentialPersistence::system().write(key, token,
                     [guard, key, generation, deleting = token.isEmpty(), hasContext = context != nullptr, done = std::move(done)](tailnetshim::CredentialPersistence::Value result) {
        using Error = tailnetshim::CredentialPersistence::Error;
        const bool ok = result.error == Error::None || (deleting && result.error == Error::Missing);
        if (!ok) {
            qWarning() << "TailnetShimTokenStore: keychain write failed:" << result.message;
        }
        if (saveGenerations().value(key) == generation) {
            sessionOverrides().insert(key, !ok);
            if (ok) { persistedVersions().insert(key, result.version); }
        }
        if (done && (!hasContext || guard)) {
            done(ok, ok ? QString() : result.message);
        }
    });
#else
    if (done) {
        QTimer::singleShot(0, context ? context : QCoreApplication::instance(), [done = std::move(done)] {
            done(false, QStringLiteral("this build has no system keychain support"));
        });
    }
#endif
}

quint64 TailnetShimTokenStore::revision(const QString& radioSerial, Entry entry)
{
    return credentialRevisions().value(keyFor(radioSerial, entry));
}

bool TailnetShimTokenStore::clearIfCurrent(const QString& radioSerial, const QString& expectedToken,
                                          quint64 expectedRevision, Entry entry, QObject* context,
                                          std::function<void(bool)> done)
{
    if (radioSerial.trimmed().isEmpty() || expectedToken.isEmpty()
        || sessionTokens().value(keyFor(radioSerial, entry)) != expectedToken
        || revision(radioSerial, entry) != expectedRevision) {
        return false;
    }
#ifdef HAVE_KEYCHAIN
    const QString key = keyFor(radioSerial, entry);
    if (sessionOverrides().value(key) || persistedVersions().value(key).size() != 16) { return false; }
    const quint64 generation = saveGenerations().value(key);
    const QPointer<QObject> guard(context);
    tailnetshim::CredentialPersistence::system().retire(key, expectedToken, persistedVersions().value(key),
        [key, expectedToken, expectedRevision, generation, guard, hasContext = context != nullptr, done = std::move(done)](tailnetshim::CredentialPersistence::Value result) {
        const bool removed = result.error == tailnetshim::CredentialPersistence::Error::Missing;
        if (saveGenerations().value(key) == generation && credentialRevisions().value(key) == expectedRevision
            && sessionTokens().value(key) == expectedToken) {
            ++credentialRevisions()[key];
            if (removed) {
                ++saveGenerations()[key];
                sessionTokens().remove(key);
                sessionOverrides().insert(key, false);
            }
            persistedVersions().remove(key);
        }
        if (done && (!hasContext || guard)) { done(removed); }
    });
#else
    save(radioSerial, QString(), context, [done = std::move(done)](bool, const QString&) { if (done) { done(false); } }, entry);
#endif
    return true;
}

void TailnetShimTokenStore::load(const QString& radioSerial, QObject* context,
                                 std::function<void(const QString&)> callback, Entry entry)
{
    // As save() does: with no context the callback still runs, on the app.
    if (!context) {
        context = QCoreApplication::instance();
    }
    if (radioSerial.trimmed().isEmpty()) {
        // No serial, no entry to read: never another radio's token.
        QTimer::singleShot(0, context, [callback = std::move(callback)] { callback(QString()); });
        return;
    }
    const QString key = keyFor(radioSerial, entry);
    const QString session = sessionTokens().value(key);
#ifdef HAVE_KEYCHAIN
    if (session.isEmpty() || !sessionOverrides().value(key)) {
        const quint64 startedAt = saveGenerations().value(key);
        QPointer<QObject> guard(context);
        tailnetshim::CredentialPersistence::system().read(key,
                         [guard, key, startedAt, callback = std::move(callback)](tailnetshim::CredentialPersistence::Value result) {
            using Error = tailnetshim::CredentialPersistence::Error;
            QString read;
            if (result.error == Error::None) { read = result.token; }
            const bool savedSince = saveGenerations().value(key) != startedAt;
            const QString token = pickLoadedToken(savedSince, read, sessionTokens().value(key));
            if (!savedSince) {
                if (result.error == Error::None || result.error == Error::Missing) {
                    if (sessionTokens().value(key) != token || persistedVersions().value(key) != result.version) {
                        ++credentialRevisions()[key];
                    }
                    persistedVersions().insert(key, result.version);
                    if (token.isEmpty()) { sessionTokens().remove(key); }
                    else { sessionTokens().insert(key, token); }
                } else {
                    // An uncertain backend read cannot authorize retirement
                    // using a previously adopted process-local revision.
                    ++credentialRevisions()[key];
                    persistedVersions().remove(key);
                }
                sessionOverrides().insert(key, false);
            }
            if (guard) {
                callback(token);
            }
        });
        return;
    }
#endif
    // Read when it runs, not now: a save in between is the newer token.
    QTimer::singleShot(0, context, [key, callback = std::move(callback)] {
        callback(sessionTokens().value(key));
    });
}

QString TailnetShimTokenStore::pickLoadedToken(bool savedSinceReadStarted, const QString& read,
                                               const QString& sessionNow)
{
    return savedSinceReadStarted ? sessionNow : read;
}

}  // namespace AetherSDR
