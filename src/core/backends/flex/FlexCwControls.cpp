#include "FlexCwControls.h"
#include "PanadapterStream.h"
#include "core/LogManager.h"
#include <QtEndian>
#include <QRegularExpression>
#include <algorithm>
#include <cstring>
#include <utility>

namespace AetherSDR {
namespace {
qint64 monotonicMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
}
FlexCwControls::FlexCwControls(PanadapterStream* stream, QObject* parent)
    : QObject(parent), m_stream(stream)
{
    m_createDeadline.setSingleShot(true);
    m_pttDeadline.setSingleShot(true);
    connect(&m_createDeadline, &QTimer::timeout, this, [this] {
        m_createGuard.cancel();
        qCWarning(lcCw) << "NetCW stream request timed out; outcome unknown";
    });
    connect(&m_pttDeadline, &QTimer::timeout, this, [this] {
        finishLocalPtt({RadioControlReply::Kind::Failed, -1,
            QStringLiteral("Local PTT request timed out; ownership is unconfirmed.")});
    });
}
FlexCwControls::~FlexCwControls() { retire(); }

void FlexCwControls::retire()
{
    m_session.retireSession();
    m_streamGuard.retireSession();
    m_createDeadline.stop();
    m_createGuard.cancel();
    m_streamId = 0;
    m_handle = 0;
    m_index = 1;
    m_packetCount = 0;
    m_clockStartMs = -1;
    m_lastSendMs = -1;
    m_prepared = false;
    m_current = {};
    m_session = SessionCommandGuard::create();
    finishLocalPtt({RadioControlReply::Kind::Cancelled, -1,
        QStringLiteral("The original radio session ended.")});
}

void FlexCwControls::prepare(quint32 handle, const RadioControlFence& current)
{
    if (m_prepared || !m_controlSink || !current || !current()) { return; }
    m_prepared = true;
    m_handle = handle;
    m_current = current;
    m_createGuard = m_session.request();
    const SessionCommandGuard guard = m_createGuard;
    const QPointer<FlexCwControls> self(this);
    m_createDeadline.start(30000);
    const auto sink = m_controlSink;
    const bool sent = sink(QStringLiteral("stream create netcw"), guard,
        [self, guard, current](const RadioControlReply& reply) {
            if (!self || !guard.current() || !current()) { return; }
            guard.cancel();
            self->m_createDeadline.stop();
            static const QRegularExpression idPattern(QStringLiteral("^(?:0[xX])?[0-9A-Fa-f]{1,8}$"));
            const QString body = reply.message.trimmed();
            bool valid = false;
            const quint32 id = body.toUInt(&valid, 16);
            if (reply.kind != RadioControlReply::Kind::Accepted || !valid || id == 0
                || !idPattern.match(body).hasMatch()) {
                qCWarning(lcCw) << "NetCW stream unavailable; code" << reply.code;
                return;
            }
            self->m_streamGuard = SessionCommandGuard::create();
            self->m_streamId = id;
            self->m_index = 1;
            self->m_packetCount = 0;
            self->m_clockStartMs = -1;
            self->m_lastSendMs = -1;
        });
    if (self && !sent) {
        guard.cancel();
        self->m_createDeadline.stop();
    }
}

void FlexCwControls::finishLocalPtt(const RadioControlReply& reply)
{
    m_pttGuard.cancel();
    m_pttDeadline.stop();
    const RadioControlCompletion completion = std::exchange(m_pttCompletion, {});
    if (completion) { completion(reply); }
}

ReceiveDispatch FlexCwControls::requestLocalPtt(const RadioControlCompletion& completion,
                                               const RadioControlFence& current)
{
    if (m_pttDeadline.isActive() || !m_controlSink || !current || !current()) {
        return ReceiveDispatch::Unsupported;
    }
    m_pttCompletion = completion;
    m_pttGuard = m_session.request();
    const SessionCommandGuard guard = m_pttGuard;
    const QPointer<FlexCwControls> self(this);
    m_pttDeadline.start(30000);
    const auto sink = m_controlSink;
    const bool sent = sink(QStringLiteral("client set local_ptt=1"), guard,
        [self, guard, current](const RadioControlReply& reply) {
            if (!self || !guard.current()) { return; }
            self->finishLocalPtt(current() ? reply : RadioControlReply{
                RadioControlReply::Kind::Cancelled, -1, QStringLiteral("The original radio session ended.")});
        });
    if (self && !sent && guard.current()) {
        self->finishLocalPtt({RadioControlReply::Kind::Refused, -1, QStringLiteral("Local PTT request was not dispatched.")});
    }
    return sent ? ReceiveDispatch::Dispatched : ReceiveDispatch::Unsupported;
}

void FlexCwControls::observeStreamRemoved(quint32 streamId)
{
    if (streamId == m_streamId) {
        m_streamGuard.retireSession();
        m_streamId = 0;
    }
}

QByteArray FlexCwControls::packet(const QByteArray& payload)
{
    const qsizetype padded = (payload.size() + 3) & ~qsizetype(3);
    QByteArray bytes(28 + padded, '\0');
    const quint32 header = 0x38D00000u | (quint32(m_packetCount) << 16)
        | quint32(bytes.size() / 4);
    m_packetCount = (m_packetCount + 1) & 15;
    qToBigEndian(header, bytes.data());
    qToBigEndian(m_streamId, bytes.data() + 4);
    qToBigEndian(quint32(0x00001C2D), bytes.data() + 8);
    qToBigEndian(quint32(0x534C03E3), bytes.data() + 12);
    std::memcpy(bytes.data() + 28, payload.constData(), size_t(payload.size()));
    return bytes;
}

FlexCwControls::Timestamp FlexCwControls::timestamp(qint64 nowMs, qint64 scheduleAgeMs)
{
    // Flex uses a relative 16-bit millisecond clock. The first edge after idle
    // resynchronizes at zero; subsequent scheduled edges may backdate by 100 ms.
    qint64 elapsed = nowMs - m_clockStartMs;
    if (m_clockStartMs < 0 || m_lastSendMs < 0 || elapsed - m_lastSendMs > 3000) {
        m_clockStartMs = nowMs;
        m_lastSendMs = 0;
        return {0, 0, false};
    }
    qint64 backdate = 0;
    bool fallback = false;
    if (scheduleAgeMs >= 0) {
        backdate = std::clamp(scheduleAgeMs, qint64(0), qint64(100));
        elapsed -= backdate;
        if (elapsed <= m_lastSendMs) {
            elapsed = std::max(nowMs - m_clockStartMs, m_lastSendMs + 1);
            backdate = 0;
            fallback = true;
        }
    }
    m_lastSendMs = elapsed;
    return {static_cast<quint16>(elapsed & 0xFFFF), backdate, fallback};
}

QString FlexCwControls::decorate(const QString& baseCmd, const CwControlRequest& request)
{
    const auto now = std::chrono::steady_clock::now();
    const qint64 age = request.scheduledAt == std::chrono::steady_clock::time_point{} ? -1
        : std::max(qint64(0), qint64(std::chrono::duration_cast<std::chrono::milliseconds>(now - request.scheduledAt).count()));
    const Timestamp stamp = timestamp(std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count(), age);
    const qint64 index = m_index <= 0x7FFFFFFFu ? qint64(m_index) : qint64(m_index) - 0x100000000LL;
    ++m_index;
    const QString time = QStringLiteral("%1").arg(stamp.value, 4, 16, QLatin1Char('0')).toUpper();
    const QString handle = QStringLiteral("%1").arg(m_handle, 8, 16, QLatin1Char('0')).toUpper();
    const quint64 sourceNow = quint64(monotonicMs());
    const qint64 sourceAge = request.debugSourceMs == 0 ? -1
        : qint64(sourceNow - std::min(sourceNow, request.debugSourceMs));
    qCDebug(lcCw).noquote() << "CW netcw schedule trace=" << request.debugTraceId
        << "source=" << request.debugSource << "sinceSourceMs=" << sourceAge
        << "index=" << index << "time=0x" << time
        << "schedAgeMs=" << age << "backdateMs=" << stamp.backdate
        << "backdateFallback=" << stamp.fallback;
    return QStringLiteral("%1 time=0x%2 index=%3 client_handle=0x%4")
        .arg(baseCmd, time, QString::number(index), handle);
}

ReceiveDispatch FlexCwControls::request(const CwControlRequest& request,
                                       const TxCoordinator::Command& command)
{
    if (request.down != command.keying || command.operation.independent()
        || !command.permitsDispatch(monotonicMs()) || !m_txSink
        || (request.down && m_current && !m_current())) { return ReceiveDispatch::Unsupported; }
    // Closing admission must not suppress release. Its original operation's
    // cleanup fence still prevents an old key-up reaching a replacement session.
    const bool carrier = request.kind == CwControlRequest::Kind::Carrier;
    if (!carrier && request.kind != CwControlRequest::Kind::PttEnvelope) {
        return ReceiveDispatch::Unsupported;
    }
    const QString base = QStringLiteral("cw %1 %2")
        .arg(carrier ? QStringLiteral("key") : QStringLiteral("ptt")).arg(int(request.down));
    const auto tx = m_txSink;
    if (m_streamId == 0) {
        // Published FlexLib exposes CWKeyImmediate. Firmware support varies;
        // retain the compatibility attempt without claiming radio acceptance.
        const QString fallback = carrier ? QStringLiteral("cw key immediate %1").arg(int(request.down)) : base;
        tx(fallback, command);
        return ReceiveDispatch::Dispatched;
    }
    const QString encoded = decorate(base, request);
    // Each branch retains the aggregate until all queued closures are destroyed,
    // including cancelled timers/receivers. A TCP completion alone cannot end it.
    const TxCoordinator::Completion aggregate([completion = command.completion] { completion.finish(); });
    const QPointer<PanadapterStream> stream = m_stream;
    const SessionCommandGuard session = m_session;
    const SessionCommandGuard streamGuard = m_streamGuard;
    const qint64 scheduledMs = monotonicMs();
    for (int copy = 0; copy < 4; ++copy) {
        const QByteArray bytes = packet(encoded.toLatin1());
        const auto send = [stream, session, streamGuard, operation = command.operation,
                           down = request.down, bytes, aggregate, copy, scheduledMs, trace = request.debugTraceId] {
            if (!stream || !session.current() || !streamGuard.current()) { return; }
            QMetaObject::invokeMethod(stream, [stream, session, streamGuard, operation, down, bytes, aggregate, copy, scheduledMs, trace] {
                const TxCoordinator::Dispatch dispatch = operation.beginDispatch(monotonicMs(), down);
                if (stream && session.current() && streamGuard.current() && dispatch) {
                    qCDebug(lcCw) << "CW netcw udp-send trace=" << trace << "copy=" << copy
                        << "delayMs=" << copy * 5 << "actualDelayMs=" << monotonicMs() - scheduledMs
                        << "bytes=" << bytes.size();
                    stream->sendToRadio(bytes);
                }
            }, Qt::QueuedConnection);
        };
        if (copy == 0) { send(); }
        else { QTimer::singleShot(copy * 5, this, send); }
    }
    tx(encoded, {command.operation, command.keying, TxCoordinator::Completion([aggregate] {})});
    return ReceiveDispatch::Dispatched;
}
} // namespace AetherSDR
