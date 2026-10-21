#include "RadioModel.h"

#include "core/backends/IRadioBackend.h"
#include "models/SliceModel.h"

#include <QPointer>
#include <QTimer>
#include <QDateTime>
#include <algorithm>

namespace AetherSDR {
namespace {
constexpr int kOwnershipTimeoutMs = 3000;
constexpr qsizetype kMaxRetiredStreams = 256;

QString streamToken(quint32 id)
{
    return QStringLiteral("0x%1").arg(id, 8, 16, QLatin1Char('0'));
}
}

TxCoordinator::Context RadioModel::captureDaxTxMedia(const TxCoordinator::Request& request) const
{
    const TxCoordinator::Context context = captureTxMedia(request);
    if (!backendCapabilities().hasDaxStreams) { return context; }
    const DaxTxAudioPermit permit = m_daxTxAudioPermit;
    return context.withMediaPermit([permit] { return permit.permits(); });
}

TxCoordinator::Context RadioModel::captureDaxTxMedia(const TxCoordinator::Producer& producer) const
{
    const TxCoordinator::Context context = captureTxMedia(producer);
    if (!backendCapabilities().hasDaxStreams) { return context; }
    const DaxTxAudioPermit permit = m_daxTxAudioPermit;
    return context.withMediaPermit([permit] { return permit.permits(); });
}

bool RadioModel::daxTxStreamReady() const
{
    return isConnected() && m_daxTxStreamId != 0 && m_daxTxClientHandle != 0
        && m_daxTxClientHandle == ourClientHandle() && m_daxTxKnown && m_daxTxActive
        && !m_daxTxUncertain && m_daxTxAudioPermit.permitsStream(m_daxTxStreamId);
}

QString RadioModel::daxTxOwnershipUnavailableReason(bool take) const
{
    if (!isConnected() || m_txSessionClosing) {
        return tr("Connect to a radio before changing DAX TX ownership.");
    }
    if (!m_backend || !backendCapabilities().canTransmit || !backendCapabilities().hasDaxStreams) {
        return tr("This radio does not support DAX TX stream ownership.");
    }
    if (ourClientHandle() == 0) {
        return tr("Waiting for this client's radio identity.");
    }
    if (profileLoadRadioStateWritesHeld()) {
        return tr("Wait for the profile load to finish.");
    }
    const bool preparingDax = std::any_of(m_daxTxInputs.cbegin(), m_daxTxInputs.cend(),
        [](const TxCoordinator::Request::Weak& input) { return input.lock().valid(); });
    if (isRadioTransmitting() || transmitOwnershipPending() || preparingDax
        || m_wsprTxInput.originalSessionCurrent()) {
        return tr("Finish the current transmission or transmit preparation before changing DAX TX ownership.");
    }
    if (m_daxTxOwnershipIntent) {
        return tr("Waiting for the current DAX TX ownership request.");
    }
    if (m_daxTxUncertain) {
        return tr("Waiting for fresh radio status after an unconfirmed request; no retry was sent.");
    }
    if (m_daxTxStreamId != 0
        && (m_daxTxClientHandle == 0 || m_daxTxClientHandle != ourClientHandle() || !m_daxTxKnown)) {
        return tr("Waiting for confirmed ownership and TX status of this client's stream.");
    }
    if (take) {
        for (const SliceModel* slice : slices()) {
            if (slice && slice->isTxSlice() && sliceOwnedByUs(slice->sliceId())) {
                return {};
            }
        }
        return tr("Select a TX slice owned by this client before taking DAX TX.");
    }
    if (m_daxTxStreamId == 0) {
        return tr("This client has no DAX TX stream to release.");
    }
    return {};
}

QJsonObject RadioModel::daxTxOwnershipSnapshot() const
{
    const bool owned = m_daxTxStreamId != 0 && m_daxTxClientHandle != 0
        && m_daxTxClientHandle == ourClientHandle() && m_daxTxKnown && m_daxTxActive;
    const bool available = isConnected() && m_backend && backendCapabilities().canTransmit
        && backendCapabilities().hasDaxStreams;
    QString state;
    if (!available) {
        state = QStringLiteral("unavailable");
    } else if (m_daxTxOwnershipIntent) {
        state = m_daxTxOwnershipIntent->streamId == 0
            ? QStringLiteral("creating")
            : (m_daxTxOwnershipIntent->take ? QStringLiteral("taking") : QStringLiteral("releasing"));
    } else if (m_daxTxUncertain) {
        state = m_daxTxOutcome.isEmpty() ? QStringLiteral("unknown") : m_daxTxOutcome;
    } else if (!m_daxTxOutcome.isEmpty()) {
        state = m_daxTxOutcome;
    } else if (m_daxTxStreamId == 0) {
        state = QStringLiteral("no-stream");
    } else if (!m_daxTxKnown || m_daxTxClientHandle == 0) {
        state = QStringLiteral("unknown");
    } else {
        state = owned ? QStringLiteral("owned") : QStringLiteral("released");
    }
    const QString takeReason = daxTxOwnershipUnavailableReason(true);
    const QString releaseReason = daxTxOwnershipUnavailableReason(false);
    return {
        {QStringLiteral("available"), available},
        {QStringLiteral("streamId"), streamToken(m_daxTxStreamId)},
        {QStringLiteral("clientHandle"), streamToken(m_daxTxClientHandle)},
        {QStringLiteral("txKnown"), m_daxTxKnown},
        {QStringLiteral("owned"), owned},
        {QStringLiteral("ready"), daxTxStreamReady()},
        {QStringLiteral("pending"), m_daxTxOwnershipIntent.has_value()},
        {QStringLiteral("state"), state},
        {QStringLiteral("error"), m_daxTxError},
        {QStringLiteral("canTake"), takeReason.isEmpty()},
        {QStringLiteral("canRelease"), releaseReason.isEmpty()},
        {QStringLiteral("takeReason"), takeReason},
        {QStringLiteral("releaseReason"), releaseReason}
    };
}

void RadioModel::publishDaxTxOwnership()
{
    const QPointer<RadioModel> self(this);
    const quint64 session = m_daxTxSession;
    const quint64 incarnation = m_daxTxIncarnation;
    const quint64 revision = m_daxTxObservationRevision;
    const bool releasing = m_wsprTxReleaseWhenReady
        || (m_daxTxOwnershipIntent && !m_daxTxOwnershipIntent->take);
    const bool ready = isConnected() && m_daxTxStreamId != 0 && m_daxTxClientHandle != 0
        && m_daxTxClientHandle == ourClientHandle() && m_daxTxKnown && m_daxTxActive
        && !m_daxTxUncertain && !releasing;
    if (!ready || !m_daxTxAudioPermit.permitsStream(m_daxTxStreamId)) {
        const bool hadGrant = !m_daxTxAudioPermit.sameGrant(DaxTxAudioPermit::blocked());
        m_daxTxAudioPermit.revoke();
        m_daxTxAudioPermit = DaxTxAudioPermit::blocked();
        if (hadGrant) { retireDaxTxInputs(); }
        if (!self || session != m_daxTxSession || incarnation != m_daxTxIncarnation
            || revision != m_daxTxObservationRevision) { return; }
        if (ready) { m_daxTxAudioPermit = DaxTxAudioPermit::confirmed(m_daxTxStreamId); }
    }
    emit daxTxAudioRouteChanged(ready ? m_daxTxStreamId : 0, m_daxTxAudioPermit);
    if (self) { emit daxTxOwnershipChanged(); }
}

void RadioModel::noteDaxTxInput(const TxCoordinator::Request& request)
{
    if (!backendCapabilities().hasDaxStreams || !m_txCoordinator.ownsRequest(request) || !request.valid()) {
        return;
    }
    std::erase_if(m_daxTxInputs, [](const TxCoordinator::Request::Weak& input) {
        return !input.lock().valid();
    });
    for (const TxCoordinator::Request::Weak& input : m_daxTxInputs) {
        if (input.lock().sameRequest(request)) { return; }
    }
    if (m_daxTxInputs.size() < static_cast<size_t>(TxCoordinator::kMaximumRequests)) {
        m_daxTxInputs.push_back(request.weak());
    }
    emit daxTxOwnershipChanged();
}

void RadioModel::retireDaxTxInputs()
{
    const std::vector<TxCoordinator::Request::Weak> inputs = std::move(m_daxTxInputs);
    m_daxTxInputs.clear();
    const QPointer<RadioModel> self(this);
    for (const TxCoordinator::Request::Weak& reference : inputs) {
        const TxCoordinator::Request input = reference.lock();
        const auto bound = m_txCoordinator.requestsFrom(input);
        (void)m_txCoordinator.closeRequest(input);
        for (const TxCoordinator::Request& request : bound) {
            if (!self) { return; }
            abortProducerPtt(request, input.sameRequest(m_wsprTxInput)
                ? TransmitModel::PttSource::Wspr : TransmitModel::PttSource::Dax);
        }
        if (!self) { return; }
    }
}

void RadioModel::refreshDaxTxOwnershipGuards()
{
    const QPointer<RadioModel> self(this);
    const quint64 session = m_daxTxSession;
    m_daxTxGuardTimer.stop();
    if (profileLoadRadioStateWritesHeld()) {
        const qint64 remaining = m_profileLoadRadioStateWriteHoldUntilMs
            - QDateTime::currentMSecsSinceEpoch();
        m_daxTxGuardTimer.start(static_cast<int>(std::clamp<qint64>(remaining + 1, 1, 60000)));
        interruptDaxTxOwnership(tr("The profile load interrupted DAX TX ownership. Waiting for fresh radio status."));
    }
    if (self && session == m_daxTxSession) { emit daxTxOwnershipChanged(); }
}

void RadioModel::interruptDaxTxOwnership(const QString& reason)
{
    if (!m_daxTxOwnershipIntent) { return; }
    m_daxTxOwnershipIntent->guard.revoke();
    m_daxTxCreateGuard.revoke();
    ++m_daxTxCreateTicket;
    m_daxTxCreatePending = false;
    m_daxTxUncertain = true;
    finishDaxTxOwnership(false, reason);
}

void RadioModel::resetDaxTxOwnership()
{
    m_daxTxGuardTimer.stop();
    retireDaxTxInputs();
    m_daxTxAudioPermit.revoke();
    ++m_daxTxSession;
    ++m_daxTxIncarnation;
    ++m_daxTxCreateTicket;
    if (m_daxTxOwnershipIntent) { m_daxTxOwnershipIntent->guard.revoke(); }
    m_daxTxCreateGuard.revoke();
    m_daxTxOwnershipIntent.reset();
    m_daxTxRetiredAt.clear();
    m_daxTxStreamId = 0;
    m_daxTxClientHandle = 0;
    m_daxTxKnown = false;
    m_daxTxActive = false;
    m_daxTxCreatePending = false;
    m_daxTxUncertain = false;
    m_daxTxOutcome.clear();
    m_daxTxError.clear();
    m_wsprTxOwnershipRequested = false;
    m_wsprTxReleaseWhenReady = false;
    publishDaxTxOwnership();
}

void RadioModel::observeDaxTxStream(const DaxTxStreamDelta& delta)
{
    if (!isConnected() || m_txSessionClosing || delta.streamId == 0 || ourClientHandle() == 0) {
        return;
    }
    const QPointer<RadioModel> self(this);
    const quint64 session = m_daxTxSession;
    ++m_daxTxObservationRevision;
    if (delta.removed) {
        m_daxTxRetiredAt.insert(delta.streamId, m_daxTxObservationRevision);
        if (m_daxTxRetiredAt.size() > kMaxRetiredStreams) {
            // A create whose retirement evidence overflowed is ambiguous.
            if (m_daxTxCreatePending) {
                ++m_daxTxCreateTicket;
                m_daxTxCreatePending = false;
                finishDaxTxOwnership(false, tr("Stream history changed while creation was pending."));
                if (!self || session != m_daxTxSession) { return; }
            }
            m_daxTxRetiredAt.clear();
            m_daxTxRetiredAt.insert(delta.streamId, m_daxTxObservationRevision);
        }
        if (delta.streamId != m_daxTxStreamId) {
            return;
        }
        m_daxTxAudioPermit.revoke();
        ++m_daxTxIncarnation;
        ++m_daxTxCreateTicket;
        m_daxTxStreamId = 0;
        m_daxTxClientHandle = 0;
        m_daxTxKnown = false;
        m_daxTxActive = false;
        m_daxTxCreatePending = false;
        m_wsprTxReleaseWhenReady = false;
        finishDaxTxOwnership(false, tr("The radio removed this client's DAX TX stream."));
        return;
    }
    if (m_daxTxStreamId == 0) {
        if (delta.invalidOwner || !delta.clientHandle || *delta.clientHandle != ourClientHandle()) {
            return;
        }
        // A retired numeric ID needs a fresh complete identity, not an old
        // owner-only/tx-only delta, before it can represent another stream.
        if (m_daxTxRetiredAt.contains(delta.streamId) && (!delta.tx || delta.invalidTx)) {
            return;
        }
        m_daxTxStreamId = delta.streamId;
        ++m_daxTxIncarnation;
        m_daxTxClientHandle = 0;
        m_daxTxKnown = false;
        m_daxTxActive = false;
    }
    if (delta.streamId != m_daxTxStreamId) {
        return;
    }
    if (delta.invalidOwner || (delta.clientHandle && *delta.clientHandle != ourClientHandle())) {
        m_daxTxAudioPermit.revoke();
        ++m_daxTxIncarnation;
        m_daxTxRetiredAt.insert(delta.streamId, m_daxTxObservationRevision);
        m_daxTxStreamId = 0;
        m_daxTxClientHandle = 0;
        m_daxTxKnown = false;
        m_daxTxActive = false;
        m_wsprTxReleaseWhenReady = false;
        finishDaxTxOwnership(false, tr("The stream's client ownership changed or became unknown."));
        return;
    }
    if (delta.clientHandle) {
        if (m_daxTxClientHandle != *delta.clientHandle) {
            m_daxTxKnown = false;
            m_daxTxActive = false;
        }
        m_daxTxClientHandle = *delta.clientHandle;
    }
    if (delta.invalidTx) {
        m_daxTxKnown = false;
        m_daxTxActive = false;
        ++m_daxTxIncarnation;
        finishDaxTxOwnership(false, tr("The radio's DAX TX status was malformed."));
        if (!self || session != m_daxTxSession) { return; }
    } else if (delta.tx && m_daxTxClientHandle == ourClientHandle()) {
        m_daxTxKnown = true;
        m_daxTxActive = *delta.tx;
        if (m_daxTxUncertain) {
            m_daxTxUncertain = false;
            m_daxTxOutcome.clear();
            m_daxTxError.clear();
        }
    }
    updateOperatorTransmit();
    if (!self || session != m_daxTxSession) { return; }
    continueDaxTxOwnership();
    if (!self || session != m_daxTxSession) { return; }
    completeDaxTxOwnership();
    if (!self || session != m_daxTxSession) { return; }
    finishWsprDaxTxRelease();
    if (self && session == m_daxTxSession) { publishDaxTxOwnership(); }
}

bool RadioModel::acceptDaxTxCreate(quint32 id, quint64 startRevision)
{
    if (id == 0 || m_daxTxRetiredAt.value(id, 0) > startRevision
        || (m_daxTxStreamId != 0 && m_daxTxStreamId != id)) {
        finishDaxTxOwnership(false, tr("The create reply no longer identifies a current local stream."));
        return false;
    }
    if (m_daxTxStreamId == 0) {
        m_daxTxStreamId = id;
        ++m_daxTxIncarnation;
        m_daxTxClientHandle = 0;
        m_daxTxKnown = false;
        m_daxTxActive = false;
    }
    publishDaxTxOwnership();
    return true;
}

bool RadioModel::requestDaxTxOwnership(bool take)
{
    return beginDaxTxOwnership(take, false);
}

bool RadioModel::beginDaxTxOwnership(bool take, bool wspr)
{
    if (m_daxTxOwnershipIntent && m_daxTxOwnershipIntent->take == take) {
        return true;
    }
    QString reason = daxTxOwnershipUnavailableReason(take);
    if (wspr && isConnected() && m_backend && backendCapabilities().hasDaxStreams
        && backendCapabilities().canTransmit && ourClientHandle() != 0
        && !m_daxTxUncertain && !m_daxTxOwnershipIntent && !profileLoadRadioStateWritesHeld()
        && !isRadioTransmitting()) {
        reason.clear();
    }
    if (!reason.isEmpty()) {
        m_daxTxError = reason;
        publishDaxTxOwnership();
        return false;
    }
    if (m_daxTxStreamId != 0 && m_daxTxClientHandle == ourClientHandle()
        && m_daxTxKnown && m_daxTxActive == take) {
        m_daxTxError.clear();
        m_daxTxOutcome.clear();
        publishDaxTxOwnership();
        return true;
    }
    const quint64 ticket = ++m_daxTxTicket;
    DaxTxOwnershipIntent intent;
    intent.ticket = ticket;
    intent.session = m_daxTxSession;
    intent.take = take;
    intent.guard = DaxTxRequestGuard::create(kOwnershipTimeoutMs);
    m_daxTxOwnershipIntent = intent;
    m_daxTxError.clear();
    m_daxTxOutcome.clear();
    const quint64 session = m_daxTxSession;
    const QPointer<RadioModel> self(this);
    QTimer::singleShot(kOwnershipTimeoutMs, this, [this, ticket] { expireDaxTxOwnership(ticket); });
    publishDaxTxOwnership();
    if (!self || session != m_daxTxSession || !m_daxTxOwnershipIntent
        || ticket != m_daxTxOwnershipIntent->ticket) {
        return false;
    }
    if (m_daxTxStreamId == 0) {
        if (!take || !ensureDaxTxStream(wspr ? DaxTxRequestReason::WsprBeacon
                                           : DaxTxRequestReason::ExplicitOwnership)) {
            finishDaxTxOwnership(false, tr("A local DAX TX stream could not be created."));
            return false;
        }
    }
    continueDaxTxOwnership();
    return true;
}

void RadioModel::continueDaxTxOwnership()
{
    if (!m_daxTxOwnershipIntent || m_daxTxOwnershipIntent->dispatched || m_daxTxCreatePending
        || m_daxTxStreamId == 0 || !m_daxTxKnown || m_daxTxClientHandle != ourClientHandle()) {
        return;
    }
    if (m_daxTxActive == m_daxTxOwnershipIntent->take) {
        finishDaxTxOwnership(true, {});
        return;
    }
    if (!m_daxTxOwnershipIntent->guard.permits()) {
        expireDaxTxOwnership(m_daxTxOwnershipIntent->ticket);
        return;
    }
    DaxTxOwnershipIntent& pending = *m_daxTxOwnershipIntent;
    pending.streamId = m_daxTxStreamId;
    pending.incarnation = m_daxTxIncarnation;
    pending.startRevision = m_daxTxObservationRevision;
    pending.dispatched = true;
    const DaxTxOwnershipIntent request = pending;
    const QPointer<RadioModel> self(this);
    const QPointer<IRadioBackend> origin(m_backend.get());
    publishDaxTxOwnership();
    if (!self || !origin || !m_daxTxOwnershipIntent
        || request.ticket != m_daxTxOwnershipIntent->ticket) {
        return;
    }
    const ReceiveDispatch dispatch = origin->requestDaxTxOwnership(
        {request.streamId, request.take, request.guard},
        [self, origin, request](bool accepted, const QString& reason) {
            if (!self || !origin || self->m_backend.get() != origin
                || request.session != self->m_daxTxSession
                || request.incarnation != self->m_daxTxIncarnation
                || !self->m_daxTxOwnershipIntent
                || request.ticket != self->m_daxTxOwnershipIntent->ticket) {
                return;
            }
            if (!accepted) {
                self->finishDaxTxOwnership(false, reason.isEmpty()
                    ? tr("The radio refused the DAX TX ownership request.") : reason);
                return;
            }
            self->m_daxTxOwnershipIntent->accepted = true;
            self->completeDaxTxOwnership();
        });
    if (self && m_daxTxOwnershipIntent && request.ticket == m_daxTxOwnershipIntent->ticket
        && dispatch != ReceiveDispatch::Dispatched) {
        finishDaxTxOwnership(false, tr("This backend could not dispatch DAX TX ownership."));
    }
}

void RadioModel::completeDaxTxOwnership()
{
    if (!m_daxTxOwnershipIntent || !m_daxTxOwnershipIntent->accepted) {
        return;
    }
    const DaxTxOwnershipIntent& request = *m_daxTxOwnershipIntent;
    if (request.session == m_daxTxSession && request.incarnation == m_daxTxIncarnation
        && request.streamId == m_daxTxStreamId && m_daxTxKnown
        && m_daxTxClientHandle == ourClientHandle()
        && m_daxTxObservationRevision > request.startRevision && m_daxTxActive == request.take) {
        finishDaxTxOwnership(true, {});
    }
}

void RadioModel::finishDaxTxOwnership(bool success, const QString& reason)
{
    const QPointer<RadioModel> self(this);
    const quint64 session = m_daxTxSession;
    if (m_daxTxOwnershipIntent) { m_daxTxOwnershipIntent->guard.revoke(); }
    if (!success) {
        m_daxTxCreateGuard.revoke();
        ++m_daxTxCreateTicket;
        m_daxTxCreatePending = false;
        if (m_wsprTxInput.originalSessionCurrent()) { (void)m_txCoordinator.closeRequest(m_wsprTxInput); }
    }
    m_daxTxOwnershipIntent.reset();
    m_daxTxOutcome = success ? QString() : QStringLiteral("refused");
    m_daxTxError = reason;
    publishDaxTxOwnership();
    if (self && session == m_daxTxSession && success) {
        finishWsprDaxTxRelease();
    }
}

void RadioModel::expireDaxTxOwnership(quint64 ticket)
{
    if (!m_daxTxOwnershipIntent || ticket != m_daxTxOwnershipIntent->ticket) { return; }
    const QPointer<RadioModel> self(this);
    const quint64 session = m_daxTxSession;
    const TxCoordinator::Request wsprInput = m_wsprTxInput;
    m_daxTxOwnershipIntent->guard.revoke();
    m_daxTxCreateGuard.revoke();
    ++m_daxTxCreateTicket;
    m_daxTxCreatePending = false;
    m_daxTxOwnershipIntent.reset();
    m_daxTxUncertain = true;
    m_wsprTxReleaseWhenReady = false;
    m_daxTxOutcome = QStringLiteral("timed-out");
    m_daxTxError = tr("DAX TX ownership was not confirmed. No retry or rollback was sent.");
    m_daxTxAudioPermit.revoke();
    retireDaxTxInputs();
    if (!self || session != m_daxTxSession) { return; }
    if (wsprInput.originalSessionCurrent()) { (void)m_txCoordinator.closeRequest(wsprInput); }
    publishDaxTxOwnership();
}

void RadioModel::releaseWsprDaxTxOwnership()
{
    const QPointer<RadioModel> self(this);
    const quint64 session = m_daxTxSession;
    m_wsprTxReleaseWhenReady = true;
    if (m_daxTxOwnershipIntent) {
        // Sink admission can precede the actual network write. Cancellation
        // revokes that exact queued mutation even when the sink accepted it.
        m_daxTxOwnershipIntent->guard.revoke();
        m_daxTxCreateGuard.revoke();
        ++m_daxTxCreateTicket;
        ++m_daxTxTicket;
        m_daxTxCreatePending = false;
        m_daxTxOwnershipIntent.reset();
        m_daxTxUncertain = true;
        m_daxTxOutcome = QStringLiteral("unknown");
        m_daxTxError = tr("The beacon ended before ownership settled. Waiting for fresh radio status.");
    }
    finishWsprDaxTxRelease();
    if (self && session == m_daxTxSession) { publishDaxTxOwnership(); }
}

void RadioModel::finishWsprDaxTxRelease()
{
    if (!m_wsprTxReleaseWhenReady || isRadioTransmitting() || m_daxTxUncertain) {
        return;
    }
    if (m_daxTxOwnershipIntent) {
        return;
    }
    if (m_daxTxStreamId == 0 || m_daxTxClientHandle != ourClientHandle() || !m_daxTxKnown) {
        return;
    }
    m_wsprTxReleaseWhenReady = false;
    (void)beginDaxTxOwnership(false, true);
}

} // namespace AetherSDR
