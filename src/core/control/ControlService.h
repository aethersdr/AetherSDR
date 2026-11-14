#pragma once

#include "ControlPairing.h"
#include "ControlProtocolCodec.h"
#include "ControlResourceStore.h"
#include "ControlSession.h"
#include "RadioConnectionTarget.h"
#include "SliceFrequencyTarget.h"
#include "ReceiveControlTarget.h"
#include "TransmitControlService.h"

#include <QJsonObject>
#include <QString>
#include <QPointer>

namespace AetherSDR::control {

struct ServiceReply {
    QJsonObject message;
    bool closeAfterWrite{false};
    // The handler answers later, through ControlSession::completeDeferred().
    // The transport writes nothing now and reads no further input from that
    // connection until the answer is delivered.
    bool deferred{false};
};

// Transport-neutral Stage-3 service kernel. The current surface is strictly
// typed reads/subscriptions plus optional non-TX connection intents. A session's
// trusted transport context supplies authorization; hello cannot grant it.
class ControlService final {
public:
    explicit ControlService(ControlResourceStore* resources,
                            RadioConnectionTarget* connectionTarget = nullptr,
                            SliceFrequencyTarget* frequencyTarget = nullptr);

    // Trusted startup only: bind once, on the owning thread, before dispatch.
    // This lets the daemon claim its endpoint before constructing any models.
    [[nodiscard]] bool bindConnectionTarget(RadioConnectionTarget* target);
    [[nodiscard]] bool bindFrequencyTarget(SliceFrequencyTarget* target);
    [[nodiscard]] bool bindReceiveTarget(ReceiveControlTarget* target);
    // Optional trusted verifier, loaded before serving. Its absence retains
    // the existing credential-rejecting local endpoint.
    [[nodiscard]] bool bindCredentials(ControlCredentials* credentials);
    [[nodiscard]] bool bindTransmitTarget(TransmitControlTarget* target);
    // Remote-client pairing (aetherd-remote-clients.md §3). Bound only when a
    // TLS listener exists, since the code is tied to its certificate. The
    // issuer and the clock outlive the service; nowMs is monotonic.
    struct PairingBinding {
        PairingIssuer* issuer{nullptr};
        QString certificateSha256;
        std::function<qint64()> nowMs;
    };
    [[nodiscard]] bool bindPairing(PairingBinding binding);

    [[nodiscard]] ServiceReply handle(
        const QByteArray& bytes, ControlSession* session) const;

    [[nodiscard]] QJsonObject capabilities(const ControlSession& session) const;

private:
    [[nodiscard]] static ServiceReply failure(
        const QString& id, ProtocolError error, bool closeAfterWrite = false);
    [[nodiscard]] static std::optional<ProtocolError> validateHelloParams(
        const QJsonObject& params);
    [[nodiscard]] static bool acceptsVersionOne(const QJsonObject& params);
    [[nodiscard]] ServiceReply handleConnection(
        const ProtocolRequest& request, const ControlSession& session) const;
    [[nodiscard]] ServiceReply handleFrequency(
        const ProtocolRequest& request, const ControlSession& session) const;
    [[nodiscard]] ServiceReply handleReceive(const ProtocolRequest& request,
        const ControlSession& session, ReceiveOperation operation) const;
    [[nodiscard]] ServiceReply beginPairing(const ProtocolRequest& request,
        const ControlSession& session) const;
    [[nodiscard]] ServiceReply completePairing(const ProtocolRequest& request,
        ControlSession* session) const;

    ControlResourceStore* m_resources{nullptr};
    QPointer<RadioConnectionTarget> m_connectionTarget;
    bool m_targetBound{false};
    QPointer<SliceFrequencyTarget> m_frequencyTarget;
    bool m_frequencyTargetBound{false};
    QPointer<ReceiveControlTarget> m_receiveTarget;
    bool m_receiveTargetBound{false};
    QPointer<ControlCredentials> m_credentials;
    bool m_credentialsBound{false};
    std::optional<PairingBinding> m_pairingBinding;
    mutable ControlPairing m_pairing;
    mutable bool m_dispatchStarted{false};
    std::unique_ptr<TransmitControlService> m_transmit;
};

} // namespace AetherSDR::control
