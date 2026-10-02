#pragma once

#include <QObject>
#include <QString>

#include <functional>

namespace AetherSDR {

// The Icom network password, and the ONLY place it is persisted (RFC #4603 E):
// QtKeychain, never the settings database. IcomSettings has no password field
// and ("Icom", "Password") is registered in SettingsCredentialPolicy so it is
// stripped and redacted if ever written. The protocol only obfuscates it with a
// reversible 95-entry substitution table, so it must not also land in settings
// files attached to bug reports.
// Async read, sync use: the connect dialog loads it into a process-lifetime
// cache, and the synchronous connect path (including auto-reconnect) reads only
// the cache, so nothing on that path blocks on the keyring.
class IcomCredentials {
public:
    // Read the stored password. Concurrent callers share one keychain read so
    // macOS never presents several authorization prompts for the same item.
    // The callback runs on `context`'s thread once the keychain answers; it
    // receives an empty string when nothing is stored or the keychain is
    // unavailable. Also primes the session cache on success. With QtKeychain,
    // context must be non-null; destroyed contexts receive no callback. Without
    // QtKeychain, context is unused and the session result is delivered
    // synchronously on the calling thread.
    static void load(QObject* context, std::function<void(const QString&)> callback);

    // Persist it, and prime the session cache immediately so a connect issued
    // in the same breath as the save does not race the keyring. An empty value
    // DELETES the stored credential rather than storing an empty one — "the
    // operator cleared the field" and "the operator has a blank password" must
    // not be the same state.
    static void save(const QString& password);

    // The session cache: what the connect path actually reads. Empty when
    // nothing has been loaded or typed this session.
    [[nodiscard]] static QString sessionPassword();

    // Set the cache WITHOUT touching the keychain. For a password the operator
    // typed but has not asked to remember.
    static void setSessionPassword(const QString& password);

    // Forget the session copy. Does not touch the keychain.
    static void clearSession();

    // False when this build has no QtKeychain, in which case the password lives
    // only in the session cache and the UI should say so rather than implying
    // it was saved.
    [[nodiscard]] static bool persistentStoreAvailable();
};

}  // namespace AetherSDR
