#include "TxGrantManager.h"

#include <QScopeGuard>

#include <algorithm>
#include <limits>
#include <utility>

namespace AetherSDR {

struct TxGrantManager::Identity {};
struct TxGrantManager::ClientState {
    std::weak_ptr<Identity> manager;
    QString principal;
    AuthorizationCurrent authorizationCurrent;
    bool connected{true};
};
struct TxGrantManager::GrantState {
    std::shared_ptr<ClientState> client;
    TxCoordinator::Actor actor;
    TxCoordinator::Producer producer;
    TxCoordinator::Operation operation;
    TxCoordinator::Request input;
    Policy policy;
    qint64 issuedMs{0};
    qint64 expiresMs{0};
    qint64 lastKeepAliveMs{0};
    qint64 operationStartedMs{0};
    quint64 highWater{0};
    bool retired{false};
};

TxGrantManager::TxGrantManager(TxCoordinator& coordinator, QualifiedActivities qualifiedActivities,
                               QObject* parent)
    : QObject(parent), m_coordinator(coordinator)
    , m_qualifiedActivities(std::move(qualifiedActivities))
    , m_identity(std::make_shared<Identity>())
    , m_timer(this)
{
    m_timer.setSingleShot(true);
    m_timer.setTimerType(Qt::PreciseTimer);
    connect(&m_timer, &QTimer::timeout, this, &TxGrantManager::processDeadlines);
}

TxGrantManager::~TxGrantManager()
{
    m_closing = true;
    m_timer.stop();
    retire(m_grants);
}

bool TxGrantManager::onThread() const
{
    return QThread::currentThread() == thread();
}

bool TxGrantManager::validClient(const Client& client) const
{
    return client.m_state && client.m_state->manager.lock() == m_identity
        && client.m_state->connected && !m_closing;
}

bool TxGrantManager::owns(const Client& client, const Grant& grant) const
{
    return validClient(client) && grant.m_state && !grant.m_state->retired
        && grant.m_state->client == client.m_state;
}

bool TxGrantManager::live(const GrantState& grant, qint64 now) const
{
    return !grant.retired && grant.client->connected && now >= grant.issuedMs
        && now >= grant.lastKeepAliveMs && now < grant.expiresMs
        && now - grant.lastKeepAliveMs < grant.policy.keepAliveMs
        && grant.actor.permitsDispatch(now);
}

TxGrantManager::Client TxGrantManager::registerClient(const QString& verifiedPrincipal,
                                                     AuthorizationCurrent authorizationCurrent)
{
    if (!onThread() || m_mutating || m_closing || verifiedPrincipal.isEmpty()
        || verifiedPrincipal.size() > 256 || m_clients.size() >= kMaximumClients
        || (authorizationCurrent && !authorizationCurrent())) {
        return {};
    }
    Client client;
    client.m_state = std::make_shared<ClientState>();
    client.m_state->manager = m_identity;
    client.m_state->principal = verifiedPrincipal;
    client.m_state->authorizationCurrent = std::move(authorizationCurrent);
    m_clients.push_back(client.m_state);
    return client;
}

TxGrantManager::Issuance TxGrantManager::issue(const Client& client, Policy policy)
{
    if (!onThread()) { return {{}, Error::WrongThread}; }
    if (m_mutating || !validClient(client)) { return {{}, Error::InvalidClient}; }
    processDeadlines();
    if (!validClient(client) || (client.m_state->authorizationCurrent
        && !client.m_state->authorizationCurrent())) { return {{}, Error::InvalidClient}; }
    const qint64 now = m_coordinator.currentTimeMs();
    if (policy.maximumOperationMs <= 0 || policy.maximumOperationMs > kMaximumOperationMs
        || policy.lifetimeMs <= 0 || policy.lifetimeMs > kMaximumGrantMs
        || policy.keepAliveMs <= 0 || policy.keepAliveMs > kMaximumKeepAliveMs
        || policy.activities == 0 || (policy.activities & ~TxCoordinator::kAllActivities)
        || now < 0 || now > std::numeric_limits<qint64>::max() - policy.lifetimeMs) {
        return {{}, Error::InvalidPolicy};
    }
    const unsigned qualified = m_qualifiedActivities ? m_qualifiedActivities() : 0;
    if ((policy.activities & qualified) != policy.activities) {
        return {{}, Error::Unsupported};
    }
    if (std::any_of(m_grants.begin(), m_grants.end(), [&client](const auto& entry) {
            return !entry->retired && entry->client == client.m_state;
        })) {
        return {{}, Error::Conflict};
    }
    Grant grant;
    grant.m_state = std::make_shared<GrantState>();
    GrantState& state = *grant.m_state;
    state.client = client.m_state;
    state.policy = policy;
    state.issuedMs = now;
    state.lastKeepAliveMs = now;
    state.expiresMs = now + policy.lifetimeMs;
    state.actor = m_coordinator.registerActor({true, policy.maximumOperationMs,
        state.expiresMs, policy.activities, true, client.m_state->authorizationCurrent});
    state.producer = m_coordinator.registerProducer(state.actor);
    if (!state.actor.permitsDispatch(now) || !state.producer.valid()
        || !m_coordinator.refreshLiveness(state.actor, now,
            now + std::min(policy.keepAliveMs, policy.lifetimeMs))) {
        m_coordinator.revoke(state.actor);
        return {{}, Error::Capacity};
    }
    m_grants.push_back(grant.m_state);
    schedule();
    return {grant, Error::None};
}

TxGrantManager::Admission TxGrantManager::acquire(const Client& client, const Grant& grant,
                                                 quint64 intentSerial, TxCoordinator::Activity activity)
{
    if (!onThread()) { return {{}, {}, {}, Error::WrongThread}; }
    if (m_mutating || !owns(client, grant)) { return {}; }
    const bool wasLive = live(*grant.m_state, m_coordinator.currentTimeMs());
    processDeadlines();
    if (!wasLive || !owns(client, grant)) { return {{}, {}, {}, Error::Expired}; }
    GrantState& state = *grant.m_state;
    // Consume even refused intent. A Busy request must never become an
    // automatically delayed transmission when another owner releases.
    if (intentSerial == 0 || intentSerial <= state.highWater) {
        return {{}, {}, {}, Error::Replay};
    }
    state.highWater = intentSerial;
    const unsigned bit = static_cast<unsigned>(activity);
    const unsigned qualified = m_qualifiedActivities ? m_qualifiedActivities() : 0;
    if (bit == 0 || (bit & (bit - 1)) || !(state.policy.activities & bit)
        || !(qualified & bit)) {
        return {{}, {}, {}, Error::Unsupported};
    }
    if (state.input.valid()) { return {{}, {}, {}, Error::Conflict}; }
    const qint64 now = m_coordinator.currentTimeMs();
    const TxCoordinator::Admission admission = m_coordinator.acquire(state.actor, now);
    if (!admission.accepted()) {
        return {{}, {}, admission.refusal, Error::None};
    }
    if (!state.operation.sameOperation(admission.operation)) {
        state.operationStartedMs = now;
    }
    state.operation = admission.operation;
    state.input = state.producer.request();
    const TxCoordinator::Intent intent = m_coordinator.beginRequest(state.input, state.operation, activity);
    if (!intent.pending()) {
        (void)m_coordinator.cancel(state.actor, state.operation);
        // An unbound request stays valid after cancel; do not let it block
        // every later fresh intent under this otherwise-live grant.
        state.input = {};
        schedule();
        return {{}, {}, {}, Error::Capacity};
    }
    schedule();
    return {m_coordinator.requestOperation(state.input), state.input,
        TxCoordinator::Refusal::None, Error::None};
}

bool TxGrantManager::release(const Client& client, const Grant& grant,
                              const TxCoordinator::Operation& operation)
{
    if (!onThread() || m_mutating || !owns(client, grant)
        || !grant.m_state->operation.sameOperation(operation)) { return false; }
    // This API is cancellation/stop, not a local-queue completion claim. The
    // engine stop handler must provide qualified evidence before handoff.
    (void)m_coordinator.closeRequest(grant.m_state->input);
    return m_coordinator.cancel(grant.m_state->actor, grant.m_state->operation);
}

bool TxGrantManager::cancel(const Client& client, const Grant& grant,
                             const TxCoordinator::Operation& operation)
{
    return release(client, grant, operation);
}

bool TxGrantManager::keepAlive(const Client& client, const Grant& grant)
{
    if (!onThread() || m_mutating || !owns(client, grant)) { return false; }
    processDeadlines();
    const qint64 now = m_coordinator.currentTimeMs();
    if (!owns(client, grant) || !live(*grant.m_state, now)
        || !m_coordinator.refreshLiveness(grant.m_state->actor, now,
            now + std::min(grant.m_state->policy.keepAliveMs, grant.m_state->expiresMs - now))) {
        return false;
    }
    grant.m_state->lastKeepAliveMs = now;
    schedule();
    return true;
}

void TxGrantManager::retire(const std::vector<std::shared_ptr<GrantState>>& grants)
{
    // Fence every affected producer before the first stop callback. Those
    // callbacks can re-enter the manager, but cannot issue/admit fresh work.
    const auto pending = grants;
    const bool wasMutating = m_mutating;
    m_mutating = true;
    const auto restore = qScopeGuard([this, wasMutating] { m_mutating = wasMutating; });
    for (const auto& grant : pending) {
        grant->retired = true;
        grant->producer.invalidate();
    }
    for (const auto& grant : pending) {
        m_coordinator.revoke(grant->actor);
    }
    std::erase_if(m_grants, [](const auto& grant) { return grant->retired; });
}

bool TxGrantManager::revoke(const Grant& grant)
{
    if (!onThread() || m_mutating || !grant.m_state || grant.m_state->retired
        || grant.m_state->client->manager.lock() != m_identity) {
        return false;
    }
    retire({grant.m_state});
    schedule();
    return true;
}

void TxGrantManager::disconnectClient(const Client& client)
{
    if (!onThread() || !validClient(client)) { return; }
    client.m_state->connected = false;
    std::vector<std::shared_ptr<GrantState>> pending;
    for (const auto& grant : m_grants) {
        if (grant->client == client.m_state) { pending.push_back(grant); }
    }
    retire(pending);
    std::erase(m_clients, client.m_state);
    schedule();
}

void TxGrantManager::invalidateRadio()
{
    if (!onThread()) { return; }
    retire(m_grants);
    schedule();
}

void TxGrantManager::processDeadlines()
{
    if (!onThread() || m_mutating || m_closing) { return; }
    const qint64 now = m_coordinator.currentTimeMs();
    std::vector<std::shared_ptr<GrantState>> pending;
    for (const auto& grant : m_grants) {
        if (!live(*grant, now)) { pending.push_back(grant); }
    }
    retire(pending);
    m_coordinator.expire(now);
    schedule();
}

void TxGrantManager::schedule()
{
    m_timer.stop();
    if (m_closing || m_grants.empty()) { return; }
    // Wake for the earliest deadline; keepalive never slides an operation or
    // grant deadline. Workers independently fence writes if this wake is late.
    qint64 delay = kMaximumGrantMs;
    const qint64 now = m_coordinator.currentTimeMs();
    for (const auto& grant : m_grants) {
        if (now < grant->issuedMs || now < grant->lastKeepAliveMs) {
            delay = 0;
            break;
        }
        delay = std::min(delay, now >= grant->expiresMs ? 0 : grant->expiresMs - now);
        const qint64 elapsed = now >= grant->lastKeepAliveMs ? now - grant->lastKeepAliveMs : 0;
        delay = std::min(delay, std::max(qint64{0}, grant->policy.keepAliveMs - elapsed));
        // Ownership outlives dispatch permission: expiry or local completion
        // must still schedule stop, including a deadline crossed since expire().
        // Already-stopping operations no longer own the coordinator and must
        // not create a zero-delay timer loop while awaiting backend evidence.
        if (m_coordinator.owns(grant->actor, grant->operation)) {
            delay = std::min(delay, std::max(qint64{0}, grant->policy.maximumOperationMs
                - (now - grant->operationStartedMs)));
        }
    }
    m_timer.start(static_cast<int>(delay));
}

int TxGrantManager::clientCount() const
{
    return onThread() ? static_cast<int>(m_clients.size()) : 0;
}

int TxGrantManager::grantCount() const
{
    return onThread() ? static_cast<int>(m_grants.size()) : 0;
}

bool TxGrantManager::isClient(const Client& client) const
{
    return onThread() && validClient(client);
}

bool TxGrantManager::isLive(const Client& client, const Grant& grant) const
{
    return onThread() && owns(client, grant) && live(*grant.m_state, m_coordinator.currentTimeMs());
}

} // namespace AetherSDR
