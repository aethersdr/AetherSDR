#pragma once

#include <QObject>
#include <QString>

#include <functional>

namespace AetherSDR {

// The Icom network password, and the ONLY place it is persisted.
//
// RFC #4603 proposal E: no credential is ever stored in the settings database.
// QtKeychain is the persistent store; `IcomSettings` deliberately has no
// password field, and ("Icom", "Password") is registered in
// SettingsCredentialPolicy so that a future caller writing one into the
// document has it stripped at the AppSettings seam and redacted from exports.
//
// WHY THIS MATTERS MORE THAN USUAL. The Icom protocol does not encrypt the
// password — it passes it through a fixed 95-entry substitution table
// (icom-oracle §2.5) that is trivially reversible. Anyone with a packet capture
// on the same LAN already has the operator's radio password. That is the
// radio's design and we cannot change it; what we CAN avoid is compounding it
// by writing the same secret into a settings file that ends up attached to bug
// reports. Hence the keychain, and hence this being a named component rather
// than a QString on a settings object.
//
// ASYNC READ, SYNC USE. Keychain reads are asynchronous, and the connect path
// is not — RadioModel builds a RadioConnectRequest and calls connectRadio()
// synchronously, including on the auto-reconnect timer. So the password is
// loaded into a process-lifetime session cache (the connect dialog does this
// when the operator selects Icom, and again when they type one), and the
// connect path reads the cache. A reconnect therefore works without a second
// keychain prompt, and nothing on the connect path can block on the keyring.
class IcomCredentials {
public:
    // Read the stored password. The callback runs on `context`'s thread once
    // the keychain answers; it receives an empty string when nothing is stored
    // or the keychain is unavailable. Also primes the session cache on success.
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
