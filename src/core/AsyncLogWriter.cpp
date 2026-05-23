#include "AsyncLogWriter.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <utility>

namespace AetherSDR {

namespace {

constexpr qsizetype kMaxQueueEntries = 8192;
constexpr qsizetype kHighPriorityReserveEntries = 1024;
constexpr qsizetype kHardMaxQueueEntries = kMaxQueueEntries + kHighPriorityReserveEntries;
constexpr qsizetype kMaxBatchEntries = 256;
constexpr int kFlushIntervalMs = 250;

bool isDebugOrInfo(QtMsgType type)
{
    return type == QtDebugMsg || type == QtInfoMsg;
}

QString labelForType(QtMsgType type)
{
    switch (type) {
    case QtDebugMsg:
        return QStringLiteral("DBG");
    case QtWarningMsg:
        return QStringLiteral("WRN");
    case QtCriticalMsg:
        return QStringLiteral("CRT");
    case QtFatalMsg:
        return QStringLiteral("FTL");
    case QtInfoMsg:
        return QStringLiteral("INF");
    }
    return QStringLiteral("???");
}

} // namespace (anonymous)

// Public so SupportBundle and other callers can scrub PII the same way
// log lines are scrubbed.  Declared in AsyncLogWriter.h.
QString redactPii(const QString& msg)
{
    QString out = msg;

    // IPv4 addresses: 192.168.50.121 -> *.*.*. 121 (keep last octet).
    // The word boundary skips v/V-prefixed version strings; the ver=
    // lookbehind and trailing digit check skip firmware/software versions
    // with build numbers such as software_ver=4.2.18.41174.
    static const QRegularExpression* ipRe = new QRegularExpression(
        R"((?<!ver=)\b(?:25[0-5]|2[0-4]\d|1\d\d|[1-9]?\d)\.(?:25[0-5]|2[0-4]\d|1\d\d|[1-9]?\d)\.(?:25[0-5]|2[0-4]\d|1\d\d|[1-9]?\d)\.((?:25[0-5]|2[0-4]\d|1\d\d|[1-9]?\d))(?!\d))");
    out.replace(*ipRe, QStringLiteral("*.*.*. \\1"));

    // Radio serial: 4424-1213-8600-7836 -> ****-****-****-7836
    static const QRegularExpression* serialRe = new QRegularExpression(
        R"(\d{4}-\d{4}-\d{4}-(\d{4}))");
    out.replace(*serialRe, QStringLiteral("****-****-****-\\1"));

    // Auth tokens. Case-insensitive; \b prevents app-specific identifiers
    // that end in "token" (e.g. keytoken=) from being scrubbed. The separator
    // and any "Bearer "/"Basic "/"Digest " scheme are preserved so the
    // scrubbed line still reads in context; a 4-char prefix is kept for
    // cross-line correlation without leaking JWT header entropy. (#2954)
    //
    // Split into two passes so "auth id_token=…" doesn't get eaten by the
    // "auth"-keyword match and leave the real token unscrubbed: pass 1
    // requires a "[:=]" separator (catches keyword=value and Authorization:),
    // pass 2 handles the standalone "bearer <token>" scheme.
    static const QRegularExpression* tokenRe = new QRegularExpression(
        R"(\b(id_token|access_token|refresh_token|token|authorization|auth)(\s*[:=]\s*)(?:(bearer|basic|digest)(\s+))?([A-Za-z0-9_\-\.]{4})[A-Za-z0-9_\-\.]+)",
        QRegularExpression::CaseInsensitiveOption);
    out.replace(*tokenRe, QStringLiteral("\\1\\2\\3\\4\\5***REDACTED***"));
    static const QRegularExpression* bearerRe = new QRegularExpression(
        R"(\b(bearer)(\s+)([A-Za-z0-9_\-\.]{4})[A-Za-z0-9_\-\.]+)",
        QRegularExpression::CaseInsensitiveOption);
    out.replace(*bearerRe, QStringLiteral("\\1\\2\\3***REDACTED***"));

    // MAC addresses: 00-1C-2D-05-37-2A -> **-**-**-**-**-2A
    //                00:1C:2D:05:37:2A -> **:**:**:**:**:2A
    static const QRegularExpression* macRe = new QRegularExpression(
        R"(([0-9A-Fa-f]{2})([:-])([0-9A-Fa-f]{2})\2([0-9A-Fa-f]{2})\2([0-9A-Fa-f]{2})\2([0-9A-Fa-f]{2})\2([0-9A-Fa-f]{2}))");
    out.replace(*macRe, QStringLiteral("**\\2**\\2**\\2**\\2**\\2\\7"));

    return out;
}

namespace {

QByteArray formatLine(QtMsgType type,
                      const QTime& timestamp,
                      const QString& category,
                      const QString& message)
{
    const QString safeMsg = redactPii(message);
    return QString("[%1] %2 %3: %4\n")
        .arg(timestamp.toString(QStringLiteral("HH:mm:ss.zzz")),
             labelForType(type),
             category,
             safeMsg)
        .toUtf8();
}

} // namespace

AsyncLogWriter::AsyncLogWriter() = default;

AsyncLogWriter::~AsyncLogWriter()
{
    shutdown();
}

void AsyncLogWriter::setRotationConfig(qint64 maxFileBytes, RotationCallback cb)
{
    std::lock_guard lock(m_mutex);
    m_maxFileBytes = maxFileBytes;
    m_rotationCallback = std::move(cb);
}

bool AsyncLogWriter::start(const QString& path, bool mirrorToStderr)
{
    shutdown();

    std::promise<bool> opened;
    std::future<bool> openedFuture = opened.get_future();

    {
        std::lock_guard lock(m_mutex);
        m_filePath = path;
        m_mirrorToStderr = mirrorToStderr;
        m_started = true;
        m_accepting = false;
        m_stopping = false;
        m_queue.clear();
        m_counters = Counters{};
        m_pendingDroppedDebugInfo = 0;
        m_pendingDroppedHighPriority = 0;
    }

    m_worker = std::thread(&AsyncLogWriter::run, this, std::move(opened));
    const bool ok = openedFuture.get();
    if (!ok) {
        if (m_worker.joinable())
            m_worker.join();
        std::lock_guard lock(m_mutex);
        m_started = false;
        m_accepting = false;
        m_stopping = false;
        return false;
    }

    {
        std::lock_guard lock(m_mutex);
        m_accepting = true;
    }
    m_cv.notify_one();
    return true;
}

void AsyncLogWriter::shutdown()
{
    std::shared_ptr<SyncPoint> sync;
    {
        std::lock_guard lock(m_mutex);
        if (!m_started)
            return;

        m_accepting = false;
        m_stopping = true;
        sync = std::make_shared<SyncPoint>();
        QueueItem item;
        item.kind = ItemKind::Stop;
        item.sync = sync;
        m_queue.push_back(std::move(item));
    }
    m_cv.notify_one();

    {
        std::unique_lock lock(sync->mutex);
        sync->cv.wait(lock, [&] { return sync->done; });
    }

    if (m_worker.joinable())
        m_worker.join();

    std::lock_guard lock(m_mutex);
    m_started = false;
    m_stopping = false;
    m_queue.clear();
}

bool AsyncLogWriter::isRunning() const
{
    std::lock_guard lock(m_mutex);
    return m_started && !m_stopping;
}

void AsyncLogWriter::enqueue(QtMsgType type,
                             const QTime& timestamp,
                             const QString& category,
                             const QString& message)
{
    {
        std::lock_guard lock(m_mutex);
        if (!m_started || !m_accepting)
            return;

        if (m_queue.size() >= kMaxQueueEntries) {
            if (isDebugOrInfo(type)) {
                ++m_counters.droppedDebugInfoLines;
                ++m_pendingDroppedDebugInfo;
                m_cv.notify_one();
                return;
            }

            auto dropIt = std::find_if(m_queue.begin(), m_queue.end(),
                [](const QueueItem& item) {
                    return item.kind == ItemKind::Log && isDebugOrInfo(item.log.type);
                });
            if (dropIt != m_queue.end()) {
                m_queue.erase(dropIt);
                ++m_counters.droppedDebugInfoLines;
                ++m_pendingDroppedDebugInfo;
            } else if (m_queue.size() >= kHardMaxQueueEntries) {
                ++m_counters.droppedHighPriorityLines;
                ++m_pendingDroppedHighPriority;
                m_cv.notify_one();
                return;
            }
        }

        QueueItem item;
        item.kind = ItemKind::Log;
        item.log.type = type;
        item.log.timestamp = timestamp;
        item.log.category = category;
        item.log.message = message;
        m_queue.push_back(std::move(item));

        ++m_counters.queuedLines;
        m_counters.maxQueueDepth = std::max<quint64>(m_counters.maxQueueDepth, m_queue.size());
    }
    m_cv.notify_one();
}

void AsyncLogWriter::flush()
{
    enqueueControlAndWait(ItemKind::Flush);
}

void AsyncLogWriter::clearLog()
{
    enqueueControlAndWait(ItemKind::Clear);
}

AsyncLogWriter::Counters AsyncLogWriter::counters() const
{
    std::lock_guard lock(m_mutex);
    return m_counters;
}

bool AsyncLogWriter::enqueueControlAndWait(ItemKind kind)
{
    std::shared_ptr<SyncPoint> sync;
    {
        std::lock_guard lock(m_mutex);
        if (!m_started || m_stopping)
            return false;

        sync = std::make_shared<SyncPoint>();
        QueueItem item;
        item.kind = kind;
        item.sync = sync;
        m_queue.push_back(std::move(item));
        m_counters.maxQueueDepth = std::max<quint64>(m_counters.maxQueueDepth, m_queue.size());
    }
    m_cv.notify_one();

    std::unique_lock lock(sync->mutex);
    sync->cv.wait(lock, [&] { return sync->done; });
    return true;
}

void AsyncLogWriter::markDone(const std::shared_ptr<SyncPoint>& sync)
{
    if (!sync)
        return;

    {
        std::lock_guard lock(sync->mutex);
        sync->done = true;
    }
    sync->cv.notify_all();
}

void AsyncLogWriter::run(std::promise<bool> opened)
{
    QString path;
    bool mirrorToStderr = false;
    qint64 maxFileBytes = 0;
    RotationCallback rotationCb;
    {
        std::lock_guard lock(m_mutex);
        path = m_filePath;
        mirrorToStderr = m_mirrorToStderr;
        maxFileBytes = m_maxFileBytes;
        rotationCb = m_rotationCallback;
    }

    QDir().mkpath(QFileInfo(path).absolutePath());

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        opened.set_value(false);
        return;
    }
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    opened.set_value(true);

