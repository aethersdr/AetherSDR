#pragma once

#include <QString>

#include <functional>

class QJsonObject;
class QObject;

namespace AetherSDR {

// Configuration for the agent automation bridge (#3646): one JSON object under
// "AutomationBridge", read/written atomically, migrated once from flat keys. The
// token lives in the OS secret store via QtKeychain (service/key below), never
// here. Bools:
//   enabled    - bridge runs at launch (Radio Setup -> Network)
//   txAllowed  - an MCP client may key the transmitter (the TX guard)
//   txAck      - operator has acknowledged the TX warning once
//   readOnly   - bridge refuses every mutating verb (#4188)
// Accessors go through AppSettings and are process-wide.
class AutomationBridgeSettings {
public:
    static bool enabled();
    static void setEnabled(bool on);
    static bool txAllowed();
    static void setTxAllowed(bool on);
    static bool txAck();
    static void setTxAck(bool on);
    static bool readOnly();
    static void setReadOnly(bool on);

    // True when AETHER_AUTOMATION force-enabled the bridge at launch (the
    // headless/CI override). An env-forced start is not an operator opt-in,
    // so nothing below may rewrite the saved toggle because of one (#4181).
    static bool envForced();
    // Persist the outcome of an ASYNCHRONOUS bridge start (#4181). The saved
    // `enabled` flag is the operator's opt-in as observed by the socket: a
    // successful bind records true, a failed bind clears it so a doomed start
    // is not silently re-attempted every launch. When `forced` (see
    // envForced()) the setting is left untouched on either outcome. Returns
    // the value now persisted. This is the policy seam the GUI calls from
    // MainWindow::startAutomationBridge()'s token callback; it is here so it
    // can be pinned by a socket-free test.
    static bool recordStartOutcome(bool ok, bool forced);

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
