#include "RadioModel.h"
#include "core/LogManager.h"

#include <QPointer>
#include <QThread>
#include <QTimer>
#include <memory>
#include <optional>
#include <utility>

namespace AetherSDR {

void RadioModel::resetAudioControls()
{
    m_remoteRxCreateGuard.revoke();
    m_remoteRxAllocationPending = false;
    ++m_remoteRxRequestRevision;
    m_remoteRxRestartAfterCancel = false;
    const auto pending = std::exchange(m_audioPending, {});
    for (PanadapterModel* pan : m_panadapters) { pan->cancelIqBinding(); }
    for (const AudioControlGuard& guard : m_graphicEqGuards) { guard.revoke(); }
    m_graphicEqGuards.clear();
    m_audioStreams.clear();
    m_audioRetirements.clear();
    ++m_audioRetirementEpoch;
    for (const auto& request : pending) {
        request->request.guard.revoke();
        request->answered = true;
        if (request->deadline) { request->deadline->stop(); request->deadline->deleteLater(); }
    }
    const QPointer<RadioModel> alive(this);
    for (const auto& request : pending) {
        if (!request->completed) {
            request->completed = true;
            if (request->completion) {
                request->completion({{RadioControlReply::Kind::Cancelled, -1, QStringLiteral("audio session retired")}, 0});
            }
            if (!alive) { return; }
        }
    }
}

void RadioModel::applyIqStreamDelta(const AudioStreamDelta& delta)
{
    const quint32 streamId = delta.streamId;
    const auto leave = [this, streamId] {
        if (m_panStream) { m_panStream->unregisterIqStream(streamId); }
        m_daxIqModel.handleStreamRemoved(streamId);
    };
    // Any removed stream leaves the registry, whatever its type.
    if (delta.removed || delta.invalidOwner) {
        leave();
        qCDebug(lcDax) << "RadioModel: unregistered removed DAX IQ stream" << "0x" + QString::number(streamId, 16);
        return;
    }
    if (delta.kind != AudioStreamKind::Iq) { return; }
    // A stream with no owner field is taken as ours, as is 0x0 unless it is a
    // dead orphan at 0.0.0.0.
    const bool ours = delta.clientHandle ? *delta.clientHandle == ourClientHandle() : !delta.orphaned;
    if (!ours) {
        leave();
        qCDebug(lcDax) << "RadioModel: ignoring DAX IQ stream for another client"
                       << "stream=0x" + QString::number(streamId, 16);
        return;
    }
    m_daxIqModel.applyStreamDelta(delta);
    // panStream is null on a backend that carries its own IQ; there is no
    // VITA-49 stream to register against.
    const int ch = delta.channel.value_or(0);
    if (streamId && ch >= 1 && ch <= 4 && m_panStream
        && m_daxIqModel.stream(ch).exists && m_daxIqModel.stream(ch).streamId == streamId) {
        m_panStream->registerIqStream(streamId, ch);
    }
}

void RadioModel::applyAudioStreamDelta(const AudioStreamDelta& delta)
{
    if (delta.removed || delta.invalidOwner) {
        m_audioStreams.remove(delta.streamId);
        m_audioRetirements.insert(delta.streamId, ++m_audioRetirementSerial);
        if (m_audioRetirements.size() > 256) {
            m_audioRetirements.clear();
            ++m_audioRetirementEpoch;
        }
        for (const auto& pending : m_audioPending) {
            if (pending->request.streamId == delta.streamId) { pending->request.guard.revoke(); }
        }
        return;
    }
    auto stream = m_audioStreams.find(delta.streamId);
    if (stream == m_audioStreams.end()) {
        // Owner 0 is unknown (no client_handle yet, or 0x0): the model's
        // adopters decide whether such a stream is ours, as they do for pans.
        if (!delta.kind || m_audioStreams.size() >= 64) { return; }
        stream = m_audioStreams.insert(delta.streamId,
            {*delta.kind, delta.channel.value_or(0), delta.clientHandle.value_or(0), ++m_audioRetirementSerial});
    } else if ((delta.kind && *delta.kind != stream->kind)
        || (delta.clientHandle && stream->owner != 0 && *delta.clientHandle != stream->owner)
        || (delta.channel && stream->channel != 0 && *delta.channel != stream->channel)) {
        stream->incarnation = ++m_audioRetirementSerial;
        m_audioRetirements.insert(delta.streamId, m_audioRetirementSerial);
        if (m_audioRetirements.size() > 256) {
            m_audioRetirements.clear();
            ++m_audioRetirementEpoch;
        }
        for (const auto& pending : m_audioPending) {
            if (pending->request.streamId == delta.streamId) { pending->request.guard.revoke(); }
        }
    }
    if (delta.kind) { stream->kind = *delta.kind; }
    if (delta.channel) { stream->channel = *delta.channel; }
    if (delta.clientHandle) { stream->owner = *delta.clientHandle; }
}

ReceiveDispatch RadioModel::requestAudioStream(AudioStreamRequest request, const AudioStreamCompletion& completion)
{
    const auto refuse = [&completion](const QString& reason) {
        if (completion) { completion({{RadioControlReply::Kind::Refused, -1, reason}, 0}); }
        return ReceiveDispatch::Unsupported;
    };
    if (QThread::currentThread() != thread()) { return refuse(QStringLiteral("audio request is off-thread")); }
    const RadioControlSession session = controlSession();
    const std::optional<AudioStreamControl> control = backendCapabilities().audioStreamControl;
    if (!request.valid() || !m_backend || !controlSessionCurrent(session) || !control
        || !control->supports(request.kind) || m_audioPending.size() >= 64 || clientHandle() == 0
        || (request.kind == AudioStreamKind::Iq && request.action == AudioStreamAction::Create
            && backendCapabilities().daxIqSampleRates.isEmpty())
        || (request.kind == AudioStreamKind::Iq && request.action == AudioStreamAction::SetIqRate
            && !backendCapabilities().daxIqSampleRates.contains(request.sampleRate))) {
        return refuse(QStringLiteral("audio stream request unavailable"));
    }
    if (request.action == AudioStreamAction::Create) {
        for (const auto& pending : m_audioPending) {
            if (pending->request.action == AudioStreamAction::Create && pending->request.kind == request.kind
                && pending->request.channel == request.channel) {
                return refuse(QStringLiteral("waiting for the previous stream allocation to settle"));
            }
        }
    }
    quint64 incarnation = 0;
    if (request.action != AudioStreamAction::Create) {
        const auto stream = m_audioStreams.constFind(request.streamId);
        if (stream == m_audioStreams.cend() || stream->kind != request.kind
            || (stream->owner != 0 && stream->owner != clientHandle())
            || (request.channel != 0 && stream->channel != 0 && stream->channel != request.channel)) {
            return refuse(QStringLiteral("audio stream identity is not owned by this client"));
        }
        incarnation = stream->incarnation;
        for (const auto& pending : m_audioPending) {
            if (pending->request.streamId == request.streamId
                && (request.action == AudioStreamAction::Remove
                    || pending->request.action == AudioStreamAction::SetIqRate)) {
                pending->request.guard.revoke();
            }
        }
    }
    if (!request.guard.present()) { request.guard = AudioControlGuard::create(30000); }
    if (!request.guard.permits()) { return refuse(QStringLiteral("audio request has expired")); }
    const quint64 ticket = ++m_audioTicket;
    const auto pending = std::make_shared<PendingAudioControl>(PendingAudioControl{
        request, session, m_audioRetirementSerial, m_audioRetirementEpoch, incarnation, completion});
    m_audioPending.insert(ticket, pending);
    const QPointer<RadioModel> alive(this);
    const auto finish = [alive, pending, ticket](AudioStreamReply reply) {
        if (!alive || pending->answered) { return; }
        pending->answered = true;
        const bool unknownAllocation = pending->request.action == AudioStreamAction::Create
            && reply.streamId == 0 && reply.result.code == 0;
        if (!unknownAllocation) { alive->m_audioPending.remove(ticket); }
        if (pending->deadline) { pending->deadline->stop(); pending->deadline->deleteLater(); }
        const bool sameSession = alive->controlSessionCurrent(pending->session);
        bool current = sameSession && pending->request.guard.permits();
        if (reply.streamId != 0 && pending->request.action == AudioStreamAction::Create) {
            const auto existing = alive->m_audioStreams.constFind(reply.streamId);
            const bool identitySafe = sameSession && pending->retirementEpoch == alive->m_audioRetirementEpoch
                && alive->m_audioRetirements.value(reply.streamId) <= pending->retirementSerial
                && (existing == alive->m_audioStreams.cend()
                    || ((existing->owner == 0 || existing->owner == alive->clientHandle())
                    && existing->kind == pending->request.kind
                        && existing->incarnation > pending->retirementSerial
                        && (existing->channel == 0 || existing->channel == pending->request.channel)));
            if (identitySafe && reply.result.code == 0 && alive->m_audioStreams.size() < 64) {
                if (existing == alive->m_audioStreams.cend()) {
                    alive->m_audioStreams.insert(reply.streamId, {pending->request.kind, pending->request.channel,
                        alive->clientHandle(), ++alive->m_audioRetirementSerial});
                }
                if ((!current || pending->completed) && pending->request.kind != AudioStreamKind::DaxTransmit) {
                    bool newerCreate = false;
                    for (auto it = alive->m_audioPending.cbegin(); it != alive->m_audioPending.cend(); ++it) {
                        newerCreate = newerCreate || (it.key() > ticket
                            && it.value()->request.action == AudioStreamAction::Create
                            && it.value()->request.kind == pending->request.kind
                            && it.value()->request.channel == pending->request.channel);
                    }
                    if (!newerCreate) {
                        alive->requestAudioStream({pending->request.kind, AudioStreamAction::Remove, reply.streamId,
                            pending->request.channel});
                    }
                    if (!alive) { return; }
                }
            } else { current = false; }
        } else if (reply.streamId != 0) {
            const auto existing = alive->m_audioStreams.constFind(reply.streamId);
            current = current && existing != alive->m_audioStreams.cend()
                && existing->incarnation == pending->incarnation
                && (existing->owner == 0 || existing->owner == alive->clientHandle());
        }
        if (!current) { reply.result.kind = RadioControlReply::Kind::Cancelled; }
        if (!pending->completed) {
            pending->completed = true;
            if (pending->completion) { pending->completion(reply); }
        }
    };
    QTimer* deadline = new QTimer(this);
    deadline->setObjectName(QStringLiteral("audioControlDeadline"));
    deadline->setSingleShot(true);
    pending->deadline = deadline;
    connect(deadline, &QTimer::timeout, this, [alive, pending, ticket] {
        if (!alive) { return; }
        if (pending->deadline) { pending->deadline->deleteLater(); }
        if (!pending->completed) {
            pending->request.guard.revoke();
            pending->completed = true;
            if (pending->request.action != AudioStreamAction::Create) { alive->m_audioPending.remove(ticket); }
            if (pending->completion) {
                pending->completion({{RadioControlReply::Kind::Failed, -1,
                    QStringLiteral("audio stream request timed out; allocation remains unconfirmed")}, 0});
            }
        }
    });
    deadline->start(30000);
    const ReceiveDispatch result = m_backend->requestAudioStream(request, finish);
    if (result == ReceiveDispatch::Unsupported && !pending->completed) {
        finish({{RadioControlReply::Kind::Refused, -1, QStringLiteral("audio stream request not dispatched")}, 0});
    }
    return result;
}

ReceiveDispatch RadioModel::requestGraphicEq(GraphicEqRequest request)
{
    if (QThread::currentThread() != thread() || !request.valid()) { return ReceiveDispatch::Unsupported; }
    const std::optional<GraphicEqControl> control = backendCapabilities().radioGraphicEqControl;
    if (!control) { return ReceiveDispatch::LocalOnly; }
    if (!controlSessionCurrent(controlSession()) || !control->paths.contains(request.path)) {
        return ReceiveDispatch::Unsupported;
    }
    const int key = int(request.path) * 9 + (request.field == GraphicEqRequest::Field::Enabled ? 8 : request.band);
    m_graphicEqGuards.value(key).revoke();
    request.guard = AudioControlGuard::create(30000);
    m_graphicEqGuards.insert(key, request.guard);
    return m_backend->requestGraphicEq(request);
}

ReceiveDispatch RadioModel::requestPanIqBinding(PanadapterModel* pan, int channel)
{
    if (!pan || clientHandle() == 0 || !pan->ownedByClient(clientHandle())) { return ReceiveDispatch::Unsupported; }
    return requestPanDisplay(pan, {PanDisplayField::DaxIqChannel, channel});
}

void RadioModel::reassertSliceAudio(SliceModel* source)
{
    if (source && m_backend && controlSessionCurrent(controlSession()) && slice(source->sliceId()) == source
        && sliceMayBelongToUs(source->sliceId()) && !source->audioMute() && !source->externalReceiveReplacementActive()) {
        m_backend->requestSliceAudio(source->sliceId(), {SliceAudioRequest::Field::Mute, 0});
    }
}

} // namespace AetherSDR
