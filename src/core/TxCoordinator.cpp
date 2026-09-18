#include "TxCoordinator.h"

#include <QScopeGuard>

#include <algorithm>
#include <chrono>
#include <limits>
#include <utility>

namespace AetherSDR {

qint64 TxCoordinator::monotonicMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

TxCoordinator::Dispatch::Dispatch(std::shared_ptr<Identity> identity, bool continuous)
    : m_identity(std::move(identity)), m_continuous(continuous)
{
}

TxCoordinator::Dispatch::~Dispatch()
{
    if (m_identity) {
        (m_continuous ? m_identity->continuousDispatches : m_identity->dispatches)
            .fetch_sub(1, std::memory_order_release);
    }
}

TxCoordinator::Dispatch::Dispatch(Dispatch&& other) noexcept
    : m_identity(std::move(other.m_identity)), m_continuous(other.m_continuous)
{
}

TxCoordinator::Dispatch& TxCoordinator::Dispatch::operator=(Dispatch&& other) noexcept
{
    if (this != &other) {
        if (m_identity) {
            (m_continuous ? m_identity->continuousDispatches : m_identity->dispatches)
                .fetch_sub(1, std::memory_order_release);
        }
        m_identity = std::move(other.m_identity);
        m_continuous = other.m_continuous;
    }
    return *this;
}

bool TxCoordinator::Operation::permitsDispatch(qint64 now) const
{
    if (!m_state || m_state->cancelled.load(std::memory_order_acquire)
        || (m_producer && !m_producer->valid.load(std::memory_order_acquire))
        || (m_intent && (m_intent->ended.load(std::memory_order_acquire)
            || (m_intent->producer && m_intent->producerEpoch != m_intent->producer->inputEpoch.load(std::memory_order_acquire))
            || (m_intent->inputPermit && !m_intent->inputPermit())))
        || (m_keyingPermit && !(*m_keyingPermit)())
        || now < m_state->startedMs) {
        return false;
    }
    // Subtract only after validating nonnegative, ordered clock values. Avoid
    // deadline addition, which overflows for a large trusted-clock test input.
    return m_state->maximumMs == 0
        || now - m_state->startedMs < m_state->maximumMs;
}

bool TxCoordinator::Operation::sameOperation(const Operation& other) const
{
    return m_state && m_state == other.m_state;
}

bool TxCoordinator::Operation::sameAuthority(const Operation& other) const
{
    return sameOperation(other) && m_producer == other.m_producer && m_intent == other.m_intent
        && m_keyingPermit == other.m_keyingPermit;
}

TxCoordinator::Operation TxCoordinator::Operation::withKeyingPermit(std::function<bool()> permit) const
{
    Operation operation = *this;
    const std::shared_ptr<const std::function<bool()>> previous = m_keyingPermit;
    operation.m_keyingPermit = std::make_shared<const std::function<bool()>>(
        [previous, permit = std::move(permit)] {
            return (!previous || (*previous)()) && permit && permit();
        });
    return operation;
}

TxCoordinator::Operation TxCoordinator::Operation::heldKeying() const
{
    return heldActivities(static_cast<unsigned>(Activity::Mox) | static_cast<unsigned>(Activity::CwPtt));
}

TxCoordinator::Operation TxCoordinator::Operation::heldCwKeying() const
{
    return heldActivities(static_cast<unsigned>(Activity::CwKey));
}

TxCoordinator::Operation TxCoordinator::Operation::heldActivities(unsigned activities) const
{
    if (!m_intent || !(static_cast<unsigned>(m_intent->activity) & activities)) {
        return *this;
    }
    Operation held = *this;
    held.m_producer.reset();
    held.m_intent.reset();
    const std::weak_ptr<OperationState> state = m_state;
    return held.withKeyingPermit([state, activities] {
        const std::shared_ptr<OperationState> operation = state.lock();
        if (!operation) {
            return false;
        }
        const auto holds = std::atomic_load_explicit(&operation->holds, std::memory_order_acquire);
        if (!holds) {
            return false;
        }
        for (const std::weak_ptr<IntentState>& entry : *holds) {
            const std::shared_ptr<IntentState> intent = entry.lock();
            if (intent && (static_cast<unsigned>(intent->activity) & activities)
                && !intent->ended.load(std::memory_order_acquire)
                && (!intent->producer || intent->producer->valid.load(std::memory_order_acquire))
                && (!intent->producer || intent->producerEpoch == intent->producer->inputEpoch.load(std::memory_order_acquire))
                && (!intent->inputPermit || intent->inputPermit())) {
                return true;
            }
        }
        return false;
    });
}

bool TxCoordinator::Operation::permitsCleanup() const
{
    if (!m_state) {
        return false;
    }
    const std::shared_ptr<Identity> identity = m_state->actor->coordinator.lock();
    return identity && identity->alive.load(std::memory_order_acquire)
        && identity->generation.load(std::memory_order_acquire) == m_state->generation;
}

TxCoordinator::Dispatch TxCoordinator::Operation::beginDispatch(qint64 now, bool keying) const
{
    if (!m_state) {
        return {};
    }
    const std::shared_ptr<Identity> identity = m_state->actor->coordinator.lock();
    if (!identity) {
        return {};
    }
    quint64 count = identity->dispatches.load(std::memory_order_acquire);
    do {
        if (count >= Identity::kChangingGeneration - 1) {
            return {};
        }
    } while (!identity->dispatches.compare_exchange_weak(
        count, count + 1, std::memory_order_acquire, std::memory_order_relaxed));
    Dispatch dispatch(identity);
    // Increment before checking validity. Either cancellation wins this check,
    // or the entered write stays counted through its return. Admission cannot
    // change generation between this check and the terminal writer.
    if (!permitsCleanup() || (keying && !permitsDispatch(now))) {
        return {};
    }
    return dispatch;
}

bool TxCoordinator::Intent::pending() const
{
    return m_state && !m_state->ended.load(std::memory_order_acquire)
        && m_state->operation.permitsCleanup();
}

bool TxCoordinator::Intent::permitsDispatch(qint64 now) const
{
    return pending() && m_state->operation.permitsDispatch(now)
        && (!m_state->producer || (m_state->producer->valid.load(std::memory_order_acquire)
            && m_state->producerEpoch == m_state->producer->inputEpoch.load(std::memory_order_acquire)))
        && (!m_state->inputPermit || m_state->inputPermit());
}

bool TxCoordinator::Intent::sameIntent(const Intent& other) const
{
    return m_state && m_state == other.m_state;
}

bool TxCoordinator::Intent::isActivity(Activity activity) const
{
    return m_state && m_state->activity == activity;
}

bool TxCoordinator::Producer::valid() const
{
    if (!m_state || !m_state->valid.load(std::memory_order_acquire)) {
        return false;
    }
    const std::shared_ptr<Identity> identity = m_state->coordinator.lock();
    return identity && identity->alive.load(std::memory_order_acquire);
}

bool TxCoordinator::Producer::sameProducer(const Producer& other) const
{
    return m_state && m_state == other.m_state;
}

void TxCoordinator::Producer::invalidate() const
{
    if (m_state) {
        m_state->valid.store(false, std::memory_order_release);
    }
}

void TxCoordinator::Producer::discardInputs() const
{
    if (m_state) {
        m_state->inputEpoch.fetch_add(1, std::memory_order_acq_rel);
    }
}

TxCoordinator::Request TxCoordinator::Producer::request() const
{
    return makeRequest({});
}

bool TxCoordinator::Producer::ownsRequest(const Request& input) const
{
    return input.m_state && sameProducer(input.m_state->producer);
}

bool TxCoordinator::Request::sameInputEpoch(const Request& other) const
{
    return m_state && other.m_state && m_state->producer.sameProducer(other.m_state->producer)
        && m_state->session == other.m_state->session
        && m_state->inputEpoch == other.m_state->inputEpoch
        && m_state->producerEpoch == other.m_state->producerEpoch;
}

TxCoordinator::Request TxCoordinator::Producer::makeRequest(const std::shared_ptr<RequestState>& input) const
{
    if (!valid()) {
        return {};
    }
    const std::shared_ptr<Identity> identity = m_state->coordinator.lock();
    if (!identity) {
        return {};
    }
    const quint64 session = identity->session.load(std::memory_order_acquire);
    const quint64 inputEpoch = identity->inputEpoch.load(std::memory_order_acquire);
    const quint64 producerEpoch = m_state->inputEpoch.load(std::memory_order_acquire);
    int count = identity->requests.load(std::memory_order_acquire);
    do {
        if (count >= kMaximumRequests) {
            return {};
        }
    } while (!identity->requests.compare_exchange_weak(count, count + 1));
    Request request;
    request.m_state = std::make_shared<RequestState>();
    request.m_state->identity = identity;
    request.m_state->producer = *this;
    request.m_state->session = session;
    request.m_state->inputEpoch = inputEpoch;
    request.m_state->producerEpoch = producerEpoch;
    request.m_state->input = input;
    {
        std::lock_guard<std::mutex> lock(m_state->requestMutex);
        std::erase_if(m_state->requests, [](const auto& weak) { return weak.expired(); });
        m_state->requests.push_back(request.m_state);
    }
    return request;
}

bool TxCoordinator::Request::valid() const
{
    if (!m_state || m_state->closed.load(std::memory_order_acquire)
        || !m_state->producer.valid()
        || m_state->producerEpoch != m_state->producer.m_state->inputEpoch.load(std::memory_order_acquire)
        || m_state->session != m_state->identity->session.load(std::memory_order_acquire)
        || m_state->inputEpoch != m_state->identity->inputEpoch.load(std::memory_order_acquire)) {
        return false;
    }
    const std::shared_ptr<IntentState> intent = m_state->bound.load(std::memory_order_acquire)
        ? m_state->intent : nullptr;
    if (m_state->input) {
        Request input;
        input.m_state = m_state->input;
        if (!input.valid()) {
            return false;
        }
    }
    return !intent || (!intent->ended.load(std::memory_order_acquire)
        && intent->operation.permitsDispatch(monotonicMs()));
}

bool TxCoordinator::Request::originalSessionCurrent() const
{
    return m_state && m_state->identity->alive.load(std::memory_order_acquire)
        && m_state->session == m_state->identity->session.load(std::memory_order_acquire);
}

bool TxCoordinator::Request::derivedFrom(const Request& input) const
{
    return m_state && input.m_state && m_state->input == input.m_state;
}

bool TxCoordinator::Request::sameRequest(const Request& other) const
{
    return m_state && m_state == other.m_state;
}

TxCoordinator::Request TxCoordinator::Request::derive() const
{
    if (!valid() || m_state->input || m_state->bound.load(std::memory_order_acquire)) {
        return {};
    }
    return m_state->producer.makeRequest(m_state);
}

bool TxCoordinator::Context::permitsDispatch(qint64 now) const
{
    if (now < 0 || !m_producer.valid()) {
        return false;
    }
    const std::shared_ptr<Identity> identity = m_producer.m_state->coordinator.lock();
    return identity && identity->session.load(std::memory_order_acquire) == m_session
        && (m_continuous || m_operation.permitsDispatch(now));
}

TxCoordinator::Dispatch TxCoordinator::Context::beginDispatch(qint64 now) const
{
    if (!m_producer.m_state) {
        return {};
    }
    const std::shared_ptr<Identity> identity = m_producer.m_state->coordinator.lock();
    if (!identity) {
        return {};
    }
    std::atomic<quint64>& dispatches = m_continuous
        ? identity->continuousDispatches : identity->dispatches;
    quint64 count = dispatches.load(std::memory_order_acquire);
    do {
        if (count >= Identity::kChangingGeneration - 1) {
            return {};
        }
    } while (!dispatches.compare_exchange_weak(
        count, count + 1, std::memory_order_acquire, std::memory_order_relaxed));
    Dispatch dispatch(identity, m_continuous);
    if (!permitsDispatch(now)) {
        return {};
    }
    return dispatch;
}

bool TxCoordinator::Context::sameContext(const Context& other) const
{
    return m_producer.sameProducer(other.m_producer) && m_session == other.m_session
        && m_continuous == other.m_continuous
        && (m_continuous || m_operation.sameAuthority(other.m_operation));
}

TxCoordinator::TxCoordinator(StopHandler stopHandler)
    : m_thread(QThread::currentThread())
    , m_identity(std::make_shared<Identity>())
    , m_stopHandler(std::move(stopHandler))
{
    qRegisterMetaType<Context>();
    qRegisterMetaType<Request>();
}

TxCoordinator::~TxCoordinator()
{
    // The owner performs reset while its backend is alive. Destruction is a
    // final fence only: callbacks into a partially destroyed owner are unsafe.
    m_identity->alive.store(false, std::memory_order_release);
    if (m_active.m_state) {
        m_active.m_state->cancelled.store(true, std::memory_order_release);
    }
    if (m_stopping.m_state) {
        m_stopping.m_state->cancelled.store(true, std::memory_order_release);
    }
    for (const std::shared_ptr<IntentState>& intent : m_intents) {
        intent->ended.store(true, std::memory_order_release);
    }
}

bool TxCoordinator::onThread() const
{
    return QThread::currentThread() == m_thread;
}

bool TxCoordinator::validActor(const Actor& actor) const
{
    return m_identity->alive.load(std::memory_order_acquire)
        && actor.m_state && !actor.m_state->revoked
        && actor.m_state->coordinator.lock() == m_identity;
}

TxCoordinator::Actor TxCoordinator::registerActor(ActorPolicy policy)
{
    if (!onThread() || !m_identity->alive.load(std::memory_order_acquire)
        || policy.maximumOperationMs < 0 || !m_stopHandler) {
        return {};
    }
    std::erase_if(m_actors, [](const std::weak_ptr<ActorState>& actor) {
        const std::shared_ptr<ActorState> state = actor.lock();
        return !state || state->revoked;
    });
    if (m_actors.size() >= kMaximumActors) {
        return {};
    }
    Actor actor;
    actor.m_state = std::make_shared<ActorState>(ActorState{m_identity, policy, false});
    m_actors.push_back(actor.m_state);
    return actor;
}

TxCoordinator::Producer TxCoordinator::registerProducer(bool continuousMicrophone)
{
    if (!onThread() || !m_identity->alive.load(std::memory_order_acquire)) {
        return {};
    }
    std::erase_if(m_producers, [](const std::weak_ptr<ProducerState>& weak) {
        const std::shared_ptr<ProducerState> producer = weak.lock();
        return !producer || !producer->valid.load(std::memory_order_acquire);
    });
    if (m_producers.size() >= kMaximumProducers) {
        return {};
    }
    Producer producer;
    producer.m_state = std::make_shared<ProducerState>();
    producer.m_state->coordinator = m_identity;
    producer.m_state->continuousMicrophone = continuousMicrophone;
    m_producers.push_back(producer.m_state);
    return producer;
}

bool TxCoordinator::acceptsRequest(const Request& request) const
{
    return onThread() && request.valid() && request.m_state->identity == m_identity;
}

std::vector<TxCoordinator::Request> TxCoordinator::producerRequests(const Producer& producer) const
{
    std::vector<Request> requests;
    if (!onThread() || !producer.m_state || producer.m_state->coordinator.lock() != m_identity) {
        return requests;
    }
    std::lock_guard<std::mutex> lock(producer.m_state->requestMutex);
    for (const auto& weak : producer.m_state->requests) {
        Request request;
        request.m_state = weak.lock();
        if (request.m_state) {
            requests.push_back(std::move(request));
        }
    }
    return requests;
}

void TxCoordinator::setProducerAdmissionObserver(const Producer& producer, std::function<void()> observer)
{
    if (onThread() && producer.m_state && producer.m_state->coordinator.lock() == m_identity) {
        producer.m_state->admitted = std::move(observer);
    }
}

void TxCoordinator::notifyProducerAdmission(const Request& request)
{
    if (acceptsRequest(request) && requestIntent(request).pending()) {
        const std::function<void()> observer = request.m_state->producer.m_state->admitted;
        if (observer) {
            observer();
        }
    }
}

bool TxCoordinator::hasForeignIntents(const Operation& operation, const Request* request,
                                      unsigned activities) const
{
    if (!onThread()) {
        return true;
    }
    const std::shared_ptr<ProducerState> producer = request && request->m_state
        ? request->m_state->producer.m_state : nullptr;
    return std::any_of(m_intents.begin(), m_intents.end(),
        [&operation, &producer, activities](const std::shared_ptr<IntentState>& intent) {
            return intent->producer != producer && !intent->ended.load(std::memory_order_acquire)
                && intent->operation.sameOperation(operation)
                && (activities == 0 || (activities & static_cast<unsigned>(intent->activity)));
        });
}

TxCoordinator::Intent TxCoordinator::requestIntent(const Request& request) const
{
    Intent intent;
    if (onThread() && request.m_state && request.m_state->identity == m_identity
        && request.m_state->bound.load(std::memory_order_acquire)) {
        intent.m_state = request.m_state->intent;
    }
    return intent;
}

bool TxCoordinator::ownsRequest(const Request& request) const
{
    return request.m_state && request.m_state->identity == m_identity;
}

TxCoordinator::Intent TxCoordinator::beginRequest(const Request& request,
                                                  const Operation& operation, Activity activity)
{
    if (!acceptsRequest(request)) {
        return {};
    }
    Intent intent = requestIntent(request);
    if (intent.m_state) {
        return intent.pending() && intent.m_state->activity == activity
            && intent.m_state->operation.sameOperation(operation) ? intent : Intent{};
    }
    Request input;
    input.m_state = request.m_state->input;
    intent = beginIntent(operation, {}, activity, request.m_state->producer.m_state,
        input.m_state ? std::function<bool()>([input] { return input.valid(); }) : std::function<bool()>{},
        request.m_state->producerEpoch);
    if (intent.pending()) {
        request.m_state->intent = intent.m_state;
        request.m_state->bound.store(true, std::memory_order_release);
        m_boundRequests.push_back(request);
    }
    return intent;
}

TxCoordinator::Operation TxCoordinator::requestOperation(const Request& request) const
{
    const Intent intent = requestIntent(request);
    if (!intent.m_state) {
        return {};
    }
    Operation operation = intent.m_state->operation;
    operation.m_producer = request.m_state->producer.m_state;
    operation.m_intent = intent.m_state;
    return operation;
}

TxCoordinator::Context TxCoordinator::mediaContext(const Request& request) const
{
    if (!onThread() || !request.m_state || request.m_state->identity != m_identity) {
        return {};
    }
    return mediaContext(request.m_state->producer, requestOperation(request));
}

TxCoordinator::Intent TxCoordinator::closeRequest(const Request& request)
{
    if (!onThread() || !request.m_state || request.m_state->identity != m_identity
        || request.m_state->closed.exchange(true)) {
        return {};
    }
    const Intent intent = requestIntent(request);
    (void)requestIntentEnd(intent);
    return intent;
}

bool TxCoordinator::hasOtherIntents(const Operation& operation, const Intent& excluded,
                                     unsigned activities, bool includeFinishing) const
{
    return onThread() && std::any_of(m_intents.begin(), m_intents.end(),
        [&operation, &excluded, activities, includeFinishing](const std::shared_ptr<IntentState>& intent) {
            return intent != excluded.m_state && !intent->ended.load(std::memory_order_acquire)
                && (includeFinishing || !intent->finishing)
                && (activities == 0 || (activities & static_cast<unsigned>(intent->activity)))
                && intent->operation.sameOperation(operation);
        });
}

TxCoordinator::Context TxCoordinator::mediaContext(const Producer& producer, const Operation& operation) const
{
    if (!onThread() || recovering() || !producer.valid() || producer.m_state->coordinator.lock() != m_identity
        || (!producer.m_state->continuousMicrophone
            && (!m_active.sameOperation(operation) || m_active.m_state->cancelled.load(std::memory_order_acquire)))) {
        return {};
    }
    Context context;
    context.m_producer = producer;
    context.m_session = m_identity->session.load(std::memory_order_acquire);
    context.m_continuous = producer.m_state->continuousMicrophone;
    if (!context.m_continuous) {
        context.m_operation = operation;
    }
    return context;
}

TxCoordinator::Admission TxCoordinator::acquire(const Actor& actor, qint64 now)
{
    if (!onThread()) {
        return {{}, Refusal::WrongThread};
    }
    if (!validActor(actor) || now < 0) {
        return {{}, Refusal::InvalidActor};
    }
    if (!actor.m_state->policy.mayTransmit) {
        return {{}, Refusal::Denied};
    }
    expire(now);
    // stopHandler may synchronously revoke the requesting actor.
    if (!validActor(actor)) {
        return {{}, Refusal::InvalidActor};
    }
    if (m_stopping.m_state || m_inStopHandler) {
        return {{}, Refusal::Recovering};
    }
    if (m_active.m_state) {
        if (m_active.m_state->actor != actor.m_state) {
            return {{}, Refusal::Busy};
        }
        return {m_active, Refusal::None};
    }
    if (m_unconfirmed.m_state && m_unconfirmed.m_state->actor != actor.m_state) {
        // Local queue completion says nothing about a radio-buffered tail.
        // Keep the preceding owner until qualified stop evidence arrives.
        return {{}, Refusal::Busy};
    }
    if (m_identity->generation.load() == std::numeric_limits<quint64>::max()) {
        return {{}, Refusal::Recovering};
    }
    quint64 expected = 0;
    if (!m_identity->dispatches.compare_exchange_strong(
            expected, Identity::kChangingGeneration, std::memory_order_acquire)) {
        // No delayed acquisition: an in-flight old write requires fresh intent
        // after it returns, not a surprise transmission when a queue drains.
        return {{}, Refusal::Recovering};
    }
    const auto releaseGeneration = qScopeGuard([this] {
        m_identity->dispatches.store(0, std::memory_order_release);
    });
    m_active.m_state = std::make_shared<OperationState>();
    m_active.m_state->actor = actor.m_state;
    m_active.m_state->startedMs = m_unconfirmed.m_state
        ? m_unconfirmed.m_state->startedMs : now;
    m_active.m_state->maximumMs = actor.m_state->policy.maximumOperationMs;
    m_active.m_state->generation = ++m_identity->generation;
    m_unconfirmed = {};
    return {m_active, Refusal::None};
}

bool TxCoordinator::owns(const Actor& actor, const Operation& operation) const
{
    return onThread() && validActor(actor)
        && ((m_active.sameOperation(operation) && m_active.m_state->actor == actor.m_state)
            || (m_unconfirmed.sameOperation(operation)
                && m_unconfirmed.m_state->actor == actor.m_state));
}

TxCoordinator::Intent TxCoordinator::beginIntent(const Operation& operation,
                                                const Intent& previous, Activity activity)
{
    return beginIntent(operation, previous, activity, {});
}

TxCoordinator::Intent TxCoordinator::beginIntent(const Operation& operation,
                                                const Intent& previous, Activity activity,
                                                const std::shared_ptr<ProducerState>& producer,
                                                std::function<bool()> inputPermit, quint64 producerEpoch)
{
    const unsigned activityBit = static_cast<unsigned>(activity);
    if (!onThread() || !m_active.sameOperation(operation)
        || m_active.m_state->cancelled.load(std::memory_order_acquire)
        || activityBit == 0 || activityBit > static_cast<unsigned>(Activity::Cwx)
        || (activityBit & (activityBit - 1)) != 0) {
        return {};
    }
    if (previous.pending()) {
        // An unrelated live handle is not this producer's reusable slot.
        if (previous.m_state->operation.sameOperation(operation)
            && previous.m_state->activity == activity
            && std::find(m_intents.begin(), m_intents.end(), previous.m_state) != m_intents.end()) {
            if (!previous.m_state->finishing) {
                return previous;
            }
        } else {
            return {};
        }
    }
    if (m_intents.size() >= kMaximumIntents) {
        return {};
    }
    Intent intent;
    intent.m_state = std::make_shared<IntentState>();
    intent.m_state->operation = operation;
    intent.m_state->producer = producer;
    intent.m_state->producerEpoch = producerEpoch;
    intent.m_state->activity = activity;
    intent.m_state->inputPermit = std::move(inputPermit);
    m_intents.push_back(intent.m_state);
    auto holds = std::make_shared<std::vector<std::weak_ptr<IntentState>>>();
    holds->reserve(m_intents.size());
    for (const std::shared_ptr<IntentState>& entry : m_intents) {
        if (entry->operation.sameOperation(operation)) {
            holds->push_back(entry);
        }
    }
    std::atomic_store_explicit(&operation.m_state->holds,
        std::shared_ptr<const std::vector<std::weak_ptr<IntentState>>>(std::move(holds)),
        std::memory_order_release);
    return intent;
}

bool TxCoordinator::requestIntentEnd(const Intent& intent)
{
    if (!onThread() || !intent.m_state
        || std::find(m_intents.begin(), m_intents.end(), intent.m_state) == m_intents.end()) {
        return false;
    }
    intent.m_state->finishing = true;
    return true;
}

bool TxCoordinator::endIntent(const Intent& intent)
{
    if (!onThread() || !intent.m_state) {
        return false;
    }
    const auto found = std::find(m_intents.begin(), m_intents.end(), intent.m_state);
    if (found == m_intents.end()) {
        return false;
    }
    intent.m_state->ended.store(true, std::memory_order_release);
    m_intents.erase(found);
    std::erase_if(m_boundRequests, [&intent](const Request& request) {
        return request.m_state->intent == intent.m_state;
    });
    return true;
}

bool TxCoordinator::hasIntents(const Operation& operation) const
{
    return onThread() && std::any_of(m_intents.begin(), m_intents.end(),
        [&operation](const std::shared_ptr<IntentState>& intent) {
            return intent->operation.sameOperation(operation);
        });
}

unsigned TxCoordinator::activeActivities(const Operation& operation) const
{
    if (!onThread()) {
        return 0;
    }
    unsigned activities = 0;
    for (const std::shared_ptr<IntentState>& intent : m_intents) {
        if (intent->operation.sameOperation(operation)) {
            activities |= static_cast<unsigned>(intent->activity);
        }
    }
    return activities;
}

void TxCoordinator::endIntents(const Operation& operation)
{
    std::erase_if(m_intents, [&operation](const std::shared_ptr<IntentState>& intent) {
        if (!intent->operation.sameOperation(operation)) {
            return false;
        }
        intent->ended.store(true, std::memory_order_release);
        return true;
    });
    std::erase_if(m_boundRequests, [&operation](const Request& request) {
        return request.m_state->intent->operation.sameOperation(operation);
    });
}

TxCoordinator::Operation TxCoordinator::cleanupFence() const
{
    if (!onThread()) {
        return {};
    }
    Operation fence;
    fence.m_state = std::make_shared<OperationState>();
    fence.m_state->actor = std::make_shared<ActorState>(ActorState{m_identity, {}, true});
    fence.m_state->cancelled.store(true, std::memory_order_release);
    fence.m_state->generation = m_identity->generation.load(std::memory_order_acquire);
    return fence;
}

bool TxCoordinator::finishLocalIntent(const Operation& operation)
{
    if (!onThread() || !m_active.sameOperation(operation) || hasIntents(operation)) {
        return false;
    }
    m_active.m_state->cancelled.store(true, std::memory_order_release);
    m_unconfirmed = m_active;
    m_active = {};
    return true;
}

void TxCoordinator::finishLocalIntents(const Operation& operation)
{
    if (!onThread() || !operation.permitsCleanup()) {
        return;
    }
    endIntents(operation);
    (void)finishLocalIntent(operation);
}

void TxCoordinator::discardCapturedRequests()
{
    if (onThread()) {
        m_identity->inputEpoch.fetch_add(1, std::memory_order_acq_rel);
    }
}

void TxCoordinator::stop(StopReason reason)
{
    if (!m_active.m_state && !m_unconfirmed.m_state) {
        return;
    }
    m_stopping = m_active.m_state ? m_active : m_unconfirmed;
    m_active = {};
    m_unconfirmed = {};
    m_stopping.m_state->cancelled.store(true, std::memory_order_release);
    // Publish recovery state before invoking user code. Reentrant requests may
    // not acquire and get unkeyed by cleanup for the preceding operation.
    const Operation stopping = m_stopping;
    m_inStopHandler = true;
    m_stopHandler(stopping, reason);
    endIntents(stopping);
    m_inStopHandler = false;
}

bool TxCoordinator::cancel(const Actor& actor, const Operation& operation)
{
    if (!owns(actor, operation)) {
        return false;
    }
    stop(StopReason::OwnerCancelled);
    return true;
}

void TxCoordinator::revoke(const Actor& actor)
{
    if (!onThread() || !validActor(actor)) {
        return;
    }
    actor.m_state->revoked = true;
    if ((m_active.m_state && m_active.m_state->actor == actor.m_state)
        || (m_unconfirmed.m_state && m_unconfirmed.m_state->actor == actor.m_state)) {
        stop(StopReason::ActorRevoked);
    }
}

void TxCoordinator::expire(qint64 now)
{
    if (!onThread()) {
        return;
    }
    if (m_active.m_state && !m_active.permitsDispatch(now)) {
        stop(StopReason::Expired);
    } else if (m_unconfirmed.m_state) {
        // Its dispatch fence is already cancelled, so check the original
        // deadline rather than treating cancellation itself as expiration.
        const OperationState& state = *m_unconfirmed.m_state;
        if (now < state.startedMs
            || (state.maximumMs > 0 && now - state.startedMs >= state.maximumMs)) {
            stop(StopReason::Expired);
        }
    }
}

void TxCoordinator::reset()
{
    if (onThread()) {
        if (m_identity->session.load() == std::numeric_limits<quint64>::max()) {
            m_identity->alive.store(false, std::memory_order_release);
        } else {
            ++m_identity->session;
        }
        stop(StopReason::Reset);
        // A transport can survive a reconnect. Fence old queued key-ups too,
        // before that same socket is ever reused for a new radio connection.
        if (m_identity->generation.load() < std::numeric_limits<quint64>::max()) {
            ++m_identity->generation;
        }
    }
}

void TxCoordinator::emergencyStop()
{
    if (onThread()) {
        stop(StopReason::Emergency);
    }
}

bool TxCoordinator::acknowledgeStopped(const Operation& operation)
{
    if (!onThread()) {
        return false;
    }
    quint64 expected = 0;
    if (!m_identity->dispatches.compare_exchange_strong(
            expected, Identity::kChangingGeneration, std::memory_order_acquire)) {
        return false;
    }
    const auto releaseGeneration = qScopeGuard([this] {
        m_identity->dispatches.store(0, std::memory_order_release);
    });
    expected = 0;
    if (!m_identity->continuousDispatches.compare_exchange_strong(
            expected, Identity::kChangingGeneration, std::memory_order_acquire)) {
        return false;
    }
    const auto releaseContinuous = qScopeGuard([this] {
        m_identity->continuousDispatches.store(0, std::memory_order_release);
    });
    if (m_stopping.sameOperation(operation)) {
        m_stopping = {};
        return true;
    }
    if (m_unconfirmed.sameOperation(operation)) {
        m_unconfirmed = {};
        return true;
    }
    return false;
}

bool TxCoordinator::recovering() const
{
    return onThread() && bool(m_stopping.m_state);
}

bool TxCoordinator::hasInFlightDispatches() const
{
    return (m_identity->dispatches.load(std::memory_order_acquire)
            & ~Identity::kChangingGeneration) != 0
        || (m_identity->continuousDispatches.load(std::memory_order_acquire)
            & ~Identity::kChangingGeneration) != 0;
}

} // namespace AetherSDR
