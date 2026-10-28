#include "FlexBackend.h"
#include "models/RadioStatusOwnership.h"

#include <QPointer>
#include <QThread>
#include <QRegularExpression>
#include <array>
#include <memory>
#include <optional>
#include <utility>

namespace AetherSDR {

void FlexBackend::decodeStreamStatus(const QString& object, const QMap<QString, QString>& kvs)
{
    const QPointer<FlexBackend> alive(this);
    decodeAudioStreamStatus(object, kvs);
    if (alive) { decodeDaxTxStatus(object, kvs); }
}

void FlexBackend::decodeAudioStreamStatus(const QString& object, const QMap<QString, QString>& kvs)
{
    if (QThread::currentThread() != thread()) { return; }
    static const QRegularExpression pattern(QStringLiteral("\\A(stream|tx_audio_stream) (0x[0-9a-fA-F]{1,8})(?: (removed))?\\z"));
    const QRegularExpressionMatch match = pattern.match(object);
    if (!match.hasMatch()) { return; }
    AudioStreamDelta delta;
    delta.streamId = RadioStatusOwnership::parseFlexId(match.captured(2));
    if (delta.streamId == 0) { return; }
    delta.removed = match.captured(3) == QLatin1String("removed") || kvs.contains(QStringLiteral("removed"))
        || kvs.value(QStringLiteral("in_use")) == QLatin1String("0");
    const QString type = match.captured(1) == QLatin1String("tx_audio_stream")
        ? QStringLiteral("dax_tx") : kvs.value(QStringLiteral("type"));
    if (type == QLatin1String("remote_audio_rx")) { delta.kind = AudioStreamKind::RemoteReceive; }
    else if (type == QLatin1String("remote_audio_tx")) { delta.kind = AudioStreamKind::RemoteTransmit; }
    else if (type == QLatin1String("dax_rx")) { delta.kind = AudioStreamKind::DaxReceive; }
    else if (type == QLatin1String("dax_tx")) { delta.kind = AudioStreamKind::DaxTransmit; }
    else if (type == QLatin1String("dax_iq")) { delta.kind = AudioStreamKind::Iq; }
    else if (kvs.contains(QStringLiteral("type"))) { delta.removed = true; }
    if (kvs.contains(QStringLiteral("client_handle"))) {
        const quint32 owner = RadioStatusOwnership::parseFlexId(kvs.value(QStringLiteral("client_handle")));
        delta.invalidOwner = owner == 0;
        if (!delta.invalidOwner) { delta.clientHandle = owner; }
    }
    const bool iqChannel = kvs.contains(QStringLiteral("daxiq_channel"));
    const QString channelKey = iqChannel ? QStringLiteral("daxiq_channel") : QStringLiteral("dax_channel");
    if (kvs.contains(channelKey)) {
        bool valid = false;
        const int channel = kvs.value(channelKey).toInt(&valid);
        if (valid && channel >= 1 && channel <= (iqChannel ? 4 : 8)) { delta.channel = channel; }
        else { delta.invalidOwner = true; }
    }
    emit audioStreamChanged(delta);
}

void FlexBackend::setAudioCommandSink(AudioCommandSink sink)
{
    m_audioSink = std::move(sink);
}

ReceiveDispatch FlexBackend::requestAudioStream(const AudioStreamRequest& request,
                                                const AudioStreamCompletion& completion)
{
    if (QThread::currentThread() != thread() || !request.valid() || !m_audioSink
        || !request.guard.permits()
        || (request.action == AudioStreamAction::SetIqRate
            && !capabilities().daxIqSampleRates.contains(request.sampleRate))) {
        return ReceiveDispatch::Unsupported;
    }
    QString command;
    if (request.action == AudioStreamAction::Remove) {
        command = QStringLiteral("stream remove 0x%1").arg(request.streamId, 0, 16);
    } else if (request.action == AudioStreamAction::SetIqRate) {
        command = QStringLiteral("stream set 0x%1 daxiq_rate=%2")
            .arg(request.streamId, 0, 16).arg(request.sampleRate);
    } else {
        switch (request.kind) {
        case AudioStreamKind::RemoteReceive:
            command = QStringLiteral("stream create type=remote_audio_rx compression=%1")
                .arg(request.compression == AudioCompression::Opus ? QStringLiteral("opus") : QStringLiteral("none"));
            break;
        case AudioStreamKind::RemoteTransmit:
            command = QStringLiteral("stream create type=remote_audio_tx compression=opus"); break;
        case AudioStreamKind::DaxReceive:
            command = QStringLiteral("stream create type=dax_rx dax_channel=%1").arg(request.channel); break;
        case AudioStreamKind::DaxTransmit:
            command = QStringLiteral("stream create type=dax_tx"); break;
        case AudioStreamKind::Iq:
            command = QStringLiteral("stream create type=dax_iq daxiq_channel=%1").arg(request.channel); break;
        }
    }
    const QPointer<FlexBackend> alive(this);
    struct ReplyState {
        bool sinkReturned{false};
        bool finished{false};
        std::optional<std::pair<int, QString>> early;
    };
    const auto state = std::make_shared<ReplyState>();
    const CommandReply reply = [alive, request, completion, state](int code, const QString& body) {
        if (!alive || state->finished) { return; }
        if (!state->sinkReturned) {
            if (!state->early) { state->early = std::make_pair(code, body); }
            return;
        }
        state->finished = true;
        quint32 id = request.streamId;
        if (code == 0 && request.action == AudioStreamAction::Create && body.size() <= 4096) {
            id = RadioStatusOwnership::parseCreateResponseStreamId(body);
        }
        RadioControlReply result{code == 0 ? RadioControlReply::Kind::Accepted : RadioControlReply::Kind::Failed, code, body};
        if (code == 0 && request.action == AudioStreamAction::Create && id == 0) {
            result = {RadioControlReply::Kind::Failed, 0, QStringLiteral("invalid audio stream creation reply; allocation unconfirmed")};
        }
        // Preserve an allocated ID on cancellation for original-session cleanup.
        if (!request.guard.permits()) { result.kind = RadioControlReply::Kind::Cancelled; }
        if (completion) { completion({result, id}); }
    };
    const auto sink = m_audioSink;
    const bool sent = sink(command, [alive, reply](int code, const QString& body) {
        if (!alive) { return; }
        if (QThread::currentThread() != alive->thread()) {
            QMetaObject::invokeMethod(alive, [reply, code, body] { reply(code, body); });
        } else { reply(code, body); }
    }, request.guard);
    state->sinkReturned = true;
    if (!alive) { return ReceiveDispatch::Unsupported; }
    if (!sent) { reply(-1, QStringLiteral("audio stream command not dispatched")); }
    else if (state->early) { reply(state->early->first, state->early->second); }
    return sent ? ReceiveDispatch::Dispatched : ReceiveDispatch::Unsupported;
}

ReceiveDispatch FlexBackend::requestGraphicEq(const GraphicEqRequest& request)
{
    if (QThread::currentThread() != thread() || !request.valid() || !m_audioSink || !request.guard.permits()) {
        return ReceiveDispatch::Unsupported;
    }
    const QString path = request.path == GraphicEqRequest::Path::Transmit ? QStringLiteral("TXsc") : QStringLiteral("RXsc");
    static constexpr std::array<int, 8> kBands{63, 125, 250, 500, 1000, 2000, 4000, 8000};
    const QString field = request.field == GraphicEqRequest::Field::Enabled
        ? QStringLiteral("mode=%1").arg(request.value ? QStringLiteral("True") : QStringLiteral("False"))
        : QStringLiteral("%1Hz=%2").arg(kBands[request.band]).arg(request.value);
    return m_audioSink(QStringLiteral("eq %1 %2").arg(path, field), {}, request.guard)
        ? ReceiveDispatch::Dispatched : ReceiveDispatch::Unsupported;
}

} // namespace AetherSDR
