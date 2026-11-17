#include "FlexCommandSession.h"
#include "RadioConnection.h"

#include <QPointer>
#include <QThread>
#include <limits>
#include <utility>

namespace AetherSDR {

FlexCommandSession::WriteResult FlexCommandSession::Write::run(
    const std::function<WriteResult()>& writer) const
{
    const auto permitted = [this] {
        return session.current() && request.current()
            && (!options.guarded || options.guard.current())
            && (!options.permitsWrite || options.permitsWrite());
    };
    State expected = State::Queued;
    if (!state->compare_exchange_strong(expected, State::Writing)) { return WriteResult::NotWritten; }
    WriteResult result = WriteResult::NotWritten;
    if (permitted()) {
        if (options.tx) {
            const TxCoordinator::Dispatch dispatch = options.operation.beginDispatch(
                TxCoordinator::monotonicMs(), options.keying);
            if (dispatch && permitted()) { result = writer(); }
        } else {
            result = writer();
        }
    }
    state->store(result == WriteResult::Written ? State::Written
        : result == WriteResult::Unknown ? State::Unknown : State::NotWritten);
    return result;
}

FlexCommandSession::FlexCommandSession(QObject* parent) : QObject(parent)
{
    m_expiry.setInterval(100);
    connect(&m_expiry, &QTimer::timeout, this, &FlexCommandSession::expire);
}

FlexCommandSession::~FlexCommandSession()
{
    m_session.retireSession();
    m_cleanupSession.retireSession();
    for (const Pending& pending : std::as_const(m_pending)) { pending.write.request.cancel(); }
}

void FlexCommandSession::bindLan(RadioConnection* connection)
{
    QObject::disconnect(m_replyConnection);
    setWriter([this, connection](const FlexCommandSession::Write& write) {
        const QPointer<FlexCommandSession> commands(this);
        const quint64 generation = connection->commandSessionGeneration();
        return QMetaObject::invokeMethod(connection, [connection, commands, write, generation] {
            const auto result = write.run([&] {
                if (connection->commandSessionGeneration() != generation
                    || connection->lineEpoch() != write.epoch) {
                    return FlexCommandSession::WriteResult::NotWritten;
                }
                return connection->dispatchCommand(write.sequence, write.command);
            });
            if (commands) {
                QMetaObject::invokeMethod(commands, [commands, write, result] {
                    if (commands) { commands->writeFinished(write.epoch, write.sequence, result); }
                }, Qt::QueuedConnection);
            }
        }, Qt::QueuedConnection);
    });
    m_replyConnection = connect(connection, &RadioConnection::commandResponse, this,
        [this, connection](quint32 sequence, int code, const QString& body) {
            const quint64 epoch = connection->lineEpoch();
            QMetaObject::invokeMethod(this, [this, epoch, sequence, code, body] {
                answer(epoch, sequence, code, body);
            }, Qt::QueuedConnection);
        }, Qt::DirectConnection);
}

void FlexCommandSession::setWriter(Writer writer) { m_writer = std::move(writer); }

void FlexCommandSession::begin(quint64 epoch)
{
    Q_ASSERT(QThread::currentThread() == thread());
    const QPointer<FlexCommandSession> alive(this);
    const quint64 revision = m_revision + 1;
    retire(QStringLiteral("Original command session ended."));
    // A completion may replace or cancel the session while the old one drains.
    // Even an inactive replacement supersedes this older begin request.
    if (!alive || m_revision != revision || epoch == 0) { return; }
    m_epoch = epoch;
    m_session = SessionCommandGuard::create();
    m_cleanupSession = SessionCommandGuard::create();
    m_sequence.store(1);
    m_active = true;
}

FlexCommandSession::Reply FlexCommandSession::abandoned(const Pending& pending, const QString& reason)
{
    if (pending.received) { return *pending.received; }
    const Write::State state = pending.write.state->load();
    const bool unwritten = state == Write::State::Queued || state == Write::State::NotWritten;
    return {unwritten ? ReplyKind::NotDispatched : ReplyKind::Unknown, -1,
        unwritten ? reason : QStringLiteral("No radio response; execution is unknown.")};
}

void FlexCommandSession::retire(const QString& reason)
{
    Q_ASSERT(QThread::currentThread() == thread());
    ++m_revision;
    m_active = false;
    m_session.retireSession();
    m_cleanupSession.retireSession();
    m_expiry.stop();
    QHash<quint32, Pending> pending = std::move(m_pending);
    m_pending.clear();
    for (Pending& item : pending) { item.write.request.cancel(); }
    const QPointer<FlexCommandSession> alive(this);
    for (const Pending& item : std::as_const(pending)) {
        if (!alive) { return; }
        if (!item.writeReported && item.write.options.consumed) { item.write.options.consumed(); }
        if (!alive) { return; }
        if (item.completion) { item.completion(abandoned(item, reason)); }
    }
}

quint32 FlexCommandSession::allocateSequence()
{
    // Exhaustion refuses admission until a new wire session; never reuse an
    // identifier that a delayed reply from this session could still carry.
    quint64 sequence = m_sequence.load();
    while (sequence <= std::numeric_limits<quint32>::max()) {
        if (m_sequence.compare_exchange_weak(sequence, sequence + 1)) { return static_cast<quint32>(sequence); }
    }
    return 0;
}

quint32 FlexCommandSession::submit(const QString& command, const Options& options, Completion completion)
{
    Q_ASSERT(QThread::currentThread() == thread());
    const qsizetype capacity = kMaximumPending + (options.cleanup ? kCleanupReserve : 0);
    if (!m_active || !m_writer || command.isEmpty() || options.timeoutMs <= 0
        || (!options.cleanup && !m_session.current())
        || m_pending.size() >= capacity || (options.guarded && !options.guard.current())) { return 0; }
    const quint32 sequence = allocateSequence();
    if (!sequence) { return 0; }
    const SessionCommandGuard session = options.cleanup ? m_cleanupSession : m_session;
    Write write{m_epoch, sequence, command, session, session.request(options.timeoutMs), options,
        std::make_shared<std::atomic<Write::State>>(Write::State::Queued)};
    m_pending.insert(sequence, Pending{write, std::move(completion), QDeadlineTimer(options.timeoutMs), false, {}});
    if (!m_expiry.isActive()) { m_expiry.start(); }
    const QPointer<FlexCommandSession> alive(this);
    const Writer writer = m_writer;
    const bool queued = writer(write);
    if (!alive) { return sequence; }
    if (!queued) {
        writeFinished(write.epoch, sequence, WriteResult::NotWritten);
        return 0;
    }
    return sequence;
}

void FlexCommandSession::complete(quint32 sequence, const Reply& reply)
{
    const auto it = m_pending.find(sequence);
    if (it == m_pending.end()) { return; }
    Pending pending = std::move(it.value());
    m_pending.erase(it);
    pending.write.request.cancel();
    if (m_pending.isEmpty()) { m_expiry.stop(); }
    const QPointer<FlexCommandSession> alive(this);
    if (!pending.writeReported && pending.write.options.consumed) { pending.write.options.consumed(); }
    if (alive && pending.completion) { pending.completion(reply); }
}

void FlexCommandSession::answer(quint64 epoch, quint32 sequence, int code, const QString& body)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (!m_active || epoch != m_epoch) { return; }
    const auto it = m_pending.find(sequence);
    if (it == m_pending.end() || it->received) { return; }
    const Reply reply{ReplyKind::RadioReply, code, body};
    if (!it->writeReported) {
        it->received = reply;
        return;
    }
    complete(sequence, reply);
}

