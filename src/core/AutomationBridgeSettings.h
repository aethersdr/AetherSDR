#pragma once

#include <QString>

#include <functional>

class QJsonObject;
class QObject;

namespace AetherSDR {

// Owned configuration for the agent automation bridge (#3646), per
// Constitution Principle V: one nested JSON object under a single root key
// ("AutomationBridge"), read/written atomically, with a one-shot migration
// from the earlier flat keys. The secret token is NOT stored here — it lives
// in the OS secret store via QtKeychain (service/key below), mirroring the
// MQTT-password pattern (GHSA-mmqp-cm4w-cvpp). The non-secret bools are:
//
//   enabled    — the bridge runs at launch (Radio Setup → Network toggle)
//   txAllowed  — an MCP client may key the transmitter (the TX guard)
//   txAck      — the operator has acknowledged the TX warning at least once
//
// All accessors go through AppSettings and are process-wide.
class AutomationBridgeSettings {
public:
    static bool enabled();
    static void setEnabled(bool on);
    static bool txAllowed();
    static void setTxAllowed(bool on);
    static bool txAck();
    static void setTxAck(bool on);

    // Keychain coordinates for the bridge access token (see MqttSettings for
    // the analogous MQTT-password helpers).
    static QString keychainService();        // "AetherSDR"
    static QString keychainKey();            // "automation_bridge_token"
    // Legacy plaintext setting key the token used to live under, kept only for
    // one-time migration into the keychain and for the no-keychain fallback.
    static QString legacyTokenSettingKey();  // "AutomationBridgeToken"

    // Async access-token I/O via QtKeychain, shared by the dialog and the
    // bridge lifecycle so the secret is never duplicated in plaintext settings.
    //
    // Resolution order in loadToken(): the AETHER_MCP_TOKEN env var (headless /
    // CI) wins synchronously; otherwise the OS secret store, migrating a legacy
    // plaintext token in on first read. When HAVE_KEYCHAIN is off, falls back
    // to the legacy plaintext setting (with a warning). `cb` always runs — with
    // an empty string if no token is set. `ctx` scopes the async callback.
    static void loadToken(QObject* ctx, std::function<void(const QString&)> cb);
    // Persist the token (empty string deletes it). Keychain when available,
    // else legacy plaintext. Also clears any legacy plaintext entry.
    static void saveToken(const QString& token);

private:
    // Reads the nested object, migrating the legacy flat keys on first access.
    static QJsonObject readObj();
    static void writeBool(const char* field, bool value);
};

} // namespace AetherSDR
