#include "TailnetShimOwnerKeyTransaction.h"
#include "AetherPDTransportPolicy.h"

#include <QPointer>
#include <QRandomGenerator>
#include <QThread>
#include <array>
#include <utility>

namespace AetherSDR {
namespace {
QString generateCandidate()
{
    std::array<quint32, 8> words{};
    QRandomGenerator::system()->fillRange(words.data(), words.size());
    const QByteArray bytes(reinterpret_cast<const char*>(words.data()), sizeof(words));
    return QString::fromLatin1(bytes.toHex());
}
}

TailnetShimOwnerKeyTransaction::Credentials TailnetShimOwnerKeyTransaction::systemCredentials()
{
    return {TailnetShimTokenStore::persistentStoreAvailable, TailnetShimTokenStore::load,
        TailnetShimTokenStore::revision, TailnetShimTokenStore::isDurableCurrent,
        TailnetShimTokenStore::createIfMissing, TailnetShimTokenStore::replaceIfCurrent,
        TailnetShimTokenStore::clearIfCurrent};
}
TailnetShimOwnerKeyTransaction::TailnetShimOwnerKeyTransaction(QObject* parent)
    : QObject(parent), m_client(this), m_credentials(systemCredentials()),
      m_acquire(TailnetShimClient::acquireProvisioningLease), m_generate(generateCandidate)
{
    initialize(30000);
}
TailnetShimOwnerKeyTransaction::TailnetShimOwnerKeyTransaction(QNetworkAccessManager& network, Credentials credentials,
        Acquire acquire, Generate generate, QObject* parent, int deadlineMs)
    : QObject(parent), m_client(network, this), m_credentials(std::move(credentials)),
      m_acquire(std::move(acquire)), m_generate(std::move(generate))
{
    initialize(deadlineMs);
}
void TailnetShimOwnerKeyTransaction::initialize(int deadlineMs)
{
    m_deadline.setParent(this);
    m_deadline.setSingleShot(true);
    m_deadline.setInterval(qBound(1, deadlineMs, 30000));
    connect(&m_deadline, &QTimer::timeout, this, [this] { end(Outcome::Unconfirmed, Reason::Deadline); });
    connect(&m_client, &TailnetShimClient::ownerKeyConfirmed, this, [this](const aetherpd::OwnerKeyReceipt&) {
        if (!current()) { return; }
        if (!sameCredentials()) { end(Outcome::Unconfirmed, Reason::ChangedCredential); return; }
        promote();
    });
    connect(&m_client, &TailnetShimClient::requestFailed, this,
        [this](const QString& operation, const QString&, bool unauthorized, const QByteArray&) {
        if (m_finished || operation != QLatin1String("owner-key") || !current()) { return; }
        // Only an explicit Resume authorizes this one replay of the identical
        // saved candidate. No response, reconnect or timer creates another key.
        if (m_confirming && unauthorized && m_action == Action::Resume && m_active != m_candidate
            && aetherpd::validAdminToken(m_active) && sameCredentials()) {
            submit(false);
        } else {
            end(Outcome::Unconfirmed, Reason::Remote);
        }
    });
}
bool TailnetShimOwnerKeyTransaction::begin(const Target& target, Action action)
{
    if (m_state != State::Idle || QThread::currentThread() != thread()) { return false; }
    m_reason = Reason::Unsupported;
    if (target.serial.isEmpty() || target.serial != target.serial.trimmed() || target.serial.toUtf8().size() > 512
        || !aetherpd::lanAddress(target.address) || !tailnetshim::supportsOwnerKeyReplacement(target.status)
        || !aetherpd::ownerKeyBody(target.status.installationId, QString(64, u'1')) || !target.sessionCurrent
        || !m_credentials.available || !m_credentials.load || !m_credentials.revision || !m_credentials.durable
        || !m_credentials.create || !m_credentials.replace || !m_credentials.retire || !m_acquire || !m_generate) { return false; }
    m_reason = Reason::Storage;
    if (!m_credentials.available()) { return false; }
    m_reason = Reason::Session;
    if (!target.sessionCurrent()) { return false; }
    m_reason = Reason::Busy;
    m_lease = m_acquire(target.serial);
    if (!m_lease) { return false; }
    m_target = target;
    m_action = action;
    m_reason = Reason::None;
    m_candidateId = target.serial + u'/' + target.status.installationId;
    m_client.setRadioAddress(target.address);
    m_deadline.start();
    if (transition(State::Loading)) { loadActive(); }
    return true;
}
bool TailnetShimOwnerKeyTransaction::transition(State state)
{
    m_state = state;
    const QPointer<TailnetShimOwnerKeyTransaction> guard(this);
    emit stateChanged(state);
    return guard && !m_finished;
}
bool TailnetShimOwnerKeyTransaction::current()
{
    if (m_finished) { return false; }
    if (!m_target.sessionCurrent()) { end(Outcome::SessionRetired, Reason::Session); return false; }
    return true;
}
bool TailnetShimOwnerKeyTransaction::sameCredentials() const
{
    return m_credentials.revision(m_target.serial, Entry::TailnetShim) == m_activeRevision
        && m_credentials.revision(m_candidateId, Entry::AetherPDOwnerCandidate) == m_candidateRevision;
}
void TailnetShimOwnerKeyTransaction::loadActive()
{
    m_credentials.load(m_target.serial, this, [guard = QPointer(this), lease = m_lease](const QString& token) {
        if (!guard || !guard->current()) { return; }
        guard->m_active = token;
        guard->m_activeRevision = guard->m_credentials.revision(guard->m_target.serial, Entry::TailnetShim);
        guard->loadCandidate();
    }, Entry::TailnetShim);
}
void TailnetShimOwnerKeyTransaction::loadCandidate()
{
    m_credentials.load(m_candidateId, this, [guard = QPointer(this), lease = m_lease](const QString& token) {
        if (!guard || !guard->current()) { return; }
        guard->m_candidate = token;
        guard->m_candidateRevision = guard->m_credentials.revision(guard->m_candidateId, Entry::AetherPDOwnerCandidate);
        if (!token.isEmpty()) {
            if (guard->m_action == Action::Replace) { guard->end(Outcome::Refused, Reason::PendingCandidate); return; }
            if (!aetherpd::ownerKeyBody(guard->m_target.status.installationId, token)) {
                guard->end(Outcome::Refused, Reason::Storage); return;
            }
            if (guard->m_credentials.durable(guard->m_candidateId, token, guard->m_candidateRevision, Entry::AetherPDOwnerCandidate)) {
                guard->verifyCandidate();
            } else { guard->prepareCandidate(false); }
        } else {
            if (guard->m_action == Action::Resume) { guard->end(Outcome::Refused, Reason::MissingCandidate); return; }
            if (!aetherpd::validAdminToken(guard->m_active)) { guard->end(Outcome::Refused, Reason::NoOwner); return; }
            guard->m_candidate = guard->m_generate();
            if (!aetherpd::ownerKeyBody(guard->m_target.status.installationId, guard->m_candidate)) {
                guard->end(Outcome::Refused, Reason::Storage); return;
            }
            guard->prepareCandidate(true);
        }
    }, Entry::AetherPDOwnerCandidate);
}
void TailnetShimOwnerKeyTransaction::prepareCandidate(bool creating)
{
    if (!current() || !transition(State::SavingCandidate) || !current()) { return; }
    const QPointer<TailnetShimOwnerKeyTransaction> guard(this);
    Done done = [guard, lease = m_lease](bool persisted, const QString&) {
        if (!guard || !guard->current()) { return; }
        if (!persisted) { guard->end(Outcome::Refused, Reason::Storage); return; }
        guard->verifyCandidate();
    };
    const bool admitted = creating
        ? m_credentials.create(m_candidateId, m_candidateRevision, m_candidate, this, std::move(done), Entry::AetherPDOwnerCandidate)
        : m_credentials.replace(m_candidateId, m_candidate, m_candidateRevision, m_candidate, this, std::move(done), Entry::AetherPDOwnerCandidate);
    if (!admitted && guard && !m_finished) { end(Outcome::Refused, Reason::Storage); }
}
void TailnetShimOwnerKeyTransaction::verifyCandidate()
{
    m_credentials.load(m_candidateId, this, [guard = QPointer(this), lease = m_lease](const QString& token) {
        if (!guard || !guard->current()) { return; }
        guard->m_candidateRevision = guard->m_credentials.revision(guard->m_candidateId, Entry::AetherPDOwnerCandidate);
        if (token != guard->m_candidate || !guard->m_credentials.durable(guard->m_candidateId, token,
                guard->m_candidateRevision, Entry::AetherPDOwnerCandidate) || !guard->sameCredentials()) {
            guard->end(Outcome::Unconfirmed, Reason::Storage); return;
        }
        guard->submit(guard->m_action == Action::Resume);
    }, Entry::AetherPDOwnerCandidate);
}
void TailnetShimOwnerKeyTransaction::submit(bool confirming)
{
    if (!current()) { return; }
    if (!sameCredentials() || !m_credentials.durable(m_candidateId, m_candidate, m_candidateRevision, Entry::AetherPDOwnerCandidate)) {
        end(Outcome::Unconfirmed, Reason::ChangedCredential); return;
    }
    m_confirming = confirming;
    if (!transition(confirming ? State::Confirming : State::Replacing)) { return; }
    if (!current()) { return; }
    if (!sameCredentials() || !m_credentials.durable(m_candidateId, m_candidate, m_candidateRevision, Entry::AetherPDOwnerCandidate)) {
        end(Outcome::Unconfirmed, Reason::ChangedCredential); return;
    }
    const bool sent = m_client.replaceOwnerKey(m_target.status.installationId, m_candidate, confirming ? m_candidate : m_active);
    m_remoteRequestSent |= sent;
    if (!sent) { end(Outcome::Unconfirmed, Reason::Remote); }
}
void TailnetShimOwnerKeyTransaction::promote()
{
    if (!transition(State::Promoting) || !current()) { return; }
    if (m_active == m_candidate && m_credentials.durable(m_target.serial, m_active, m_activeRevision, Entry::TailnetShim)) {
        verifyActive(); return;
    }
    const QPointer<TailnetShimOwnerKeyTransaction> guard(this);
    Done done = [guard, lease = m_lease](bool persisted, const QString&) {
        if (!guard || !guard->current()) { return; }
        if (!persisted) { guard->end(Outcome::Unconfirmed, Reason::Storage); return; }
        guard->m_activeRevision = guard->m_credentials.revision(guard->m_target.serial, Entry::TailnetShim);
        guard->verifyActive();
    };
    const bool admitted = m_active.isEmpty()
        ? m_credentials.create(m_target.serial, m_activeRevision, m_candidate, this, std::move(done), Entry::TailnetShim)
        : m_credentials.replace(m_target.serial, m_active, m_activeRevision, m_candidate, this, std::move(done), Entry::TailnetShim);
    if (!admitted && guard && !m_finished) { end(Outcome::Unconfirmed, Reason::ChangedCredential); }
}
void TailnetShimOwnerKeyTransaction::verifyActive()
{
    m_credentials.load(m_target.serial, this, [guard = QPointer(this), lease = m_lease](const QString& token) {
        if (!guard || !guard->current()) { return; }
        if (token != guard->m_candidate || !guard->sameCredentials()
            || !guard->m_credentials.durable(guard->m_target.serial, token, guard->m_activeRevision, Entry::TailnetShim)) {
            guard->end(Outcome::Unconfirmed, Reason::ChangedCredential); return;
        }
        guard->clean();
    }, Entry::TailnetShim);
}
void TailnetShimOwnerKeyTransaction::clean()
{
    if (!transition(State::Cleaning) || !current()) { return; }
    if (!sameCredentials() || !m_credentials.durable(m_target.serial, m_candidate, m_activeRevision, Entry::TailnetShim)) {
        end(Outcome::Unconfirmed, Reason::ChangedCredential); return;
    }
    const QPointer<TailnetShimOwnerKeyTransaction> guard(this);
    const bool admitted = m_credentials.retire(m_candidateId, m_candidate, m_candidateRevision, Entry::AetherPDOwnerCandidate, this,
        [guard, lease = m_lease](bool removed) {
            if (guard && guard->current()) { guard->end(removed ? Outcome::Completed : Outcome::CleanupUnconfirmed, removed ? Reason::None : Reason::Storage); }
        });
    if (!admitted && guard && !m_finished) { end(Outcome::CleanupUnconfirmed, Reason::ChangedCredential); }
}
void TailnetShimOwnerKeyTransaction::cancel() { if (m_state != State::Idle) { end(Outcome::Unconfirmed, Reason::Canceled); } }
void TailnetShimOwnerKeyTransaction::end(Outcome outcome, Reason reason)
{
    if (m_finished) { return; }
    m_finished = true;
    m_reason = reason;
    m_state = State::Finished;
    m_deadline.stop();
    m_client.retirePendingReplies();
    m_lease.reset();
    m_active.clear();m_candidate.clear();
    const QPointer<TailnetShimOwnerKeyTransaction> guard(this);
    emit stateChanged(m_state);
    if (guard) { emit finished(outcome, reason); }
}
} // namespace AetherSDR