void FlexCommandSession::writeFinished(quint64 epoch, quint32 sequence, WriteResult result)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (!m_active || epoch != m_epoch) { return; }
    auto it = m_pending.find(sequence);
    if (it == m_pending.end() || it->writeReported) { return; }
    it->writeReported = true;
    const auto written = it->write.options.written;
    const auto consumed = it->write.options.consumed;
    const bool hasReply = bool(it->completion);
    const QPointer<FlexCommandSession> alive(this);
    if (written) { written(result); }
    if (consumed) { consumed(); }
    if (!alive || !m_active || epoch != m_epoch) { return; }
    it = m_pending.find(sequence);
    if (it == m_pending.end()) { return; }
    if (it->received) {
        const Reply reply = *it->received;
        complete(sequence, reply);
        return;
    }
    if (result == WriteResult::NotWritten) {
        complete(sequence, {ReplyKind::NotDispatched, -1, QStringLiteral("Command not written.")});
    } else if (result == WriteResult::Unknown) {
        complete(sequence, {ReplyKind::Unknown, -1, QStringLiteral("Command write outcome is unknown.")});
    } else if (!hasReply) {
        complete(sequence, {ReplyKind::RadioReply, 0, {}});
    }
}

void FlexCommandSession::forget(quint64 epoch, quint32 sequence)
{
    if (epoch != m_epoch) { return; }
    const auto it = m_pending.find(sequence);
    if (it == m_pending.end()) { return; }
    it->write.request.cancel();
    m_pending.erase(it);
    if (m_pending.isEmpty()) { m_expiry.stop(); }
}

void FlexCommandSession::expire()
{
    const QPointer<FlexCommandSession> alive(this);
    const quint64 epoch = m_epoch;
    const QList<quint32> sequences = m_pending.keys();
    for (quint32 sequence : sequences) {
        if (!alive || epoch != m_epoch) { return; }
        const auto it = m_pending.constFind(sequence);
        if (it != m_pending.cend() && (it->deadline.hasExpired()
            || (it->write.options.guarded && !it->write.options.guard.current()))) {
            it->write.request.cancel();
            complete(sequence, abandoned(it.value(), QStringLiteral("Command expired before dispatch.")));
        }
    }
}

} // namespace AetherSDR
