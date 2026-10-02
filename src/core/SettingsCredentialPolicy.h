#pragma once

#include <QLatin1String>
#include <QString>

namespace AetherSDR {

// The single source of truth for which settings names are credentials (RFC
// #4603: QtKeychain is the only persistent credential store). Consumers:
//   - AppSettings: the XML-import exodus and the setValue() guard
//   - SettingsSanitizer: exact-name redaction alongside the shape regex
//   - the --config CLI: refuses to create a credential row
namespace SettingsCredentialPolicy {

// Legacy flat settings keys that hold a credential. Keychain service for all
// of them is "AetherSDR", matching MqttSettings / AutomationBridgeSettings.
struct FlatCredential {
    const char* settingsKey;  // legacy flat AppSettings key
    const char* keychainKey;  // key within the "AetherSDR" keychain service
};
inline constexpr FlatCredential kFlatCredentials[] = {
    {"AutomationBridgeToken", "automation_bridge_token"},
    {"MqttPass",              "mqtt_password"},
    {"AsrRemoteApiKey",       "asr_remote_api_key"},
};

// Credential-bearing FIELDS inside feature-owned JSON document values.
struct DocFieldCredential {
    const char* docKey;       // the settings key holding the document
    const char* field;        // the credential field inside it
    const char* keychainKey;
};
inline constexpr DocFieldCredential kDocFieldCredentials[] = {
    {"CopyAssist", "AsrRemoteApiKey", "asr_remote_api_key"},
    // Icom network password. IcomSettings deliberately has no password field
    // and IcomCredentials owns the keychain, so nothing should ever write this
    // — the entry is defence in depth, and it is worth having because the Icom
    // protocol obfuscates rather than encrypts the password (icom-oracle §2.5),
    // so a copy leaking into an exported settings file is a real exposure.
    {"Icom", "Password", "icom_password"},
};

// Exact-name check: is `key` one of the known flat credential keys?
inline bool isFlatCredentialKey(const QString& key)
{
    for (const auto& entry : kFlatCredentials) {
        if (key == QLatin1String(entry.settingsKey)) {
            return true;
        }
    }
    return false;
}

// Keychain key for a flat credential settings key (empty when `key` isn't one).
inline QString keychainKeyForFlat(const QString& key)
{
    for (const auto& entry : kFlatCredentials) {
        if (key == QLatin1String(entry.settingsKey)) {
            return QLatin1String(entry.keychainKey);
        }
    }
    return {};
}

// Does `key` hold a document with known credential fields?
inline bool isCredentialDocKey(const QString& key)
{
    for (const auto& entry : kDocFieldCredentials) {
        if (key == QLatin1String(entry.docKey)) {
            return true;
        }
    }
    return false;
}

} // namespace SettingsCredentialPolicy

} // namespace AetherSDR
