#pragma once

#include <QThread>
#include <QMetaType>
#include <QtGlobal>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace AetherSDR {

// Engine-local authority, not a protocol credential or a PttSource. Only
// trusted composition code registers actors. Neither handle is constructible
// from client-supplied IDs; handles from another coordinator are rejected.
class TxCoordinator final {
    struct Identity;
    struct ActorState;
    struct OperationState;
    struct IntentState;
    struct ProducerState;
    struct RequestState;

public:
    enum class Activity : unsigned { Mox = 1, Tune = 2, Atu = 4, CwKey = 8, CwPtt = 16, Cwx = 32 };
    class Operation;
    class Context;
    class Request;
    // A producer is a trusted in-process lifetime, not a client-supplied ID
    // or an independent TX actor. Copies cannot renew an invalidated lifetime.
    class Producer {
    public:
        [[nodiscard]] bool valid() const;
        [[nodiscard]] bool sameProducer(const Producer& other) const;
        void invalidate() const; // atomic; callable by the producer's destructor
        // A native device close discards queued/held input without destroying
        // the device object. Only a later raw input may capture the new epoch.
        void discardInputs() const;
        // Capture at the input boundary, before any queued hop. This does not
        // admit TX; only the owner can bind it to an already admitted operation.
        [[nodiscard]] Request request() const;
        [[nodiscard]] bool ownsRequest(const Request& input) const;
    private:
        friend class TxCoordinator;
        friend class Context;
        friend class Request;
        [[nodiscard]] Request makeRequest(const std::shared_ptr<RequestState>& input) const;
        std::shared_ptr<ProducerState> m_state;
    };
    class Actor {
    public:
        Actor() = default;
    private:
        friend class TxCoordinator;
        std::shared_ptr<ActorState> m_state;
    };

    // Hold across the actual terminal write, not across a queued callback.
    // Cancellation cannot retract a write already entered; this guard keeps
    // that fact visible until it returns. No worker waits for the owner thread.
    class Dispatch {
    public:
        Dispatch() = default;
        ~Dispatch();
        Dispatch(Dispatch&& other) noexcept;
        Dispatch& operator=(Dispatch&& other) noexcept;
        Dispatch(const Dispatch&) = delete;
        Dispatch& operator=(const Dispatch&) = delete;
        explicit operator bool() const { return bool(m_identity); }
    private:
        friend class TxCoordinator;
        friend class Operation;
        friend class Context;
        explicit Dispatch(std::shared_ptr<Identity> identity, bool continuous = false);
        std::shared_ptr<Identity> m_identity;
        bool m_continuous{false};
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
        [[nodiscard]] bool sameAuthority(const Operation& other) const;
        // Restrict an existing grant with a producer's immutable cancellation
        // predicate. The trusted caller captures only worker-safe state; the
        // predicate cannot authorize a command or extend the operation.
        [[nodiscard]] Operation withKeyingPermit(std::function<bool()> permit) const;
        // For a backend's already accepted MOX latch only, never initial
        // queued-command admission or audio. Compatible MOX/CW-PTT holds
        // sustain the latch; the last live producer disappearing clears it.
        [[nodiscard]] Operation heldKeying() const;
        // Separate sustain for an admitted CW carrier or its backend-owned
        // break-in envelope. A bare CW element never sustains ordinary MOX.
        // Like heldKeying(), this cannot admit a queued original command.
        [[nodiscard]] Operation heldCwKeying() const;
        [[nodiscard]] Dispatch beginDispatch(qint64 monotonicMs, bool keying = true) const;
    private:
        friend class TxCoordinator;
        [[nodiscard]] Operation heldActivities(unsigned activities) const;
        std::shared_ptr<OperationState> m_state;
        std::shared_ptr<ProducerState> m_producer;
        std::shared_ptr<IntentState> m_intent;
        std::shared_ptr<const std::function<bool()>> m_keyingPermit;
    };

