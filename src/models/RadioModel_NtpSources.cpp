#include "models/RadioModel.h"

#include <QPointer>
#include <QThread>
#include <memory>

namespace AetherSDR {

ReceiveDispatch RadioModel::requestNtpSource(const NtpSourceRequest& request,
                                            QObject* replyContext,
                                            const RadioControlCompletion& completion)
{
    if (QThread::currentThread() != thread()) { return ReceiveDispatch::Unsupported; }
    const QPointer<RadioModel> alive(this);
    const RadioControlSession session = controlSession();
    const RadioControlCompletion reply = fencedSetupCompletion(replyContext, completion, true);
    const auto refuse = [&] {
        if (reply) {
            reply({RadioControlReply::Kind::Refused, -1,
                   QStringLiteral("NTP source request not dispatched.")});
        }
        if (alive) {
            reportReceiveDispatch(ReceiveDispatch::Unsupported, QStringLiteral("NTP source"));
        }
        return ReceiveDispatch::Unsupported;
    };
    if (!m_backend || !request.valid() || !backendCapabilities().ntpSourceControl.has_value()
        || !controlSessionCurrent(session)) {
        return refuse();
    }
    const RadioControlFence current = [alive, session] {
        return alive && alive->controlSessionCurrent(session);
    };
    const ReceiveDispatch result = m_backend->requestNtpSource(request, reply, current);
    if (result == ReceiveDispatch::Unsupported && alive) { return refuse(); }
    return result;
}

void RadioModel::applyNtpSourceChanges(const NtpSourceDelta& delta)
{
    if (delta.id.isEmpty()) { return; }
    qsizetype index = -1;
    for (qsizetype i = 0; i < m_ntpSources.size(); ++i) {
        if (m_ntpSources.at(i).id == delta.id) { index = i; break; }
    }
    if (delta.removed) {
        if (index < 0) { return; }
        m_ntpSources.removeAt(index);
        emit ntpSourcesChanged();
        return;
    }
    NtpSourceState next = index >= 0 ? m_ntpSources.at(index) : NtpSourceState{};
    next.id = delta.id;
    if (delta.address) { next.address = *delta.address; }
    if (delta.directive) { next.directive = *delta.directive; }
    if (delta.description) { next.description = *delta.description; }
    if (delta.enabled) { next.enabled = *delta.enabled; }
    if (delta.selected) { next.selected = *delta.selected; }
    if (delta.prefer) { next.prefer = *delta.prefer; }
    if (delta.state) {
        next.state = *delta.state;
        // "unknown" means chrony has no data for the source: forget stale numbers
        // (FlexLib NtpSource.cs StatusUpdate).
        if (next.state == QLatin1String("unknown")) { next.stratum = -1; next.reach = -1; }
    }
    if (delta.stratum) { next.stratum = *delta.stratum; }
    if (delta.reach) { next.reach = *delta.reach; }
    if (index >= 0) {
        if (m_ntpSources.at(index) == next) { return; }
        m_ntpSources[index] = next;
    } else {
        m_ntpSources.append(next);
    }
    emit ntpSourcesChanged();
}

void RadioModel::syncNtpSources(bool connected, const RadioCapabilities& caps)
{
    if (!connected || !caps.ntpSourceControl.has_value()) {
        m_ntpSubscribedSession.reset();
        if (!m_ntpSources.isEmpty()) { m_ntpSources.clear(); emit ntpSourcesChanged(); }
        return;
    }
    if (m_ntpSubscribedSession == controlSession()) { return; }
    // A new session reports every source again after the subscription.
    if (!m_ntpSources.isEmpty()) { m_ntpSources.clear(); emit ntpSourcesChanged(); }
    // Marked only once sent, so a session that cannot send yet (still closing, or
    // capabilities not final) tries again on the next publish.
    if (requestNtpSource({NtpSourceRequest::Action::Subscribe, {}}) == ReceiveDispatch::Dispatched) {
        m_ntpSubscribedSession = controlSession();
    }
}

bool RadioModel::replaceNtpSource(const NtpSourceDefinition& definition, QObject* replyContext,
                                  std::function<void(const NtpSourceReplaceResult&)> completion)
{
    using Action = NtpSourceRequest::Action;
    using Outcome = NtpSourceReplaceResult::Outcome;
    if (QThread::currentThread() != thread() || !definition.valid()
        || m_ntpReplacing.contains(definition.id)
        || !backendCapabilities().ntpSourceControl.has_value()
        || !controlSessionCurrent(controlSession())) {
        return false;
    }
    const NtpSourceState* found = nullptr;
    for (const NtpSourceState& source : m_ntpSources) {
        if (source.id == definition.id) { found = &source; break; }
    }
    if (!found) { return false; }
    const NtpSourceDefinition original{found->id, found->address, found->directive,
                                       found->description, found->prefer};
    const bool wasEnabled = found->enabled;

    struct State {
        QString id;
        QPointer<QObject> owner;
        bool hasOwner{false};
        std::function<void(const NtpSourceReplaceResult&)> completion;
        bool done{false};
    };
    const auto state = std::make_shared<State>();
    state->id = definition.id;
    state->owner = replyContext;
    state->hasOwner = replyContext != nullptr;
    state->completion = std::move(completion);
    m_ntpReplacing.insert(definition.id);
    emit ntpSourcesChanged();

    const QPointer<RadioModel> alive(this);
    const auto finish = [alive, state](Outcome outcome, const QString& message) {
        if (state->done) { return; }
        state->done = true;
        if (alive) {
            alive->m_ntpReplacing.remove(state->id);
            emit alive->ntpSourcesChanged();
        }
        if (state->completion && (!state->hasOwner || state->owner)) {
            state->completion({outcome, message});
        }
    };
    const auto why = [](const RadioControlReply& reply) {
        return reply.message.trimmed().isEmpty() ? QStringLiteral("no reason given")
                                                 : reply.message.trimmed();
    };
    const auto cancelled = [](const RadioControlReply& reply) {
        return reply.kind == RadioControlReply::Kind::Cancelled;
    };
    // A created source starts enabled; put a disabled one back the way it was.
    const auto restoreDisabled = [alive, wasEnabled, finish, why]
        (const QString& id, Outcome outcome, const QString& message) {
        if (wasEnabled || !alive) { finish(outcome, message); return; }
        // A request that cannot be sent answers through the same completion.
        alive->requestNtpSource(
            {Action::Disable, {id, {}, {}, {}, false}}, alive,
            [finish, why, outcome, message](const RadioControlReply& reply) {
                if (reply.kind == RadioControlReply::Kind::Accepted) { finish(outcome, message); return; }
                const QString note = QStringLiteral("it was left enabled: %1").arg(why(reply));
                finish(outcome, message.isEmpty() ? note : message + QStringLiteral("; ") + note);
            });
    };

    // A step that cannot be sent answers through its own completion, so every
    // path below ends in finish().
    requestNtpSource(
        {Action::Remove, {definition.id, {}, {}, {}, false}}, this,
        [alive, definition, original, finish, why, cancelled, restoreDisabled](const RadioControlReply& reply) {
            if (cancelled(reply)) { finish(Outcome::Cancelled, reply.message); return; }
            if (reply.kind != RadioControlReply::Kind::Accepted) {
                finish(Outcome::Unchanged, QStringLiteral("the radio refused the remove: %1").arg(why(reply)));
                return;
            }
            if (!alive) { return; }
            alive->requestNtpSource({Action::Create, definition}, alive,
                [alive, definition, original, finish, why, cancelled, restoreDisabled]
                (const RadioControlReply& created) {
                    if (cancelled(created)) { finish(Outcome::Cancelled, created.message); return; }
                    if (created.kind == RadioControlReply::Kind::Accepted) {
                        restoreDisabled(definition.id, Outcome::Replaced, {});
                        return;
                    }
                    const QString refusedNew =
                        QStringLiteral("the radio refused the new definition: %1").arg(why(created));
                    if (!alive) { return; }
                    alive->requestNtpSource({Action::Create, original}, alive,
                        [finish, why, cancelled, restoreDisabled, original, refusedNew]
                        (const RadioControlReply& restored) {
                            if (cancelled(restored)) { finish(Outcome::Cancelled, restored.message); return; }
                            if (restored.kind == RadioControlReply::Kind::Accepted) {
                                restoreDisabled(original.id, Outcome::Restored, refusedNew);
                                return;
                            }
                            finish(Outcome::Lost, refusedNew
                                + QStringLiteral("; re-creating the original also failed: %1").arg(why(restored)));
                        });
                });
        });
    return true;
}

} // namespace AetherSDR
