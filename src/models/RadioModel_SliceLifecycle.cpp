#include "RadioModel.h"
#include "SliceModel.h"
#include "PanadapterModel.h"

#include <QPointer>
#include <QThread>
#include <QTimer>
#include <initializer_list>
#include <memory>
#include <utility>

namespace AetherSDR {
namespace {
constexpr int kSliceCreateDeadlineMs = 10000;
constexpr int kSliceCreateRetainMs = 60000;
constexpr int kPendingCreateLimit = 64;
constexpr int kRetirementHistoryLimit = 64;
}

struct RadioModel::PendingSliceCreate {
    quint64 ticket{0};
    RadioControlSession session{};
    QPointer<QObject> owner;
    QPointer<PanadapterModel> pan;
    QSet<int> existing;
    QHash<int, quint64> retirements;
    SliceCreationCompletion completion;
    QMetaObject::Connection ownerConnection;
    QPointer<QTimer> deadlineTimer;
    QPointer<QTimer> retainTimer;
    std::optional<SliceCreateReply> reply;
    bool transient{false};
    bool cancelled{false};
    bool notified{false};
    bool identityUncertain{false};
};

std::optional<quint64> RadioModel::retainSliceRetirement(int id)
{
    if (id < 0) { return {}; }
    const auto existing = m_sliceRetirements.constFind(id);
    if (existing != m_sliceRetirements.cend()) { return *existing; }
    compactSliceRetirements();
    if (m_sliceRetirements.size() >= kRetirementHistoryLimit) { return {}; }
    m_sliceRetirements.insert(id, 0);
    return 0;
}

void RadioModel::compactSliceRetirements()
{
    if (m_sliceRetirements.size() >= kRetirementHistoryLimit) {
        // Discarded tombstones make pending create identities unprovable. Keep
        // live and quarantined identities so issued receipts stay precise.
        for (const auto& pending : std::as_const(m_pendingSliceCreates)) { pending->identityUncertain = true; }
        for (auto it = m_sliceRetirements.begin(); it != m_sliceRetirements.end();) {
            if (!slice(it.key()) && !m_staleSlices.contains(it.key())) { it = m_sliceRetirements.erase(it); }
            else { ++it; }
        }
        QTimer::singleShot(0, this, &RadioModel::reconcileSliceCreates);
    }
}

void RadioModel::noteSliceRetirement(int id)
{
    if (const auto revision = retainSliceRetirement(id)) { m_sliceRetirements[id] = *revision + 1; }
}

void RadioModel::wireSliceLifecycle()
{
    connect(this, &RadioModel::sliceRemoved, this, [this](int id) {
        noteSliceRetirement(id);
        m_pendingSliceRemoves.remove(id);
        QTimer::singleShot(0, this, &RadioModel::reconcileSliceCreates);
    });
    connect(this, &RadioModel::sliceAdded, this, [this](SliceModel*) {
        // Finish after all ordinary sliceAdded consumers have wired the model.
        QTimer::singleShot(0, this, &RadioModel::reconcileSliceCreates);
    });
    connect(this, &RadioModel::slotOccupancyChanged, this, [this](int) {
        QTimer::singleShot(0, this, &RadioModel::reconcileSliceCreates);
    });
    connect(this, &RadioModel::connectionStateChanged, this, [this](bool connected) {
        if (!connected) {
            m_pendingSliceRemoves.clear();
            reconcileSliceCreates();
        }
    });
}

quint64 RadioModel::createSlice(const SliceCreateRequest& request, QObject* replyContext,
                               const SliceCreationCompletion& completion, bool transient)
{
    const auto refuse = [&] {
        if (completion) {
            completion({{RadioControlReply::Kind::Refused, -1,
                         QStringLiteral("Slice creation is unavailable in the original session")}, {}});
        }
        return quint64{0};
    };
    if (QThread::currentThread() != thread() || !request.valid() || !m_backend
        || !controlSessionCurrent(controlSession()) || !panadapter(request.panId)
        || !backendCapabilities().canCreateSlices || m_pendingSliceCreates.size() >= kPendingCreateLimit
        || (transient && !backendCapabilities().canTransmit)) {
        return refuse();
    }
    int reserved = 0;
    for (const auto& pending : std::as_const(m_pendingSliceCreates)) {
        if (pending->session == controlSession()
            && (!pending->reply || !pending->reply->sliceId || !slice(*pending->reply->sliceId))) {
            ++reserved;
        }
    }
    if (slices().size() + reserved >= maxSlices()) { return refuse(); }

    compactSliceRetirements();
    const auto pending = std::make_shared<PendingSliceCreate>();
    pending->ticket = ++m_nextSliceCreateTicket;
    pending->session = controlSession();
    pending->owner = replyContext ? replyContext : this;
    pending->pan = panadapter(request.panId);
    pending->completion = completion;
    pending->transient = transient;
    pending->retirements = m_sliceRetirements;
    for (SliceModel* source : m_slices) { pending->existing.insert(source->sliceId()); }
    m_pendingSliceCreates.insert(pending->ticket, pending);
    const QPointer<RadioModel> alive(this);
    pending->ownerConnection = connect(pending->owner, &QObject::destroyed, this, [alive, pending] {
        if (alive && pending->transient) { alive->cancelSliceCreation(pending->ticket); }
    });
    pending->deadlineTimer = new QTimer(this);
    pending->deadlineTimer->setSingleShot(true);
    connect(pending->deadlineTimer, &QTimer::timeout, this, [alive, pending] {
        if (!alive || !alive->m_pendingSliceCreates.contains(pending->ticket)) { return; }
        alive->expireSliceCreation(pending->ticket);
    });
    pending->deadlineTimer->start(kSliceCreateDeadlineMs);
    pending->retainTimer = new QTimer(this);
    pending->retainTimer->setSingleShot(true);
    connect(pending->retainTimer, &QTimer::timeout, this, [alive, pending] {
        if (alive) { alive->forgetSliceCreation(pending->ticket); }
    });
    pending->retainTimer->start(kSliceCreateRetainMs);
    SliceCreateRequest backendRequest = request;
    backendRequest.panId = backendPanIdFor(request.panId);
    backendRequest.requiresReceipt = transient || request.requiresReceipt;
    const RadioControlFence current = [alive, pending] {
        return alive && alive->controlSessionCurrent(pending->session)
            && pending->pan && alive->panadapter(pending->pan->panId()) == pending->pan;
    };
    const ReceiveDispatch dispatched = m_backend->requestSliceCreate(backendRequest,
        [alive, pending](const SliceCreateReply& reply) {
            if (!alive || pending->reply || !alive->m_pendingSliceCreates.contains(pending->ticket)) { return; }
            pending->reply = reply;
            alive->reconcileSliceCreates();
        }, current);
    if (!alive) { return 0; }
    if (dispatched == ReceiveDispatch::Unsupported && !pending->reply) {
        pending->reply = SliceCreateReply{{RadioControlReply::Kind::Refused, -1,
            QStringLiteral("Backend refused slice creation")}, {}};
        reconcileSliceCreates();
        return 0;
    }
    return pending->ticket;
}

void RadioModel::notifySliceCreate(const std::shared_ptr<PendingSliceCreate>& pending,
                                   const SliceCreationResult& result)
{
    if (pending->notified) { return; }
    pending->notified = true;
    if (pending->deadlineTimer) {
        pending->deadlineTimer->stop();
        pending->deadlineTimer->deleteLater();
        pending->deadlineTimer.clear();
    }
    QObject::disconnect(pending->ownerConnection);
    const SliceCreationCompletion completion = std::exchange(pending->completion, {});
    if (pending->owner && completion) { completion(result); }
}

void RadioModel::forgetSliceCreation(quint64 ticket)
{
    const auto pending = m_pendingSliceCreates.take(ticket);
    if (!pending) { return; }
    QObject::disconnect(pending->ownerConnection);
    for (QTimer* timer : {pending->deadlineTimer.data(), pending->retainTimer.data()}) {
        if (timer) {
            timer->stop();
            timer->deleteLater();
        }
    }
    pending->deadlineTimer.clear();
    pending->retainTimer.clear();
}

void RadioModel::expireSliceCreation(quint64 ticket)
{
    const auto pending = m_pendingSliceCreates.value(ticket);
    if (!pending) { return; }
    pending->cancelled = pending->transient;
    notifySliceCreate(pending, {{RadioControlReply::Kind::Failed, -1,
        QStringLiteral("Slice creation timed out; the radio outcome is unconfirmed")}, {}});
}

void RadioModel::reconcileSliceCreates()
{
    const QPointer<RadioModel> alive(this);
    const auto requests = m_pendingSliceCreates.values();
    for (const auto& pending : requests) {
        if (!m_pendingSliceCreates.contains(pending->ticket)) { continue; }
        if (pending->identityUncertain) {
            forgetSliceCreation(pending->ticket);
            notifySliceCreate(pending, {{RadioControlReply::Kind::Failed, -1,
                QStringLiteral("Slice-creation identity history is no longer available")}, {}});
            if (!alive) { return; }
            continue;
        }
        if (!controlSessionCurrent(pending->session) || !pending->pan
            || panadapter(pending->pan->panId()) != pending->pan) {
            forgetSliceCreation(pending->ticket);
            notifySliceCreate(pending, {{RadioControlReply::Kind::Cancelled, -1,
                QStringLiteral("Original slice-creation session ended")}, {}});
        } else if (pending->reply) {
            const SliceCreateReply reply = *pending->reply;
            if (reply.result.kind != RadioControlReply::Kind::Accepted || !reply.sliceId) {
                forgetSliceCreation(pending->ticket);
                notifySliceCreate(pending, {reply.result, {}});
            } else {
                const int id = *reply.sliceId;
                if (pending->existing.contains(id)
                    || m_sliceRetirements.value(id) != pending->retirements.value(id)
                    || isSlotForeign(id)) {
                    forgetSliceCreation(pending->ticket);
                    notifySliceCreate(pending, {{RadioControlReply::Kind::Failed, -1,
                        QStringLiteral("Created slice identity was retired or belongs to another owner")}, {}});
                } else if (SliceModel* source = slice(id);
                           source && source->panId() == pending->pan->panId() && sliceOwnedByUs(id)) {
                    const std::optional<quint64> retirement = retainSliceRetirement(id);
                    if (!retirement || pending->identityUncertain) {
                        forgetSliceCreation(pending->ticket);
                        notifySliceCreate(pending, {{RadioControlReply::Kind::Failed, -1,
                            QStringLiteral("Slice-creation identity capacity is unavailable")}, {}});
                        if (!alive) { return; }
                        continue;
                    }
                    CreatedSliceReceipt receipt;
                    receipt.m_sliceId = id;
                    receipt.m_identity = source;
                    receipt.m_session = pending->session;
                    receipt.m_retirement = *retirement;
                    forgetSliceCreation(pending->ticket);
                    if (pending->transient && (pending->cancelled || !pending->owner)) {
                        removeCreatedSlice(receipt);
                        if (!alive) { return; }
                        notifySliceCreate(pending, {{RadioControlReply::Kind::Cancelled, -1,
                            QStringLiteral("Transient slice creation was cancelled")}, {}});
                    } else {
                        notifySliceCreate(pending, {reply.result, receipt});
                    }
                }
            }
        }
        if (!alive) { return; }
    }
}

void RadioModel::cancelSliceCreation(quint64 ticket)
{
    const auto pending = m_pendingSliceCreates.value(ticket);
    if (!pending || !pending->transient) { return; }
    pending->cancelled = true;
    const QPointer<RadioModel> alive(this);
    notifySliceCreate(pending, {{RadioControlReply::Kind::Cancelled, -1,
        QStringLiteral("Transient slice creation was cancelled")}, {}});
    if (alive) { reconcileSliceCreates(); }
}

bool RadioModel::removeCreatedSlice(const CreatedSliceReceipt& receipt)
{
    SliceModel* source = resolveCreatedSlice(receipt);
    if (!source) { return false; }
    return dispatchSliceRemoval(source, receipt.m_session, false);
}

SliceModel* RadioModel::resolveCreatedSlice(const CreatedSliceReceipt& receipt) const
{
    if (QThread::currentThread() != thread()) { return nullptr; }
    SliceModel* source = slice(receipt.m_sliceId);
    if (!m_backend
        || !controlSessionCurrent(receipt.m_session) || !receipt.valid() || source != receipt.m_identity
        || m_sliceRetirements.value(receipt.m_sliceId) != receipt.m_retirement
        || !sliceOwnedByUs(receipt.m_sliceId)) { return nullptr; }
    return source;
}

bool RadioModel::dispatchSliceRemoval(SliceModel* source, const RadioControlSession& session,
                                     bool preserveLastReceiver)
{
    if (QThread::currentThread() != thread() || !m_backend || !source
        || !controlSessionCurrent(session) || slice(source->sliceId()) != source
        || !sliceOwnedByUs(source->sliceId())) { return false; }
    const int id = source->sliceId();
    if (m_pendingSliceRemoves.value(id) == source) { return true; }
    if (preserveLastReceiver && m_slices.size() - m_pendingSliceRemoves.size() <= 1) { return false; }
    const QPointer<RadioModel> alive(this);
    const QPointer<SliceModel> identity(source);
    m_pendingSliceRemoves.insert(id, source);
    const RadioControlFence current = [alive, identity, session, id] {
        return alive && identity && alive->controlSessionCurrent(session)
            && alive->slice(id) == identity && alive->sliceOwnedByUs(id);
    };
    const ReceiveDispatch result = m_backend->requestSliceRemove(id,
        [alive, identity, session, id](const RadioControlReply& reply) {
            if (alive && alive->controlSession() == session
                && alive->m_pendingSliceRemoves.value(id) == identity
                && reply.kind != RadioControlReply::Kind::Accepted) {
                alive->m_pendingSliceRemoves.remove(id);
                emit alive->sliceLifecycleFailed(QStringLiteral("remove"), id, reply.message);
            }
        }, current);
    if (!alive) { return result != ReceiveDispatch::Unsupported; }
    if (result == ReceiveDispatch::Unsupported) {
        m_pendingSliceRemoves.remove(id);
        emit sliceLifecycleFailed(QStringLiteral("remove"), id, tr("This radio cannot remove this slice here"));
        return false;
    }
    return true;
}

ReceiveDispatch RadioModel::requestTxSlice(SliceModel* source, bool enabled,
                                          QObject* replyContext,
                                          const RadioControlCompletion& completion,
                                          std::optional<RadioControlSession> expectedSession)
{
    if (QThread::currentThread() != thread()) { return ReceiveDispatch::Unsupported; }
    const QPointer<RadioModel> alive(this);
    const QPointer<SliceModel> target(source);
    const QPointer<QObject> owner(replyContext ? replyContext : this);
    const RadioControlSession session = expectedSession.value_or(controlSession());
    if (!m_backend || !target || !controlSessionCurrent(session)
        || slice(target->sliceId()) != target || !sliceOwnedByUs(target->sliceId())
        || !backendCapabilities().canTransmit
        || (enabled && (modeIsReceiveOnly(backendCapabilities(), target->mode())
            || !panTransmitInhibitReason(target->panId()).isEmpty()))) {
        if (owner && completion) {
            completion({RadioControlReply::Kind::Refused, -1, QStringLiteral("TX-slice selection refused")});
        }
        return ReceiveDispatch::Unsupported;
    }
    const std::optional<quint64> retirement = retainSliceRetirement(target->sliceId());
    if (!retirement) {
        if (owner && completion) {
            completion({RadioControlReply::Kind::Refused, -1,
                        QStringLiteral("TX-selection identity capacity is unavailable")});
        }
        return ReceiveDispatch::Unsupported;
    }
    const quint64 revision = ++m_txSliceSelectionRevision;
    const RadioControlFence current = [alive, target, session, enabled, revision, retirement] {
        return alive && target && alive->controlSessionCurrent(session)
            && revision == alive->m_txSliceSelectionRevision
            && alive->m_sliceRetirements.value(target->sliceId()) == *retirement
            && alive->slice(target->sliceId()) == target && alive->sliceOwnedByUs(target->sliceId())
            && alive->backendCapabilities().canTransmit
            && (!enabled || (!modeIsReceiveOnly(alive->backendCapabilities(), target->mode())
                && alive->panTransmitInhibitReason(target->panId()).isEmpty()));
    };
    const auto changed = std::make_shared<QMetaObject::Connection>();
    const auto finished = std::make_shared<bool>(false);
    const RadioControlCompletion reply = fencedSetupCompletion(this,
        [owner, current, completion, changed, finished](const RadioControlReply& result) {
            *finished = true;
            QObject::disconnect(*changed);
            if (!owner || !completion) { return; }
            if (result.kind == RadioControlReply::Kind::Accepted && !current()) {
                completion({RadioControlReply::Kind::Cancelled, -1,
                    QStringLiteral("Original TX-selection target is no longer available")});
            } else { completion(result); }
        }, true);
    if (enabled) { noteLocalTxSliceEnableIntent(source->sliceId()); }
    *changed = connect(source, &SliceModel::txSliceChanged, this, [reply, enabled](bool actual) {
        if (actual == enabled) { reply({RadioControlReply::Kind::Accepted, 0, {}}); }
    });
    const ReceiveDispatch result = m_backend->requestTxSlice(source->sliceId(), enabled, reply, current);
    if (!alive) { return result; }
    if (result == ReceiveDispatch::Unsupported) {
        reply({RadioControlReply::Kind::Refused, -1, QStringLiteral("Backend refused TX-slice selection")});
    } else if (!*finished && target && target->isTxSlice() == enabled) {
        reply({RadioControlReply::Kind::Accepted, 0, {}});
    }
    return result;
}

ReceiveDispatch RadioModel::requestFullDuplex(bool enabled, QObject* replyContext,
                                             const RadioControlCompletion& completion)
{
    if (QThread::currentThread() != thread()) { return ReceiveDispatch::Unsupported; }
    const QPointer<RadioModel> alive(this);
    const QPointer<QObject> owner(replyContext ? replyContext : this);
    const RadioControlSession session = controlSession();
    if (!m_backend || !controlSessionCurrent(session) || !backendCapabilities().hasFullDuplex) {
        if (owner && completion) {
            completion({RadioControlReply::Kind::Refused, -1, QStringLiteral("Full duplex is unavailable")});
        }
        return ReceiveDispatch::Unsupported;
    }
    const quint64 revision = ++m_fullDuplexRequestRevision;
    const quint64 observation = m_fullDuplexObservationRevision;
    const RadioControlCompletion reply = fencedSetupCompletion(this,
        [alive, owner, session, revision, observation, enabled, completion](const RadioControlReply& result) {
            if (!alive) { return; }
            if (revision != alive->m_fullDuplexRequestRevision) {
                if (owner && completion) { completion({RadioControlReply::Kind::Cancelled, -1,
                    QStringLiteral("A newer full-duplex request superseded this one")}); }
                return;
            }
            if (result.kind == RadioControlReply::Kind::Accepted
                && alive->controlSessionCurrent(session)
                && observation == alive->m_fullDuplexObservationRevision) {
                alive->setFullDuplex(enabled);
            }
            if (alive && owner && completion) { completion(result); }
        }, true);
    const RadioControlFence current = [alive, session, revision] {
        return alive && alive->controlSessionCurrent(session)
            && revision == alive->m_fullDuplexRequestRevision && alive->backendCapabilities().hasFullDuplex;
    };
    const ReceiveDispatch result = m_backend->requestFullDuplex(enabled, reply, current);
    if (alive && result == ReceiveDispatch::Unsupported) {
        reply({RadioControlReply::Kind::Refused, -1, QStringLiteral("Backend refused full duplex")});
    }
    return result;
}

ReceiveDispatch RadioModel::requestSliceInventoryRefresh(std::optional<RadioControlSession> expectedSession)
{
    const QPointer<RadioModel> alive(this);
    const RadioControlSession session = expectedSession.value_or(controlSession());
    const RadioControlFence current = [alive, session] {
        return alive && alive->controlSessionCurrent(session);
    };
    if (QThread::currentThread() != thread() || !m_backend || !current()) {
        return ReceiveDispatch::Unsupported;
    }
    // A refresh requests observations; it never clears or fabricates inventory.
    return m_backend->requestSliceInventoryRefresh({}, current);
}

} // namespace AetherSDR
