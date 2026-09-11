#pragma once

#include <QThread>
#include <QtGlobal>

#include <atomic>
#include <functional>
#include <memory>
#include <vector>

namespace AetherSDR {

// Engine-local authority, not a protocol credential or a PttSource. Only
// trusted composition code registers actors. Neither handle is constructible
// from client-supplied IDs; handles from another coordinator are rejected.
class TxCoordinator final {
    struct ActorState;
    struct OperationState;

public:
    class Actor {
    public:
        Actor() = default;
    private:
        friend class TxCoordinator;
        std::shared_ptr<ActorState> m_state;
    };

    class Operation {
    public:
        Operation() = default;
        // Transport workers may only read this cancellation fence. Admission,
        // completion and all model access remain on the engine owning thread.
        [[nodiscard]] bool permitsDispatch(qint64 monotonicMs) const;
        // A queued key-up may outlive normal completion, but never a new
        // operation, connection reset, or coordinator destruction.
        [[nodiscard]] bool permitsCleanup() const;
        [[nodiscard]] bool sameOperation(const Operation& other) const;
    private:
        friend class TxCoordinator;
        std::shared_ptr<OperationState> m_state;
    };

    struct ActorPolicy {
        bool mayTransmit{false};
        // Zero means no new timeout for an existing local operator workflow.
        // A bounded actor cannot extend a transmission by repeating acquire().
        qint64 maximumOperationMs{0};
    };

    enum class Refusal { None, WrongThread, InvalidActor, Denied, Busy, Recovering };
    struct Admission {
        Operation operation;
        Refusal refusal{Refusal::InvalidActor};
        [[nodiscard]] bool accepted() const { return refusal == Refusal::None; }
    };

    enum class StopReason { OwnerCancelled, ActorRevoked, Expired, Reset, Emergency };
    using StopHandler = std::function<void(const Operation&, StopReason)>;
    static constexpr int kMaximumActors = 64;

    explicit TxCoordinator(StopHandler stopHandler);
    ~TxCoordinator();
    TxCoordinator(const TxCoordinator&) = delete;
    TxCoordinator& operator=(const TxCoordinator&) = delete;

    [[nodiscard]] Actor registerActor(ActorPolicy policy);
    [[nodiscard]] Admission acquire(const Actor& actor, qint64 monotonicMs);
    // Stop-only delivery fence, including when no operation was acquired.
    // It conveys no key-on, ownership, completion or acknowledgment authority.
    [[nodiscard]] Operation cleanupFence() const;
    // Normal completion is for an already-stopped operation. Cancellation
    // invalidates queued work first, then calls the engine's immediate stop.
    [[nodiscard]] bool complete(const Operation& operation);
    [[nodiscard]] bool cancel(const Actor& actor, const Operation& operation);
    void revoke(const Actor& actor);
    void expire(qint64 monotonicMs);
    void reset();
    void emergencyStop();
    // The stop handler must arrange qualified readback or transport completion
    // before acknowledging recovery. Merely requesting unkey is not proof.
    [[nodiscard]] bool acknowledgeStopped(const Operation& operation);
    [[nodiscard]] bool owns(const Actor& actor, const Operation& operation) const;
    [[nodiscard]] bool recovering() const;

private:
    struct Identity {
        std::atomic<quint64> generation{0};
    };
    struct ActorState {
        std::weak_ptr<Identity> coordinator;
        ActorPolicy policy;
        bool revoked{false};
    };
    struct OperationState {
        std::shared_ptr<ActorState> actor;
        std::atomic<bool> cancelled{false};
        qint64 startedMs{0};
        qint64 maximumMs{0};
        quint64 generation{0};
    };

    [[nodiscard]] bool onThread() const;
    [[nodiscard]] bool validActor(const Actor& actor) const;
    void stop(StopReason reason);

    QThread* const m_thread;
    std::shared_ptr<Identity> m_identity;
    std::vector<std::weak_ptr<ActorState>> m_actors;
    Operation m_active;
    Operation m_stopping;
    StopHandler m_stopHandler;
    bool m_inStopHandler{false};
};

} // namespace AetherSDR
