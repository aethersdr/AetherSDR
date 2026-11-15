#pragma once

#include "core/backends/aetherd/RemoteEngineCredentials.h"

#include <QList>
#include <QObject>
#include <QString>

#include <functional>

namespace AetherSDR::aetherd {

// The paired remote engines this machine remembers (docs/aetherd-remote-backend.md,
// *Storage*). One Principle V settings object under the root key
// "RemoteEngines" holds each engine's name, LAN address, certificate
// fingerprint and optional tailnet address; the token is never in settings.
// It goes to the OS keychain (SettingsCredentialPolicy::kAetherdCredential)
// and into the session cache the connect path reads. Without a keychain the
// token lives for this session only.
class RemoteEngineStore {
public:
    // The remembered engines, without tokens.
    [[nodiscard]] static QList<RemoteEngineRecord> records();
    [[nodiscard]] static std::optional<RemoteEngineRecord> record(const QString& engineId);

    // Remembers the engine and its token, and primes the session cache at once
    // so a connect straight after pairing does not wait on the keychain.
    static void save(const RemoteEngineRecord& record);
    // Changes the optional tailnet address of an engine already paired.
    // Must be "ws://<address>:<port>" or empty; false otherwise.
    [[nodiscard]] static bool setTailnetUrl(const QString& engineId, const QString& tailnetUrl);
    // Forgets the engine here: settings, keychain and session. The engine's own
    // list of paired clients is changed only on the engine's screen.
    static void forget(const QString& engineId);

    // Reads every remembered engine's token from the keychain into the session
    // cache. done(primed) runs on context's thread once all reads answer.
    static void primeSessions(QObject* context, std::function<void(int primed)> done = {});

    [[nodiscard]] static bool persistentStoreAvailable();

    // "wss://192.0.2.7:8443" -> true; anything not wss://<IP>:<port> -> false.
    [[nodiscard]] static bool validLanUrl(const QString& url);
    [[nodiscard]] static bool validTailnetUrl(const QString& url);
};

} // namespace AetherSDR::aetherd
