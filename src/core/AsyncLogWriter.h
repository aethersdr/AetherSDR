#pragma once

#include <QTime>
#include <QString>
#include <QtCore/qlogging.h>

#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>

namespace AetherSDR {

// Apply log-style PII redaction (IPv4 → *.*.*.X, radio serial →
// ****-****-****-XXXX, Auth0 tokens, MAC addresses).  Defined in
// AsyncLogWriter.cpp; thread-safe (pure function over local copies of
// static const QRegularExpressions).  Used by AsyncLogWriter for every
// log line and by SupportBundle to scrub radio-info.json fields per
// the operator-PII redaction policy (GHSA-ccrg-j8cp-qhc4).
QString redactPii(const QString& msg);

class AsyncLogWriter {
public:
    struct Counters {
        quint64 queuedLines{0};
        quint64 writtenLines{0};
        quint64 droppedDebugInfoLines{0};
        quint64 droppedHighPriorityLines{0};
        quint64 maxQueueDepth{0};
        quint64 maxBatchSize{0};
        quint64 rotationCount{0};
    };

    // Invoked from the writer thread when the active file exceeds the
    // size cap. The callback must return the path of the new file to
    // open (a fresh timestamp under the same directory); returning an
    // empty string disables further rotation. The callback is also the
    // right place to update LogManager's active path and any symlinks.
    using RotationCallback = std::function<QString(const QString& currentPath)>;

    AsyncLogWriter();
    ~AsyncLogWriter();

    AsyncLogWriter(const AsyncLogWriter&) = delete;
    AsyncLogWriter& operator=(const AsyncLogWriter&) = delete;

    // Configure size-based rotation. Must be called before start().
    // maxFileBytes <= 0 disables rotation.
    void setRotationConfig(qint64 maxFileBytes, RotationCallback cb);

    bool start(const QString& path, bool mirrorToStderr);
    void shutdown();

    bool isRunning() const;

    void enqueue(QtMsgType type,
                 const QTime& timestamp,
                 const QString& category,
                 const QString& message);

    void flush();
    void clearLog();

    Counters counters() const;

private:
    struct LogMessage {
        QtMsgType type{QtDebugMsg};
        QTime timestamp;
        QString category;
        QString message;
    };

    enum class ItemKind {
        Log,
        Flush,
        Clear,
        Stop,
    };

    struct SyncPoint {
        std::mutex mutex;
        std::condition_variable cv;
        bool done{false};
    };

    struct QueueItem {
        ItemKind kind{ItemKind::Log};
        LogMessage log;
        std::shared_ptr<SyncPoint> sync;
    };

    void run(std::promise<bool> opened);
    bool enqueueControlAndWait(ItemKind kind);
    void markDone(const std::shared_ptr<SyncPoint>& sync);

    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<QueueItem> m_queue;
    std::thread m_worker;

    QString m_filePath;
    bool m_mirrorToStderr{false};
    bool m_started{false};
    bool m_accepting{false};
    bool m_stopping{false};

    qint64 m_maxFileBytes{0};
    RotationCallback m_rotationCallback;

    Counters m_counters;
    quint64 m_pendingDroppedDebugInfo{0};
    quint64 m_pendingDroppedHighPriority{0};
};

} // namespace AetherSDR
