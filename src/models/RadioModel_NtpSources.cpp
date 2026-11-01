#include "models/RadioModel.h"

#include <QPointer>
#include <QThread>
#include <memory>

namespace AetherSDR {

namespace {
const QString kSessionEnded = QStringLiteral("the original radio session ended");
}

ReceiveDispatch RadioModel::requestNtpSource(const NtpSourceRequest& request,
                                            QObject* replyContext,
                                            const RadioControlCompletion& completion,
                                            std::optional<RadioControlSession> expectedSession)
{
    // An edit owns its id until it ends: another command for that id would change
    // the source under the remove-and-create and its rollback.
    if (request.action != NtpSourceRequest::Action::Subscribe
        && m_ntpReplacing.contains(request.definition.id)) {
        const QPointer<QObject> owner(replyContext ? replyContext : this);
        if (completion && owner) {
            completion({RadioControlReply::Kind::Refused, -1,
                        QStringLiteral("%1 is being updated").arg(request.definition.id)});
        }
        return ReceiveDispatch::Unsupported;
    }
    return dispatchNtpSource(request, replyContext, completion, expectedSession);
}

ReceiveDispatch RadioModel::dispatchNtpSource(const NtpSourceRequest& request,
                                             QObject* replyContext,
                                             const RadioControlCompletion& completion,
                                             std::optional<RadioControlSession> expectedSession)
{
    if (QThread::currentThread() != thread()) { return ReceiveDispatch::Unsupported; }
    const QPointer<RadioModel> alive(this);
    const RadioControlSession session = controlSession();
    if (expectedSession && *expectedSession != session) {
        const QPointer<QObject> owner(replyContext ? replyContext : this);
        if (completion && owner) { completion({RadioControlReply::Kind::Cancelled, -1, kSessionEnded}); }
        return ReceiveDispatch::Unsupported;
    }
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
    const auto guard = m_ntpReplacing.find(delta.id);
    if (delta.removed) {
        // An edit's own remove keeps the row for the re-create (Radio.cs:14469).
        if (guard != m_ntpReplacing.end()) { guard->removedSeen = true; return; }
        if (index < 0) { return; }
        m_ntpSources.removeAt(index);
        emit ntpSourcesChanged();
        return;
    }
    bool changed = false;
    // The re-create after the edit's remove: the row is the radio's again.
    if (guard != m_ntpReplacing.end() && guard->removedSeen) {
        guard->removedSeen = false;
        if (!guard->operating) { m_ntpReplacing.erase(guard); changed = true; }
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
        if (m_ntpSources.at(index) != next) { m_ntpSources[index] = next; changed = true; }
    } else {
        m_ntpSources.append(next);
        changed = true;
    }
    if (changed) { emit ntpSourcesChanged(); }
}

void RadioModel::syncNtpSources(bool connected, const RadioCapabilities& caps)
{
    const auto clear = [this] {
        // Edits still running end through their own fenced replies.
        bool changed = !m_ntpSources.isEmpty();
        m_ntpSources.clear();
        for (auto it = m_ntpReplacing.begin(); it != m_ntpReplacing.end();) {
            if (it->operating) { ++it; } else { it = m_ntpReplacing.erase(it); changed = true; }
        }
        if (changed) { emit ntpSourcesChanged(); }
    };
    if (!connected || !caps.ntpSourceControl.has_value()) {
        m_ntpSubscribedSession.reset();
        clear();
        return;
    }
    if (m_ntpSubscribedSession == controlSession()) { return; }
    // A new session reports every source again after the subscription.
    clear();
    // Marked only once sent, so a session that cannot send yet (still closing, or
    // capabilities not final) tries again on the next publish.
    if (requestNtpSource({NtpSourceRequest::Action::Subscribe, {}}) == ReceiveDispatch::Dispatched) {
        m_ntpSubscribedSession = controlSession();
    }
}

void RadioModel::endNtpSourceReplace(const QString& id, NtpSourceReplaceResult::Outcome outcome)
{
    using Outcome = NtpSourceReplaceResult::Outcome;
    const auto guard = m_ntpReplacing.find(id);
    if (guard == m_ntpReplacing.end()) { return; }
    const bool removedLast = guard->removedSeen;
    if (removedLast && (outcome == Outcome::Replaced || outcome == Outcome::Restored)) {
        // The re-create status is still on its way; keep the row until it lands.
        guard->operating = false;
    } else {
        m_ntpReplacing.erase(guard);
        // Lost: the radio accepted the remove and holds neither definition.
        // Otherwise drop the row only if the radio's last word for it was removed.
        if (outcome == Outcome::Lost || removedLast) {
            for (qsizetype i = 0; i < m_ntpSources.size(); ++i) {
                if (m_ntpSources.at(i).id == id) { m_ntpSources.removeAt(i); break; }
            }
        }
    }
    emit ntpSourcesChanged();
}

bool RadioModel::replaceNtpSource(const NtpSourceDefinition& definition, QObject* replyContext,
                                  std::function<void(const NtpSourceReplaceResult&)> completion,
                                  std::optional<RadioControlSession> expectedSession)
{
    using Action = NtpSourceRequest::Action;
    using Outcome = NtpSourceReplaceResult::Outcome;
    using Kind = RadioControlReply::Kind;
    const RadioControlSession session = controlSession();
    if (QThread::currentThread() != thread() || !definition.valid()
        || m_ntpReplacing.contains(definition.id)
        || !backendCapabilities().ntpSourceControl.has_value()
        || (expectedSession && *expectedSession != session)
        || !controlSessionCurrent(session)) {
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

    const QPointer<RadioModel> alive(this);
    const auto finish = [alive, state](Outcome outcome, const QString& message) {
        if (state->done) { return; }
        state->done = true;
        if (alive) { alive->endNtpSourceReplace(state->id, outcome); }
        if (state->completion && (!state->hasOwner || state->owner)) {
            state->completion({outcome, message});
        }
    };
    const auto why = [](const RadioControlReply& reply) {
        return reply.message.trimmed().isEmpty() ? QStringLiteral("no reason given")
                                                 : reply.message.trimmed();
    };
    // Cancelled or unanswered: the radio's state is unknown, so nothing more is
    // sent and no outcome is claimed.
    const auto unsettled = [finish](const RadioControlReply& reply, const char* step) {
        if (reply.kind == Kind::Cancelled) { finish(Outcome::Cancelled, reply.message); return true; }
        // fencedSetupCompletion's local 30 s timeout, not a reply from the radio.
        if (reply.kind == Kind::Failed && reply.code == -1) {
            finish(Outcome::NoAnswer, QStringLiteral("the radio did not answer the %1").arg(QLatin1String(step)));
            return true;
        }
        return false;
    };
    // Every step is fenced to the session the edit started in.
    const auto send = [alive, session](const NtpSourceRequest& request, const RadioControlCompletion& reply) {
        if (alive) { alive->dispatchNtpSource(request, alive, reply, session); }
    };
    // A created source starts enabled; put a disabled one back the way it was.
    const auto restoreDisabled = [wasEnabled, finish, why, unsettled, send]
        (const QString& id, Outcome outcome, const QString& message) {
        if (wasEnabled) { finish(outcome, message); return; }
        send({Action::Disable, {id, {}, {}, {}, false}},
            [finish, why, unsettled, outcome, message](const RadioControlReply& reply) {
                if (unsettled(reply, "disable")) { return; }
                if (reply.kind == Kind::Accepted) { finish(outcome, message); return; }
                const QString note = QStringLiteral("it was left enabled: %1").arg(why(reply));
                finish(outcome, message.isEmpty() ? note : message + QStringLiteral("; ") + note);
            });
    };

    m_ntpReplacing.insert(definition.id, NtpReplaceGuard{wasEnabled});
    emit ntpSourcesChanged();
    // An observer of that signal can end the session; the first step's fence
    // then answers Cancelled and nothing is sent.

    // A step that cannot be sent answers through its own completion, so every
    // path below ends in finish().
    send({Action::Remove, {definition.id, {}, {}, {}, false}},
        [definition, original, finish, why, unsettled, send, restoreDisabled](const RadioControlReply& reply) {
            if (unsettled(reply, "remove")) { return; }
            if (reply.kind != Kind::Accepted) {
                finish(Outcome::Unchanged, QStringLiteral("the radio refused the remove: %1").arg(why(reply)));
                return;
            }
            send({Action::Create, definition},
                [definition, original, finish, why, unsettled, send, restoreDisabled]
                (const RadioControlReply& created) {
                    if (unsettled(created, "create")) { return; }
                    if (created.kind == Kind::Accepted) {
                        restoreDisabled(definition.id, Outcome::Replaced, {});
                        return;
                    }
                    const QString refusedNew =
                        QStringLiteral("the radio refused the new definition: %1").arg(why(created));
                    send({Action::Create, original},
                        [finish, why, unsettled, restoreDisabled, original, refusedNew]
                        (const RadioControlReply& restored) {
                            if (unsettled(restored, "re-create of the original")) { return; }
                            if (restored.kind == Kind::Accepted) {
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
