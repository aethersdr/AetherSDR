#pragma once

#include "core/backends/aetherd/RemoteEngineCredentials.h"

#include <QObject>
#include <QString>

#include <memory>

class QWebSocket;

namespace AetherSDR::aetherd {

// Pairs this machine with a remote aetherd (aetherd-remote-clients.md §3,
// docs/aetherd-remote-backend.md §3). LAN-only, over wss://. The code shown
// on the engine's screen carries four check digits derived from the engine's
// certificate: before anything is sent, the certificate this connection
// actually reached must produce them, so a man in the middle is caught without
// comparing fingerprints by hand. Then one pairing.complete; the engine closes
// the connection after answering.
class RemoteEnginePairing final : public QObject {
    Q_OBJECT

public:
    explicit RemoteEnginePairing(QObject* parent = nullptr);
    ~RemoteEnginePairing() override;

    // lanUrl is "wss://<IP>:<port>"; code is the ten digits from the engine's
    // screen (spaces and dashes allowed). clientName labels this machine in the
    // engine's Remote clients list (1–32 bytes, trimmed).
    void start(const QString& lanUrl, const QString& code, const QString& clientName,
               const QString& engineName = {});
    void cancel();
    [[nodiscard]] bool running() const { return m_socket != nullptr; }

    // Whether the code's check digits match this certificate fingerprint.
    [[nodiscard]] static bool codeMatchesCertificate(const QString& code, const QString& certificateSha256);

signals:
    // The token is in record.token; nothing is stored yet.
    void paired(const AetherSDR::aetherd::RemoteEngineRecord& record);
    void failed(const QString& reason);

private:
    void finish(const QString& failure);

    std::unique_ptr<QWebSocket> m_socket;
    quint64 m_generation{0};
    QString m_lanUrl;
    QString m_digits;
    QString m_clientName;
    QString m_engineName;
    QString m_fingerprint;
    bool m_sent{false};
};

} // namespace AetherSDR::aetherd
