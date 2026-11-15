#pragma once

#include <QList>
#include <QString>

#include <optional>

namespace AetherSDR::aetherd {

// One paired remote engine (docs/aetherd-remote-backend.md, *Storage*). The
// engine id is the certificate fingerprint it was paired against, so a
// reinstalled engine with a new certificate is a different engine.
struct RemoteEngineRecord {
    QString engineId;            // lowercase hex SHA-256 of the engine's certificate
    QString name;                // the engine's advertised host name
    QString lanUrl;              // "wss://<address>:<port>", where it was paired
    QString tailnetUrl;          // optional "ws://<tailnet address>:<port>"
    QString token;               // the bearer credential from pairing.complete

    [[nodiscard]] bool usable() const { return !engineId.isEmpty() && !lanUrl.isEmpty() && !token.isEmpty(); }
};

// The session cache the connect path reads (the IcomCredentials pattern):
// connect and automatic reconnect never wait on a keychain. The connection
// panel primes it at pairing or after its keychain read. Main thread only.
class RemoteEngineCredentials {
public:
    static void setSession(const RemoteEngineRecord& record);
    [[nodiscard]] static std::optional<RemoteEngineRecord> session(const QString& engineId);
    [[nodiscard]] static QList<RemoteEngineRecord> sessions();
    static void clearSession(const QString& engineId);
    static void clearAll();
};

} // namespace AetherSDR::aetherd