    // Local queue bookkeeping, never radio-idle evidence. Every copy shares a
    // once-only completion; dropping a queued item completes it too. The
    // trusted caller supplies a callback that marshals to its owner thread.
    class Completion {
        struct State {
            explicit State(std::function<void()> callback) : done(std::move(callback)) {}
            ~State() { finish(); }
            void finish()
            {
                if (!finished.exchange(true) && done) {
                    done();
                }
            }
            std::atomic<bool> finished{false};
            const std::function<void()> done;
        };
    public:
        Completion() = default;
        explicit Completion(std::function<void()> done)
            : m_state(std::make_shared<State>(std::move(done))) {}
        void finish() const { if (m_state) { m_state->finish(); } }
    private:
        std::shared_ptr<State> m_state;
    };

    // A typed command carries its original operation across scheduling and
    // replay. Cleanup may outlive normal completion but never a new operation.
    struct Command {
        enum class ReplayGroup { None, Keying, Atu, CwText };
        Operation operation;
        bool keying{true};
        Completion completion{};
        ReplayGroup replayGroup{ReplayGroup::None};
        [[nodiscard]] bool permitsDispatch(qint64 now) const
        {
            return keying ? operation.permitsDispatch(now) : operation.permitsCleanup();
        }
        [[nodiscard]] Dispatch beginDispatch(qint64 now) const
        {
            return operation.beginDispatch(now, keying);
        }
    };

    // One producer's contribution to an admitted operation. The producer keeps
    // its own handle and passes copies to delayed release callbacks. A handle
    // is not an actor grant, and ending it is not radio-idle evidence.
    class Intent {
    public:
        Intent() = default;
        [[nodiscard]] bool pending() const;
        [[nodiscard]] bool permitsDispatch(qint64 monotonicMs) const;
        [[nodiscard]] bool sameIntent(const Intent& other) const;
        [[nodiscard]] bool isActivity(Activity activity) const;
    private:
        friend class TxCoordinator;
        std::shared_ptr<IntentState> m_state;
    };

    // One explicit producer request, including a queued on/off pair. It binds
    // once: neither an old callback nor a reconnect can adopt a new operation.
    class Request {
    public:
        [[nodiscard]] bool valid() const;
        // Cleanup bookkeeping only: ignores request/producer cancellation,
        // but never survives a connection reset or coordinator destruction.
        // This is NOT a transmit admission or dispatch permit.
        [[nodiscard]] bool originalSessionCurrent() const;
        [[nodiscard]] bool sameRequest(const Request& other) const;
        [[nodiscard]] bool derivedFrom(const Request& input) const;
        [[nodiscard]] bool sameInputEpoch(const Request& other) const;
        // A sequencer derives each element/frame from the original unbound
        // input, never from the current connection at timer/output time.
        // One derivation level only; all handles share the global bound.
        [[nodiscard]] Request derive() const;
    private:
        friend class TxCoordinator;
        friend class Producer;
        std::shared_ptr<RequestState> m_state;
    };

    // Immutable provenance for a queued audio block. Continuous microphone
    // media is explicitly granted for VOX/RX metering; other media must carry
    // its original admitted operation. Neither kind can acquire or key TX.
    class Context {
    public:
        [[nodiscard]] bool permitsDispatch(qint64 monotonicMs) const;
        [[nodiscard]] Dispatch beginDispatch(qint64 monotonicMs) const;
        [[nodiscard]] bool sameContext(const Context& other) const;
    private:
        friend class TxCoordinator;
        Producer m_producer;
        Operation m_operation;
        quint64 m_session{0};
        bool m_continuous{false};
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
    static constexpr int kMaximumIntents = 256;
    static constexpr int kMaximumProducers = 256;
    static constexpr int kMaximumRequests = 256;

    explicit TxCoordinator(StopHandler stopHandler);
    [[nodiscard]] static qint64 monotonicMs();
    ~TxCoordinator();
    TxCoordinator(const TxCoordinator&) = delete;
    TxCoordinator& operator=(const TxCoordinator&) = delete;

