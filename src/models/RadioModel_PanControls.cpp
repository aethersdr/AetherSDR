#include "RadioModel.h"

#include "core/LogManager.h"
#include <QPointer>
#include <QTimer>
#include <QThread>
#include <algorithm>
#include <utility>

namespace AetherSDR {

struct RadioModel::PendingPanCreation {
    RadioControlSession session{};
    QPointer<QObject> owner;
    PanCreationCompletion completion;
    QSet<QString> existing;
    QHash<QString, quint64> retirements;
    quint64 historyEpoch{0};
    QString id;
    QPointer<QTimer> deadline;
    bool notified{false};
};

void RadioModel::retireDisplayResource(const QString& id)
{
    if (id.isEmpty()) { return; }
    if (!m_displayRetirements.contains(id) && m_displayRetirements.size() >= 256) {
        ++m_displayHistoryEpoch;
        m_displayRetirements.clear();
    }
    ++m_displayRetirements[id];
}

void RadioModel::settlePanCreations()
{
    const QPointer<RadioModel> alive(this);
    const auto pending = m_pendingPanCreations;
    for (auto it = pending.cbegin(); it != pending.cend(); ++it) {
        const std::shared_ptr<PendingPanCreation> request = it.value();
        PanadapterModel* pan = request->id.isEmpty() ? nullptr : panadapter(request->id);
        const bool ended = !controlSessionCurrent(request->session);
        const bool uncertain = request->historyEpoch != m_displayHistoryEpoch
            || (!request->id.isEmpty() && (request->existing.contains(request->id)
                || request->retirements.value(request->id) != m_displayRetirements.value(request->id)));
        const bool published = pan && pan->ownedByClient(clientHandle());
        if (m_pendingPanCreations.value(it.key()) != request) { continue; }
        if (!ended && (uncertain || (!request->owner && !published))) {
            if (!request->notified) {
                request->notified = true;
                const PanCreationCompletion completion = std::exchange(request->completion, {});
                if (request->deadline) { request->deadline->stop(); request->deadline->deleteLater(); }
                if (request->owner && completion) {
                    completion({RadioControlReply::Kind::Cancelled, -1,
                                QStringLiteral("pan creation identity is uncertain")}, nullptr);
                }
                if (!alive) { return; }
            }
            // Unanswered/uncertain creation still consumes a reservation. Losing
            // a requester or its history does not cancel a firmware allocation.
            continue;
        }
        if (!ended && !published) { continue; }
        m_pendingPanCreations.remove(it.key());
        if (request->deadline) { request->deadline->stop(); request->deadline->deleteLater(); }
        const PanCreationCompletion completion = std::exchange(request->completion, {});
        if (!request->notified && request->owner && completion) {
            request->notified = true;
            completion({ended ? RadioControlReply::Kind::Cancelled : RadioControlReply::Kind::Accepted,
                        ended ? -1 : 0, ended ? QStringLiteral("original pan creation retired") : QString()},
                       ended ? nullptr : pan);
        }
        if (!alive) { return; }
    }
}

ReceiveDispatch RadioModel::requestPanCreation(QObject* context, const PanCreationCompletion& completion,
                                               bool initializeDisplay)
{
    if (QThread::currentThread() != thread()) { return ReceiveDispatch::Unsupported; }
    const QPointer<RadioModel> alive(this);
    settlePanCreations();
    if (!alive) { return ReceiveDispatch::Unsupported; }
    const RadioControlSession session = controlSession();
    const QPointer<QObject> owner(context ? context : this);
    const auto control = backendCapabilities().panLifecycleControl;
    if (!m_backend || !controlSessionCurrent(session) || !control
        || !control->supports(PanLifecycleAction::Create)) { return ReceiveDispatch::Unsupported; }
    if (control->creation == PanLifecycleControl::Creation::Native) {
        QSet<QString> before;
        for (PanadapterModel* pan : panadapters()) { before.insert(pan->panId()); }
        const ReceiveDispatch result = m_backend->requestPanLifecycle({PanLifecycleAction::Create, {}, {}});
        if (!alive || !owner || !controlSessionCurrent(session)) { return result; }
        PanadapterModel* created = nullptr;
        for (PanadapterModel* pan : panadapters()) {
            if (!before.contains(pan->panId())) {
                if (created) { created = nullptr; break; }
                created = pan;
            }
        }
        if (completion) {
            completion({created ? RadioControlReply::Kind::Accepted : RadioControlReply::Kind::Refused,
                        created ? 0 : -1, {}}, created);
        }
        return result;
    }
    const int limit = std::min(maxPanadapters(), 64);
    if (m_panadapters.size() + m_pendingPanCreations.size() >= limit) {
        emit panadapterLimitReached(maxPanadapters(), m_model);
        return ReceiveDispatch::Unsupported;
    }
    const auto request = std::make_shared<PendingPanCreation>();
    request->session = session;
    request->owner = owner;
    request->retirements = m_displayRetirements;
    request->historyEpoch = m_displayHistoryEpoch;
    for (PanadapterModel* pan : m_panadapters) { request->existing.insert(pan->panId()); }
    for (auto it = m_stalePanadapters.cbegin(); it != m_stalePanadapters.cend(); ++it) { request->existing.insert(it.key()); }
    request->completion = [alive, owner, session, initializeDisplay, completion](const RadioControlReply& reply, PanadapterModel* pan) {
        const QPointer<PanadapterModel> original(pan);
        const quint64 revision = original ? original->ownershipRevision() : 0;
        const quint64 rangeRevision = original ? original->displayIntentRevision(PanDisplayField::LevelRange) : 0;
        const quint64 pixelRevision = original ? original->displayIntentRevision(PanDisplayField::Pixels) : 0;
        if (completion) { completion(reply, pan); }
        if (!alive || !owner || !original || !initializeDisplay || reply.kind != RadioControlReply::Kind::Accepted) { return; }
        QTimer::singleShot(200, alive, [alive, owner, original, session, revision, rangeRevision, pixelRevision] {
            if (!alive || !owner || !original || !alive->controlSessionCurrent(session)
                || alive->panadapter(original->panId()) != original || original->ownershipRevision() != revision) { return; }
            const auto control = alive->backendCapabilities().panDisplayControl;
            if (!control) { return; }
            if (rangeRevision == 0 && original->displayIntentRevision(PanDisplayField::LevelRange) == 0
                && control->supports(PanDisplayField::LevelRange)) {
                alive->requestPanDisplay(original, {PanDisplayField::LevelRange, PanLevelRange{-130.0, -40.0}}, {}, false);
            }
            if (!alive || !original || !alive->controlSessionCurrent(session)) { return; }
            if (pixelRevision == 0 && original->displayIntentRevision(PanDisplayField::Pixels) == 0
                && control->supports(PanDisplayField::Pixels)) {
                alive->requestPanDisplay(original, {PanDisplayField::Pixels, PanPixelDimensions{1024, 700}}, {}, false);
            }
        });
    };
    const quint64 ticket = ++m_nextPanCreation;
    m_pendingPanCreations.insert(ticket, request);
    QTimer* deadline = new QTimer(this);
    deadline->setObjectName(QStringLiteral("panCreationDeadline"));
    deadline->setSingleShot(true);
    request->deadline = deadline;
    connect(deadline, &QTimer::timeout, this, [alive, ticket] {
        if (!alive) { return; }
        const auto request = alive->m_pendingPanCreations.value(ticket);
        if (!request || request->notified) { return; }
        request->notified = true;
        if (request->deadline) { request->deadline->deleteLater(); }
        const PanCreationCompletion completion = std::exchange(request->completion, {});
        if (request->owner && completion) {
            completion({RadioControlReply::Kind::Failed, -1,
                        QStringLiteral("pan creation outcome is unconfirmed; no retry was sent")}, nullptr);
        }
    });
    deadline->start(10000);
    const RadioControlFence current = [alive, session, ticket] {
        if (!alive || !alive->controlSessionCurrent(session)) { return false; }
        const auto request = alive->m_pendingPanCreations.value(ticket);
        return request && request->owner && !request->notified;
    };
    const ReceiveDispatch result = m_backend->requestPanLifecycle({PanLifecycleAction::Create, {}, {}},
        [alive, ticket](const PanLifecycleReply& reply) {
            if (!alive) { return; }
            const auto request = alive->m_pendingPanCreations.value(ticket);
            if (!request) { return; }
            if (reply.result.kind != RadioControlReply::Kind::Accepted && reply.panId.isEmpty()
                && reply.result.kind != RadioControlReply::Kind::Cancelled && reply.result.code > 0) {
                alive->m_pendingPanCreations.remove(ticket);
                if (request->deadline) { request->deadline->stop(); request->deadline->deleteLater(); }
                const PanCreationCompletion completion = std::exchange(request->completion, {});
                if (!request->notified && request->owner && completion) { completion(reply.result, nullptr); }
                return;
            }
            request->id = reply.panId;
            if (reply.result.kind != RadioControlReply::Kind::Accepted) {
                const PanCreationCompletion completion = std::exchange(request->completion, {});
                const bool notify = !request->notified && request->owner && completion;
                request->notified = true;
                if (request->deadline) { request->deadline->stop(); request->deadline->deleteLater(); }
                if (notify) { completion(reply.result, nullptr); }
                if (!alive) { return; }
            }
            alive->settlePanCreations();
        }, current);
    if (alive && result == ReceiveDispatch::Unsupported) {
        m_pendingPanCreations.remove(ticket);
        if (request->deadline) { request->deadline->stop(); request->deadline->deleteLater(); }
    }
    return result;
}

ReceiveDispatch RadioModel::requestPanDisplay(PanadapterModel* pan, const PanDisplayRequest& request,
    const RadioControlCompletion& completion, bool announce)
{
    if (QThread::currentThread() != thread() || !request.valid() || !pan
        || panadapter(pan->panId()) != pan || !m_backend) { return ReceiveDispatch::Unsupported; }
    const RadioControlSession session = controlSession();
    const QPointer<RadioModel> alive(this);
    const QPointer<PanadapterModel> original(pan);
    const QString waterfallId = pan->waterfallId();
    const quint64 waterfallRetired = m_displayRetirements.value(waterfallId);
    const quint64 historyEpoch = m_displayHistoryEpoch;
    const quint64 revision = request.targetsWaterfall() ? pan->controlRevision() : pan->ownershipRevision();
    const quint64 intent = pan->noteDisplayIntent(request.field);
    const quint64 colorIntent = pan->displayIntentRevision(PanDisplayField::WaterfallColorGain);
    const quint64 blackIntent = pan->displayIntentRevision(PanDisplayField::WaterfallBlackLevel);
    const quint64 autoBlackIntent = pan->displayIntentRevision(PanDisplayField::WaterfallAutoBlack);
    const auto control = backendCapabilities().panDisplayControl;
    const RadioControlFence current = [alive, original, session, revision, request, intent,
                                      waterfallId, waterfallRetired, historyEpoch,
                                      colorIntent, blackIntent, autoBlackIntent] {
        if (!(alive && original && alive->controlSessionCurrent(session)
            && alive->panadapter(original->panId()) == original
            && (request.targetsWaterfall() ? original->controlRevision() : original->ownershipRevision()) == revision
            && original->ownedByClient(alive->clientHandle())
            && (!request.targetsWaterfall() || original->waterfallId() == waterfallId)
            && original->displayIntentRevision(request.field) == intent)) { return false; }
        if (!request.targetsWaterfall()) { return true; }
        if (request.field == PanDisplayField::WaterfallDefaults
            && (original->displayIntentRevision(PanDisplayField::WaterfallColorGain) != colorIntent
                || original->displayIntentRevision(PanDisplayField::WaterfallBlackLevel) != blackIntent
                || original->displayIntentRevision(PanDisplayField::WaterfallAutoBlack) != autoBlackIntent)) { return false; }
        if (alive->m_displayHistoryEpoch != historyEpoch
            || alive->m_displayRetirements.value(waterfallId) != waterfallRetired) { return false; }
        const auto observed = alive->m_radioDisplayWaterfalls.constFind(waterfallId);
        return observed == alive->m_radioDisplayWaterfalls.cend()
            || ((observed->clientHandle == 0 || observed->clientHandle == alive->clientHandle())
                && (observed->parentPanId.isEmpty() || observed->parentPanId == original->panId()));
    };
    const ReceiveDispatch result = control && control->supports(request.field) && current()
        ? m_backend->requestPanDisplay(backendPanIdFor(pan->panId()), waterfallId, request, completion, current)
        : ReceiveDispatch::Unsupported;
    if (alive && controlSessionCurrent(session) && announce) {
        reportReceiveDispatch(result, QStringLiteral("pan display"));
    }
    return result;
}

ReceiveDispatch RadioModel::requestPanDimensions(PanadapterModel* pan, int width, int height, int localPoints)
{
    const auto control = backendCapabilities().panDisplayControl;
    if (!pan || panadapter(pan->panId()) != pan || !control
        || !PanDisplayRequest{PanDisplayField::Pixels, PanPixelDimensions{width, height}}.valid()) {
        return ReceiveDispatch::Unsupported;
    }
    if (control->supports(PanDisplayField::Pixels)) {
        return requestPanDisplay(pan, {PanDisplayField::Pixels, PanPixelDimensions{width, height}}, {}, false);
    }
    if (control->supports(PanDisplayField::SpectrumPoints)) {
        return requestPanDisplay(pan, {PanDisplayField::SpectrumPoints, localPoints}, {}, false);
    }
    // Fixed native geometry needs no radio resize. The caller still updates
    // its local renderer scale; this is not evidence of a backend write.
    return ReceiveDispatch::LocalOnly;
}

void RadioModel::requestPanRfGainInfo(PanadapterModel* pan)
{
    const auto control = backendCapabilities().panLifecycleControl;
    if (!pan || !m_backend || !control || !control->supports(PanLifecycleAction::RfGainInfo)) { return; }
    const QPointer<RadioModel> alive(this);
    const QPointer<PanadapterModel> original(pan);
    const RadioControlSession session = controlSession();
    const quint64 revision = pan->ownershipRevision();
    const RadioControlFence current = [alive, original, session, revision] {
        return alive && original && alive->controlSessionCurrent(session)
            && alive->panadapter(original->panId()) == original && original->ownershipRevision() == revision;
    };
    m_backend->requestPanLifecycle({PanLifecycleAction::RfGainInfo, backendPanIdFor(pan->panId()), {}},
        [current, original](const PanLifecycleReply& reply) {
            if (current() && reply.result.kind == RadioControlReply::Kind::Accepted && reply.rfGainRange) {
                original->setRfGainInfo(reply.rfGainRange->low, reply.rfGainRange->high, reply.rfGainRange->step);
            }
        }, current);
}

} // namespace AetherSDR
