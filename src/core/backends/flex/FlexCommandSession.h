#pragma once

#include "core/backends/SessionCommandGuard.h"
#include "core/TxCoordinator.h"

#include <QDeadlineTimer>
#include <QHash>
#include <QObject>
#include <QString>
#include <QTimer>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>

namespace AetherSDR {
class RadioConnection;

// One owner-thread reply ledger per wire session. Only Write crosses to the
// transport thread; its fences contain no model or other owner-thread state.
class FlexCommandSession final : public QObject {
    friend class FlexCommandSessionTestAccess;
    friend class FlexCommandTestAccess;
public:
    enum class WriteResult { NotWritten, Written, Unknown };
    enum class ReplyKind { NotDispatched, RadioReply, Unknown };
    struct Reply {
        ReplyKind kind;
        int code;
        QString body;
    };
    using Completion = std::function<void(const Reply&)>;
    struct Options {
        SessionCommandGuard guard;
        bool guarded{false};
        std::function<bool()> permitsWrite;
        TxCoordinator::Operation operation;
        bool tx{false};
        bool keying{false};
        bool cleanup{false};
        qint64 timeoutMs{30000};
        std::function<void(WriteResult)> written;
        std::function<void()> consumed;
    };
    struct Write {
        quint64 epoch;
        quint32 sequence;
        QString command;
        SessionCommandGuard session;
        SessionCommandGuard request;
        Options options;
        enum class State { Queued, Writing, NotWritten, Written, Unknown };
        std::shared_ptr<std::atomic<State>> state;
        WriteResult run(const std::function<WriteResult()>& writer) const;
    };
    using Writer = std::function<bool(const Write&)>;

    explicit FlexCommandSession(QObject* parent = nullptr);
    ~FlexCommandSession() override;
    void setWriter(Writer writer);
    void bindLan(RadioConnection* connection);
    void begin(quint64 epoch);
    void invalidate() { ++m_revision; m_active = false; m_session.retireSession(); }
    // Retire ordinary writes while reserving the ledger for teardown replies.
    void beginCleanup() { m_session.retireSession(); }
    void retire(const QString& reason);
    quint32 submit(const QString& command, const Options& options, Completion completion = {});
    void answer(quint64 epoch, quint32 sequence, int code, const QString& body);
    void writeFinished(quint64 epoch, quint32 sequence, WriteResult result);
    void forget(quint64 epoch, quint32 sequence);
    quint32 allocateSequence();
    quint64 epoch() const { return m_epoch; }
    bool active() const { return m_active; }
    qsizetype pendingCount() const { return m_pending.size(); }
    void expire();

    static constexpr qsizetype kMaximumPending = 4096;
    static constexpr qsizetype kCleanupReserve = 64;

private:
    struct Pending {
        Write write;
        Completion completion;
        QDeadlineTimer deadline;
        bool writeReported{false};
        std::optional<Reply> received;
    };
    void complete(quint32 sequence, const Reply& reply);
    static Reply abandoned(const Pending& pending, const QString& reason);
    Writer m_writer;
    QMetaObject::Connection m_replyConnection;
    QHash<quint32, Pending> m_pending;
    QTimer m_expiry;
    SessionCommandGuard m_session;
    SessionCommandGuard m_cleanupSession;
    quint64 m_epoch{0};
    quint64 m_revision{0};
    std::atomic<quint64> m_sequence{1};
    bool m_active{false};
};

} // namespace AetherSDR