    QByteArray buffer;
    qsizetype bufferedLineCount = 0;
    bool hasUnflushedWrites = false;
    QElapsedTimer flushTimer;
    flushTimer.start();

    auto recordWritten = [this](qsizetype count) {
        if (count <= 0)
            return;
        std::lock_guard lock(m_mutex);
        m_counters.writtenLines += static_cast<quint64>(count);
    };

    auto flushBuffer = [&]() {
        if (buffer.isEmpty())
            return;
        if (file.write(buffer) >= 0) {
            recordWritten(bufferedLineCount);
            hasUnflushedWrites = true;
        }
        buffer.clear();
        bufferedLineCount = 0;
    };

    auto maybeRotate = [&]() {
        if (maxFileBytes <= 0 || !rotationCb)
            return;
        if (file.size() < maxFileBytes)
            return;

        const QString oldPath = file.fileName();
        file.close();

        const QString newPath = rotationCb(oldPath);
        if (newPath.isEmpty() || newPath == oldPath) {
            file.setFileName(oldPath);
            file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);
            maxFileBytes = 0;
            return;
        }

        file.setFileName(newPath);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            file.setFileName(oldPath);
            file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);
            maxFileBytes = 0;
            return;
        }
        file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);

        std::lock_guard lock(m_mutex);
        m_filePath = newPath;
        ++m_counters.rotationCount;
    };

    auto makeDropSummary = [](QtMsgType type, const QString& message) {
        QueueItem item;
        item.kind = ItemKind::Log;
        item.log.type = type;
        item.log.timestamp = QTime::currentTime();
        item.log.category = QStringLiteral("aether.logging");
        item.log.message = message;
        return item;
    };

    bool stop = false;
    while (!stop) {
        std::deque<QueueItem> batch;
        bool timedOut = false;
        {
            std::unique_lock lock(m_mutex);
            timedOut = !m_cv.wait_for(lock, std::chrono::milliseconds(kFlushIntervalMs), [&] {
                return !m_queue.empty()
                    || m_pendingDroppedDebugInfo > 0
                    || m_pendingDroppedHighPriority > 0;
            });

            while (!m_queue.empty() && batch.size() < kMaxBatchEntries) {
                batch.push_back(std::move(m_queue.front()));
                m_queue.pop_front();
            }

            const quint64 droppedDebugInfo = std::exchange(m_pendingDroppedDebugInfo, 0);
            const quint64 droppedHighPriority = std::exchange(m_pendingDroppedHighPriority, 0);
            if (droppedDebugInfo > 0) {
                batch.push_back(makeDropSummary(
                    QtWarningMsg,
                    QStringLiteral("Logging dropped debug/info lines count=%1 due_to=queue_full")
                        .arg(droppedDebugInfo)));
            }
            if (droppedHighPriority > 0) {
                batch.push_back(makeDropSummary(
                    QtCriticalMsg,
                    QStringLiteral("Logging dropped warning/critical/fatal lines count=%1 due_to=queue_full")
                        .arg(droppedHighPriority)));
            }

            if (!batch.empty()) {
                m_counters.maxBatchSize = std::max<quint64>(m_counters.maxBatchSize, batch.size());
            }
        }

        bool flushAfterBatch = false;
        for (QueueItem& item : batch) {
            switch (item.kind) {
            case ItemKind::Log: {
                const QByteArray line = formatLine(item.log.type,
                                                   item.log.timestamp,
                                                   item.log.category,
                                                   item.log.message);
                buffer.append(line);
                ++bufferedLineCount;
                if (mirrorToStderr) {
                    fwrite(line.constData(), 1, static_cast<size_t>(line.size()), stderr);
                }
                if (!isDebugOrInfo(item.log.type))
                    flushAfterBatch = true;
                break;
            }
            case ItemKind::Flush:
                flushBuffer();
                if (hasUnflushedWrites) {
                    file.flush();
                    if (mirrorToStderr)
                        fflush(stderr);
                    hasUnflushedWrites = false;
                    flushTimer.restart();
                }
                markDone(item.sync);
                break;
            case ItemKind::Clear:
                flushBuffer();
                if (hasUnflushedWrites) {
                    file.flush();
                    hasUnflushedWrites = false;
                    flushTimer.restart();
                }
                file.resize(0);
                file.seek(0);
                markDone(item.sync);
                break;
            case ItemKind::Stop:
                flushBuffer();
                if (hasUnflushedWrites) {
                    file.flush();
                    if (mirrorToStderr)
                        fflush(stderr);
                    hasUnflushedWrites = false;
                }
                markDone(item.sync);
                stop = true;
                break;
            }
        }

        bool batchDrained = false;
        if (flushAfterBatch) {
            flushBuffer();
            if (hasUnflushedWrites) {
                file.flush();
                if (mirrorToStderr)
                    fflush(stderr);
                hasUnflushedWrites = false;
                flushTimer.restart();
            }
            batchDrained = true;
        } else if (timedOut || flushTimer.elapsed() >= kFlushIntervalMs) {
            flushBuffer();
            if (hasUnflushedWrites) {
                file.flush();
                if (mirrorToStderr)
                    fflush(stderr);
                hasUnflushedWrites = false;
                flushTimer.restart();
            }
            batchDrained = true;
        } else if (bufferedLineCount >= kMaxBatchEntries) {
            flushBuffer();
        }

        // Check size only between batches, after the buffer is drained and
        // the OS has the bytes on disk — never mid-batch, so partial-batch
        // state can't straddle two files. (#2498)
        if (batchDrained && !stop)
            maybeRotate();
    }

    flushBuffer();
    if (hasUnflushedWrites)
        file.flush();
    file.close();
}

} // namespace AetherSDR