    [[nodiscard]] Actor registerActor(ActorPolicy policy);
    [[nodiscard]] Producer registerProducer(bool continuousMicrophone = false);
    [[nodiscard]] bool ownsRequest(const Request& request) const;
    [[nodiscard]] std::vector<Request> producerRequests(const Producer& producer) const;
    void setProducerAdmissionObserver(const Producer& producer, std::function<void()> observer);
    void notifyProducerAdmission(const Request& request);
    [[nodiscard]] Context mediaContext(const Producer& producer, const Operation& operation = {}) const;
    [[nodiscard]] bool acceptsRequest(const Request& request) const;
    [[nodiscard]] bool hasForeignIntents(const Operation& operation, const Request* request,
                                         unsigned activities = 0) const;
    [[nodiscard]] Intent beginRequest(const Request& request, const Operation& operation, Activity activity);
    [[nodiscard]] Intent requestIntent(const Request& request) const;
    [[nodiscard]] Operation requestOperation(const Request& request) const;
    [[nodiscard]] Context mediaContext(const Request& request) const;
    // Close admission immediately, including for an off received before its
    // queued on. The original intent stays alive for its normal queued tail.
    [[nodiscard]] Intent closeRequest(const Request& request);
    [[nodiscard]] bool hasOtherIntents(const Operation& operation, const Intent& excluded,
                                        unsigned activities = 0, bool includeFinishing = true) const;
    [[nodiscard]] Admission acquire(const Actor& actor, qint64 monotonicMs);
    // Repeated admission by the same producer reuses its live handle; it does
    // not accumulate reference-counted holds. Distinct producers use distinct
    // handles even when sharing the transitional desktop actor/operation.
    [[nodiscard]] Intent beginIntent(const Operation& operation, const Intent& previous, Activity activity);
    // Mark release BEFORE invoking callbacks or enqueueing cleanup. A new
    // request then gets a distinct handle while this one's queued tail drains.
    [[nodiscard]] bool requestIntentEnd(const Intent& intent);
    [[nodiscard]] bool endIntent(const Intent& intent);
    [[nodiscard]] bool hasIntents(const Operation& operation) const;
    [[nodiscard]] unsigned activeActivities(const Operation& operation) const;
    // Stop-only delivery fence, including when no operation was acquired.
    // It conveys no key-on, ownership, completion or acknowledgment authority.
    [[nodiscard]] Operation cleanupFence() const;
    // Ends local intent and fences queued key-on, but does NOT assert radio
    // idle. Until qualified acknowledgment, only this same actor can start
    // another operation (the transitional desktop compatibility workflow).
    // A bounded actor retains its original deadline across unconfirmed tails.
    [[nodiscard]] bool finishLocalIntent(const Operation& operation);
    // Operator-level local stop: retire every contribution to this exact
    // operation, retaining the same unconfirmed-actor barrier as normal end.
    void finishLocalIntents(const Operation& operation);
    // Fence inputs captured before an explicit operator cancel, including
    // requests that have not reached admission. Producers themselves survive.
    void discardCapturedRequests();
    // Cancellation invalidates queued work before the engine's immediate stop.
    [[nodiscard]] bool cancel(const Actor& actor, const Operation& operation);
    void revoke(const Actor& actor);
    void expire(qint64 monotonicMs);
    void reset();
    void emergencyStop();
    // Qualified readback or transport teardown must precede acknowledgment.
    // Merely requesting unkey or draining a local queue is not proof. A matching
    // acknowledgment clears either forced-stop recovery or an unconfirmed local
    // completion. An old completion cannot acknowledge a newer operation.
    //
    // INVARIANT: every stop source needs a matching acknowledgment, because an
    // unacknowledged stop keeps admission closed forever — recovering() stays
    // true and every later acquire() is refused Recovering. Today the only
    // production stop source is reset(), and teardownBackend()/onDisconnected()
    // acknowledge it, so the barrier always clears with the session. cancel(),
    // revoke(), expire() and emergencyStop() have no production callers yet;
    // whichever increment gives one of them a caller has to land its
    // acknowledgment path in the same change, not after it. RadioModel logs a
    // warning when it hits this refusal outside a disconnect gap.
    [[nodiscard]] bool acknowledgeStopped(const Operation& operation);
    [[nodiscard]] bool owns(const Actor& actor, const Operation& operation) const;
    [[nodiscard]] bool recovering() const;
    [[nodiscard]] bool hasInFlightDispatches() const;

private:
    struct Identity {
        static constexpr quint64 kChangingGeneration = quint64{1} << 63;
        std::atomic<quint64> generation{0};
        std::atomic<quint64> session{0};
        std::atomic<quint64> inputEpoch{0};
        std::atomic<quint64> dispatches{0};
        // RX microphone media cannot hold up a fresh PTT intent. Teardown,
        // unlike operation admission, must account for these writes too.
        std::atomic<quint64> continuousDispatches{0};
        std::atomic<bool> alive{true};
        std::atomic<int> requests{0};
    };
    struct ActorState {
        std::weak_ptr<Identity> coordinator;
        ActorPolicy policy;
        bool revoked{false};
    };
    struct ProducerState {
        std::weak_ptr<Identity> coordinator;
        std::atomic<bool> valid{true};
        std::atomic<quint64> inputEpoch{0};
        bool continuousMicrophone{false};
        std::function<void()> admitted; // owner-thread only, never called by transport workers
        std::mutex requestMutex; // input capture only, never audio dispatch
        std::vector<std::weak_ptr<RequestState>> requests;
    };
    struct OperationState {
        std::shared_ptr<ActorState> actor;
        std::atomic<bool> cancelled{false};
        qint64 startedMs{0};
        qint64 maximumMs{0};
        quint64 generation{0};
        // Immutable bounded snapshot, published by the owner. Weak entries
        // avoid an operation/intent ownership cycle. Read only by TX workers.
        std::shared_ptr<const std::vector<std::weak_ptr<IntentState>>> holds;
    };
    struct IntentState {
        Operation operation;
        std::shared_ptr<ProducerState> producer;
        Activity activity{Activity::Mox};
        std::atomic<bool> ended{false};
        std::function<bool()> inputPermit; // immutable before worker publication
        quint64 producerEpoch{0};
        bool finishing{false}; // owner-thread only; not the worker dispatch fence
    };
    struct RequestState {
        ~RequestState() { identity->requests.fetch_sub(1, std::memory_order_release); }
        std::shared_ptr<Identity> identity;
        Producer producer;
        quint64 session{0};
        quint64 inputEpoch{0};
        quint64 producerEpoch{0};
        std::shared_ptr<RequestState> input;
        std::atomic<bool> closed{false};
        // Published once by the owner, then immutable. Readers acquire bound
        // before copying intent; no mutex/atomic-shared_ptr is needed on the
        // input worker, and no toolchain-specific specialization is required.
        std::shared_ptr<IntentState> intent;
        std::atomic<bool> bound{false};
    };

    [[nodiscard]] bool onThread() const;
    [[nodiscard]] bool validActor(const Actor& actor) const;
    void stop(StopReason reason);
    void endIntents(const Operation& operation);
    [[nodiscard]] Intent beginIntent(const Operation& operation, const Intent& previous,
                                      Activity activity, const std::shared_ptr<ProducerState>& producer,
                                      std::function<bool()> inputPermit = {}, quint64 producerEpoch = 0);

    QThread* const m_thread;
    std::shared_ptr<Identity> m_identity;
    std::vector<std::weak_ptr<ActorState>> m_actors;
    std::vector<std::weak_ptr<ProducerState>> m_producers;
    std::vector<std::shared_ptr<IntentState>> m_intents;
    std::vector<Request> m_boundRequests; // retains admitted inputs through terminal cleanup
    Operation m_active;
    Operation m_unconfirmed;
    Operation m_stopping;
    StopHandler m_stopHandler;
    bool m_inStopHandler{false};
};

} // namespace AetherSDR

Q_DECLARE_METATYPE(AetherSDR::TxCoordinator::Context)
Q_DECLARE_METATYPE(AetherSDR::TxCoordinator::Request)
