#pragma once

#include "TxCoordinator.h"

#include <QObject>
#include <QString>
#include <QTimer>

#include <functional>
#include <memory>
#include <vector>

namespace AetherSDR {

// Trusted engine composition only. The protocol adapter must authenticate the
// principal and check grant-admin authority BEFORE calling registerClient or
// issue. Neither a wire ID nor a client-supplied label constructs these handles.
// One manager per radio, sharing that radio's existing (single) coordinator.
// Production issuance is gated by the backend's qualified stop-path activities.
class TxGrantManager final : public QObject {
    struct Identity;
    struct ClientState;
    struct GrantState;
public:
    class Client {
    private:
        friend class TxGrantManager;
        std::shared_ptr<ClientState> m_state;
    };
    class Grant {
    private:
        friend class TxGrantManager;
        std::shared_ptr<GrantState> m_state;
    };
    struct Policy {
        qint64 maximumOperationMs{0};
        qint64 lifetimeMs{0};
        qint64 keepAliveMs{0};
        unsigned activities{0};
    };
    enum class Error { None, WrongThread, InvalidClient, InvalidGrant, InvalidPolicy,
                       Unsupported, Capacity, Conflict, Expired, Replay };
    struct Issuance {
        Grant grant;
        Error error{Error::InvalidClient};
        [[nodiscard]] bool accepted() const { return error == Error::None; }
    };
    struct Admission {
        TxCoordinator::Operation operation;
        TxCoordinator::Request input;
        TxCoordinator::Refusal refusal{TxCoordinator::Refusal::None};
        Error error{Error::InvalidGrant};
        [[nodiscard]] bool accepted() const
        { return error == Error::None && refusal == TxCoordinator::Refusal::None; }
    };

    using QualifiedActivities = std::function<unsigned()>;
    using AuthorizationCurrent = std::function<bool()>;
    static constexpr int kMaximumClients = 8;
    static constexpr qint64 kMaximumGrantMs = 24 * 60 * 60 * 1000;
    static constexpr qint64 kMaximumOperationMs = 60 * 60 * 1000;
    static constexpr qint64 kMaximumKeepAliveMs = 60 * 1000;

    explicit TxGrantManager(TxCoordinator& coordinator, QualifiedActivities qualifiedActivities,
                            QObject* parent = nullptr);
    ~TxGrantManager() override;
    // Optional immutable worker-safe credential fence. It is also attached to
    // every actor, so credential revocation fences media/keying before any
    // session/transport callback runs. Cleanup never requires a live credential.
    [[nodiscard]] Client registerClient(const QString& verifiedPrincipal,
                                        AuthorizationCurrent authorizationCurrent = {});
    // Explicit operator issuance only. Renewal requires revoking the old grant
    // and issuing a new one; it never mutates or resurrects old handles.
    [[nodiscard]] Issuance issue(const Client& client, Policy policy);
    [[nodiscard]] Admission acquire(const Client& client, const Grant& grant,
                                     quint64 intentSerial, TxCoordinator::Activity activity);
    [[nodiscard]] bool release(const Client& client, const Grant& grant,
                                const TxCoordinator::Operation& operation);
    [[nodiscard]] bool cancel(const Client& client, const Grant& grant,
                               const TxCoordinator::Operation& operation);
    [[nodiscard]] bool keepAlive(const Client& client, const Grant& grant);
    [[nodiscard]] bool revoke(const Grant& grant);
    void disconnectClient(const Client& client);
    // Called before replacing/disconnecting the radio. All current grants are
    // retired, even idle ones. Client authentication alone can survive it.
    void invalidateRadio();
    // Also driven by the engine timer, including when no requests arrive.
    void processDeadlines();
    [[nodiscard]] int clientCount() const;
    [[nodiscard]] int grantCount() const;
    [[nodiscard]] bool isLive(const Client& client, const Grant& grant) const;
    [[nodiscard]] bool isClient(const Client& client) const;

private:
    [[nodiscard]] bool onThread() const;
    [[nodiscard]] bool validClient(const Client& client) const;
    [[nodiscard]] bool owns(const Client& client, const Grant& grant) const;
    [[nodiscard]] bool live(const GrantState& grant, qint64 now) const;
    void retire(const std::vector<std::shared_ptr<GrantState>>& grants);
    void schedule();

    TxCoordinator& m_coordinator; // owner outlives this manager
    const QualifiedActivities m_qualifiedActivities;
    const std::shared_ptr<Identity> m_identity;
    QTimer m_timer;
    std::vector<std::shared_ptr<ClientState>> m_clients;
    std::vector<std::shared_ptr<GrantState>> m_grants;
    bool m_mutating{false};
    bool m_closing{false};
};

} // namespace AetherSDR
