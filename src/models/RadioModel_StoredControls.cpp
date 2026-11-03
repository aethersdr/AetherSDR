#include "models/RadioModel.h"

#include <QList>
#include <QMap>
#include <QPointer>
#include <QThread>
#include <QTimer>
#include <memory>
#include <optional>

namespace AetherSDR {

ReceiveDispatch RadioModel::requestProfile(const ProfileRequest& request,
                                          QObject* replyContext,
                                          const RadioControlCompletion& completion,
                                          std::optional<RadioControlSession> expectedSession)
{
    if (QThread::currentThread() != thread()) { return ReceiveDispatch::Unsupported; }
    const QPointer<RadioModel> alive(this);
    const RadioControlSession session = controlSession();
    const RadioControlCompletion reply = fencedSetupCompletion(replyContext, completion, true);
    const auto refuse = [&] {
        if (reply) { reply({RadioControlReply::Kind::Refused, -1, QStringLiteral("Profile request not dispatched.")}); }
        if (alive && request.action != ProfileRequest::Action::Refresh) {
            reportReceiveDispatch(ReceiveDispatch::Unsupported, QStringLiteral("profiles"));
        }
        return ReceiveDispatch::Unsupported;
    };
    if (!m_backend || !request.valid() || !backendCapabilities().hasProfiles
        || (request.kind == ProfileRequest::Kind::Cw && !backendCapabilities().cwProfileControl.has_value())
        || !controlSessionCurrent(session) || (expectedSession && *expectedSession != session)) {
        return refuse();
    }
    if (request.action == ProfileRequest::Action::Create) {
        const QString name = QString(request.name).remove(QLatin1Char('*'));
        const QStringList names = request.kind == ProfileRequest::Kind::Transmit
            ? m_transmitModel.profileList()
            : request.kind == ProfileRequest::Kind::Cw ? m_transmitModel.cwProfileList()
            : m_transmitModel.micProfileList();
        if (name.trimmed().isEmpty() || names.contains(name)) { return refuse(); }
    }
    const RadioControlFence current = [alive, session] {
        return alive && alive->controlSessionCurrent(session);
    };
    const ReceiveDispatch result = m_backend->requestProfile(request, reply, current);
    if (result == ReceiveDispatch::Unsupported && alive) { return refuse(); }
    return result;
}

ReceiveDispatch RadioModel::requestMemory(const MemoryRequest& request,
                                         QObject* replyContext,
                                         const RadioControlCompletion& completion,
                                         std::optional<RadioControlSession> expectedSession)
{
    if (QThread::currentThread() != thread()) { return ReceiveDispatch::Unsupported; }
    const QPointer<RadioModel> alive(this);
    const QPointer<QObject> owner(replyContext ? replyContext : this);
    const RadioControlSession session = controlSession();
    const bool local = usesLocalMemoryBank();
    const MemoryEntry original = m_memories.value(request.index);
    const quint64 revision = m_memorySlotRevisions.value(request.index);
    const auto intent = std::make_shared<quint64>(0);
    const RadioControlCompletion reply = fencedSetupCompletion(replyContext,
        [alive, owner, session, local, request, original, revision, intent, completion](const RadioControlReply& result) {
        if (!alive) { return; }
        if (alive->usesLocalMemoryBank() != local) {
            if (completion) { completion({RadioControlReply::Kind::Cancelled, -1, QStringLiteral("Original memory store ended.")}); }
            return;
        }
        if (!local && result.kind == RadioControlReply::Kind::Accepted
            && *intent == alive->m_memoryWriteIntents.value(request.index)
            && alive->m_memorySlotRevisions.value(request.index) == revision
            && alive->m_memories.value(request.index) == original) {
            if (request.action == MemoryRequest::Action::Update) {
                alive->m_memories.insert(request.index, request.patch.appliedTo(original));
                ++alive->m_memorySlotRevisions[request.index];
                emit alive->memoryChanged(request.index);
            } else if (request.action == MemoryRequest::Action::Remove) {
                alive->m_memories.remove(request.index);
                ++alive->m_memorySlotRevisions[request.index];
                ++alive->m_memorySlotRetirements[request.index];
                emit alive->memoryRemoved(request.index);
            }
        }
        // Cache observers can synchronously retire the reply owner or the session.
        if (!alive || !owner || !completion) { return; }
        if (alive->controlSession() != session || alive->usesLocalMemoryBank() != local
            || (!local && !alive->controlSessionCurrent(session))) {
            completion({RadioControlReply::Kind::Cancelled, -1, QStringLiteral("Original memory session ended.")});
        } else {
            completion(result);
        }
    }, !local);
    const auto refuse = [&](const QString& message) {
        if (reply) { reply({RadioControlReply::Kind::Refused, -1, message}); }
        return ReceiveDispatch::Unsupported;
    };
    if (!request.valid() || (expectedSession && *expectedSession != session)
        || (request.expectedRevision && *request.expectedRevision != revision)) {
        return refuse(QStringLiteral("Invalid memory request or original session ended."));
    }
    if (request.action != MemoryRequest::Action::Create && !m_memories.contains(request.index)) {
        return refuse(QStringLiteral("Unknown memory slot."));
    }
    if (local) {
        LocalMemoryBank::EditResult result;
        switch (request.action) {
        case MemoryRequest::Action::Create:
            return refuse(QStringLiteral("A local memory requires a complete snapshot."));
        case MemoryRequest::Action::Update: result = m_localMemories.updateEntry(request.index, request.patch); break;
        case MemoryRequest::Action::Remove: result = m_localMemories.removeEntry(request.index); break;
        case MemoryRequest::Action::Recall:
            if (!isConnected() || !recallCachedMemory(request.index)) {
                return refuse(QStringLiteral("Memory cannot be recalled in this session."));
            }
            if (!alive) { return ReceiveDispatch::LocalOnly; }
            if (reply) { QTimer::singleShot(0, this, [reply] { reply({RadioControlReply::Kind::Accepted, 0, {}}); }); }
            return ReceiveDispatch::LocalOnly;
        }
        if (result.code != 0) { return refuse(result.message); }
        ++m_memorySlotRevisions[result.index];
        if (request.action == MemoryRequest::Action::Remove) {
            m_memories.remove(result.index);
            ++m_memorySlotRetirements[result.index];
            emit memoryRemoved(result.index);
        } else {
            m_memories.insert(result.index, m_localMemories.entries().value(result.index));
            emit memoryChanged(result.index);
        }
        if (!alive) { return ReceiveDispatch::LocalOnly; }
        if (reply) {
            QTimer::singleShot(0, this, [reply, result] {
                reply({RadioControlReply::Kind::Accepted, 0, QString::number(result.index)});
            });
        }
        return ReceiveDispatch::LocalOnly;
    }
    const RadioCapabilities caps = backendCapabilities();
    if (!m_backend || !controlSessionCurrent(session) || !caps.persistsMemories
        || (request.action == MemoryRequest::Action::Recall ? !caps.canApplyMemories : !caps.canWriteMemories)) {
        return refuse(QStringLiteral("Radio memory operation is unavailable."));
    }
    MemoryRequest effective = request;
    const QPointer<SliceModel> recallTarget(request.sliceId >= 0 ? slice(request.sliceId) : nullptr);
    if (request.recall) {
        if (!recallTarget) { return refuse(QStringLiteral("Memory recall target no longer exists.")); }
        effective.recall = m_memories.value(request.index);
    }
    const RadioControlFence current = [alive, session, effective, recallTarget] {
        return alive && alive->controlSessionCurrent(session) && !alive->usesLocalMemoryBank()
            && (!effective.recall || (recallTarget && alive->slice(effective.sliceId) == recallTarget));
    };
    if (request.action == MemoryRequest::Action::Update || request.action == MemoryRequest::Action::Remove) {
        *intent = ++m_memoryWriteIntents[request.index];
    }
    const ReceiveDispatch result = m_backend->requestMemory(effective, reply, current);
    if (result == ReceiveDispatch::Unsupported && alive) {
        return refuse(QStringLiteral("Memory request not dispatched."));
    }
    return result;
}

void RadioModel::createMemory(const MemoryEntry& entry, QObject* replyContext,
                             const MemoryCreateCompletion& completion,
                             std::optional<RadioControlSession> expectedSession)
{
    if (QThread::currentThread() != thread()) { return; }
    const QPointer<RadioModel> model(this);
    const QPointer<QObject> owner(replyContext ? replyContext : this);
    const RadioControlSession session = controlSession();
    const bool local = usesLocalMemoryBank();
    const MemoryPatch patch = MemoryPatch::fromEntry(entry);
    const auto notify = [model, owner, completion](const RadioControlReply& result, int index) {
        if (model && owner && completion) { completion(result, index); }
    };
    if (!patch.valid()
        || (expectedSession && *expectedSession != session)) {
        notify({RadioControlReply::Kind::Refused, -1, QStringLiteral("Invalid memory or original session ended.")}, -1);
        return;
    }
    if (local) {
        const LocalMemoryBank::EditResult result = m_localMemories.createEntry(patch.appliedTo(entry));
        if (result.code == 0) {
            ++m_memorySlotRevisions[result.index];
            m_memories.insert(result.index, m_localMemories.entries().value(result.index));
            emit memoryChanged(result.index);
        }
        if (!model) { return; }
        QTimer::singleShot(0, this, [model, owner, session, notify, result] {
            if (!model || !owner) { return; }
            if (model->controlSession() != session || !model->usesLocalMemoryBank()) {
                notify({RadioControlReply::Kind::Cancelled, -1, QStringLiteral("Original memory store ended.")}, -1);
            } else {
                notify({result.code == 0 ? RadioControlReply::Kind::Accepted : RadioControlReply::Kind::Failed,
                        result.code, result.message}, result.index);
            }
        });
        return;
    }
    const RadioControlFence current = [model, owner, session] {
        return model && owner && model->controlSessionCurrent(session) && !model->usesLocalMemoryBank();
    };
    const QList<int> existingIndices = m_memories.keys();
    const QMap<int, quint64> retirements = m_memorySlotRetirements;
    requestMemory({MemoryRequest::Action::Create, -1, {}}, this,
        [model, current, session, patch, existingIndices, retirements, notify](const RadioControlReply& created) {
        if (!current()) { notify({RadioControlReply::Kind::Cancelled, -1, QStringLiteral("Original memory session ended.")}, -1); return; }
        if (created.kind != RadioControlReply::Kind::Accepted) { notify(created, -1); return; }
        bool ok = false;
        const int index = created.message.trimmed().toInt(&ok);
        if (!ok || index < 0 || existingIndices.contains(index)) {
            notify({RadioControlReply::Kind::Failed, -1, QStringLiteral("The radio returned an invalid memory id.")}, -1);
            return;
        }
        const quint64 retirement = retirements.value(index);
        if (model->m_memorySlotRetirements.value(index) != retirement) {
            notify({RadioControlReply::Kind::Cancelled, -1,
                    QStringLiteral("Original slot ended; population was not attempted.")}, index);
            return;
        }
        // A create response can precede the new slot's status; reserve it for the update.
        if (!model->m_memories.contains(index)) {
            MemoryEntry placeholder; placeholder.index = index;
            model->m_memories.insert(index, placeholder);
        }
        const MemoryEntry allocated = model->m_memories.value(index);
        model->requestMemory({MemoryRequest::Action::Update, index, patch}, model,
            [model, current, session, index, allocated, retirement, notify](const RadioControlReply& updated) {
            if (!model) { return; }
            if (!current() || model->m_memorySlotRetirements.value(index) != retirement) {
                notify({RadioControlReply::Kind::Cancelled, -1,
                        QStringLiteral("Original slot ended; partial cleanup was not attempted.")}, index);
                return;
            }
            if (updated.kind == RadioControlReply::Kind::Accepted) {
                notify(updated, index);
                return;
            }
            if (model->m_memories.value(index) != allocated
                || updated.kind == RadioControlReply::Kind::Cancelled
                || updated.code == -1) {
                notify({updated.kind, updated.code, updated.message + QStringLiteral(" Partial slot cleanup was not attempted.")}, index);
                return;
            }
            model->requestMemory({MemoryRequest::Action::Remove, index, {}}, model,
                [notify, index, updated](const RadioControlReply& removed) {
                RadioControlReply failure = updated;
                if (removed.kind != RadioControlReply::Kind::Accepted) {
                    failure.message += QStringLiteral(" Cleanup also failed: ") + removed.message;
                }
                notify(failure, index);
            }, session);
        }, session);
    }, session);
}

ReceiveDispatch RadioModel::recallMemory(int index, int sliceId)
{
    const QPointer<SliceModel> target(slice(sliceId));
    if (!target || !m_memories.contains(index) || !m_memories.value(index).recallable) {
        return ReceiveDispatch::Unsupported;
    }
    MemoryRequest request{MemoryRequest::Action::Recall, index, {}};
    request.sliceId = sliceId;
    request.recall = m_memories.value(index);
    return requestMemory(request, this);
}

} // namespace AetherSDR
