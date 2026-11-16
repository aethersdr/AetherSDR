#include "AetherPDRunLease.h"
#include "RadioModel.h"

#include <QScopeGuard>
#include <QThread>
#include <algorithm>
#include <utility>

namespace AetherSDR {
namespace {
// Start helper cleanup before the authorization ceiling. Its ownership check,
// unkey write and radio release can finish later; they never extend TX authority.
constexpr qint64 kStartupReserveMs = 1000;
constexpr qint64 kMinimumKeyWindowMs = 1000;
constexpr qint64 kMaximumRunMs = txlin::TxLinearityController::kMaxTransmitMs;
}

bool AetherPDRunLease::Gate::current()
{
    if (retired.load(std::memory_order_acquire)) { return false; }
    const qint64 now = TxCoordinator::monotonicMs();
    if (now < 0 || now >= deadline.load(std::memory_order_acquire)) {
        retired.store(true, std::memory_order_release);
        return false;
    }
    return true;
}

std::shared_ptr<AetherPDRunLease> AetherPDRunLease::create(RadioModel& radio, RadioControlSession session,
    std::unique_ptr<txlin::AetherPDTestClient> helper, std::function<bool()> runCurrent)
{
    if (QThread::currentThread() != radio.thread()) { return {}; }
    if (!helper) { helper = std::make_unique<txlin::AetherPDTestClient>(); }
    if (helper->thread() != radio.thread() || helper->parent()
        || helper->state() != txlin::AetherPDTestClient::State::Idle) { return {}; }
    std::shared_ptr<AetherPDRunLease> lease = std::make_shared<AetherPDRunLease>(
        ConstructionKey{}, radio, session, std::move(helper), std::move(runCurrent));
    return lease->current() ? lease : nullptr;
}

AetherPDRunLease::AetherPDRunLease(ConstructionKey, RadioModel& radio, RadioControlSession session,
    std::unique_ptr<txlin::AetherPDTestClient> helper, std::function<bool()> runCurrent)
    : m_radio(&radio), m_session(session), m_helper(std::move(helper)), m_timer(this)
{
    const std::function<bool()> bound = radio.bindAetherPDTest(session, *m_helper);
    m_current = [bound, runCurrent = std::move(runCurrent)] {
        return bound && bound() && (!runCurrent || runCurrent());
    };
    m_handle = radio.ourClientHandle();
    m_helper->setChangedCallback([this] { poll(); });
    m_timer.setInterval(25);
    connect(&m_timer, &QTimer::timeout, this, &AetherPDRunLease::poll);
    connect(&radio, &RadioModel::radioTransmittingChanged, this, [this](bool) { poll(); });
    connect(&radio, &RadioModel::radioTransmitConfirmed, this, [this](bool) { poll(); });
}

AetherPDRunLease::~AetherPDRunLease()
{
    release();
    if (m_manager) { m_manager->disconnectClient(m_client); }
}

bool AetherPDRunLease::current() const
{
    return QThread::currentThread() == thread() && m_radio && m_handle
        && m_radio->controlSessionCurrent(m_session) && m_current && m_current();
}

bool AetherPDRunLease::start(std::int64_t absoluteDeadlineMs)
{
    const std::shared_ptr<AetherPDRunLease> keepAlive = weak_from_this().lock();
    if (QThread::currentThread() != thread() || m_started || m_releasing) { return false; }
    m_started = true;
    const qint64 now = TxCoordinator::monotonicMs();
    const qint64 duration = absoluteDeadlineMs > now ? absoluteDeadlineMs - now : 0;
    if (!current() || duration > kMaximumRunMs
        || duration < kStartupReserveMs + kMinimumKeyWindowMs
        || !m_radio->independentTxReady() || m_radio->isRadioTransmitting()) {
        fail(QStringLiteral("Original session or bounded test budget is unavailable")); return false;
    }
    m_deadline = absoluteDeadlineMs;
    m_gate->deadline.store(m_deadline, std::memory_order_release);
    m_manager = m_radio->independentTxGrants();
    if (!m_manager) { fail(QStringLiteral("Independent transmit admission is unavailable")); return false; }
    m_client = m_manager->registerClient(QStringLiteral("AetherPD operator measurement"),
        [gate = m_gate] { return gate->current(); });
    const TxGrantManager::Issuance issued = m_manager->issue(m_client,
        {duration, duration, duration, static_cast<unsigned>(TxCoordinator::Activity::Mox)});
    if (!issued.accepted()) { fail(QStringLiteral("Bounded transmit grant refused")); return false; }
    m_grant = issued.grant;
    const TxGrantManager::Admission admission = m_manager->acquire(m_client, m_grant, 1, TxCoordinator::Activity::Mox);
    if (!admission.accepted()) { fail(QStringLiteral("Original transmit operation refused")); return false; }
    m_operation = admission.operation;
    m_admitted = true;
    m_input = TxController::fromGrantedPtt(m_radio, admission.input, m_operation);
    if (!m_input.valid()) { fail(QStringLiteral("Original transmit input is unavailable")); return false; }
    m_timer.start();
    const int cleanupAfterMs = static_cast<int>(duration - kStartupReserveMs);
    if (!m_helper->begin(cleanupAfterMs)) { fail(QStringLiteral("Independent helper refused startup")); return false; }
    return !m_releasing;
}

void AetherPDRunLease::fail(const QString& error)
{
    if (m_error.isEmpty()) { m_error = error; }
    release();
}

void AetherPDRunLease::release()
{
    const std::shared_ptr<AetherPDRunLease> keepAlive = weak_from_this().lock();
    m_gate->retired.store(true, std::memory_order_release);
    if (QThread::currentThread() != thread() || m_releasing) { return; }
    m_releasing = true;
    m_timer.stop();
    if (m_admitted && m_manager) { (void)m_manager->release(m_client, m_grant, m_operation); }
    m_input.stop();
    m_helper->requestStop();
    if (m_manager) { m_manager->disconnectClient(m_client); }
}

AetherPDRunLease::State AetherPDRunLease::state() const
{
    const std::shared_ptr<const AetherPDRunLease> keepAlive = weak_from_this().lock();
    if (QThread::currentThread() != thread()) { return State::Failed; }
    if (m_operation.hasConfirmedStop() || (m_releasing && !m_admitted)) { return State::Released; }
    if (m_releasing || !current() || (m_started && !m_gate->current())) { return State::Failed; }
    if (m_keyRequested && m_helper->remainingMs() <= 0) { return State::Failed; }
    if (m_keyRequested && m_keyObserved && m_input.active()
        && m_radio->transmitOperation().sameOperation(m_operation)
        && m_radio->txClientHandle() == m_handle && m_radio->isRadioTransmitting()) {
        return State::Keyed;
    }
    return State::AwaitingKey;
}

void AetherPDRunLease::poll()
{
    const std::shared_ptr<AetherPDRunLease> keepAlive = weak_from_this().lock();
    if (QThread::currentThread() != thread() || !m_started || m_releasing || m_polling) { return; }
    m_polling = true;
    const auto polling = qScopeGuard([this] { m_polling = false; });
    if (!current() || !m_gate->current() || !m_operation.permitsDispatch(TxCoordinator::monotonicMs())) {
        fail(QStringLiteral("Original test authority expired")); return;
    }
    using HelperState = txlin::AetherPDTestClient::State;
    if (m_helper->state() == HelperState::Starting || m_helper->state() == HelperState::Verifying) { return; }
    const qint64 now = TxCoordinator::monotonicMs();
    const qint64 remaining = m_helper->remainingMs();
    const qint64 cleanupUpper = m_helper->cleanupRemainingUpperMs();
    if (remaining <= 0 || cleanupUpper <= 0 || now >= m_deadline
        || cleanupUpper > m_deadline - now) {
        fail(QStringLiteral("Helper timing cannot fit the original test deadline")); return;
    }
    if (!m_keyRequested) {
        if (remaining < kMinimumKeyWindowMs || m_radio->isRadioTransmitting()) {
            fail(QStringLiteral("Helper startup left no idle measurement window")); return;
        }
        m_gate->deadline.store(std::min(m_deadline, now + remaining), std::memory_order_release);
        m_keyRequested = true;
        if (!m_input.start()) { fail(QStringLiteral("Existing transmit gate refused the original test input")); return; }
    }
    if (m_releasing) { return; }
    if (!current()) { fail(QStringLiteral("Original session retired during keying")); return; }
    if (m_radio->isRadioTransmitting() && m_radio->txClientHandle()
        && m_radio->txClientHandle() != m_handle) {
        fail(QStringLiteral("Test transmission belongs to another GUI owner")); return;
    }
    const bool keyed = m_radio->transmitOperation().sameOperation(m_operation)
        && m_input.active() && m_radio->txClientHandle() == m_handle && m_radio->isRadioTransmitting();
    if (keyed) { m_keyObserved = true; }
    else if (m_keyObserved) { fail(QStringLiteral("Original test transmission ended")); }
}
} // namespace AetherSDR
