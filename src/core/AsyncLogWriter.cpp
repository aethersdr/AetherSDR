#include "AsyncLogWriter.h"
#include "LogRedactionPolicy.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <utility>

// Naming this thread is done HERE rather than through AetherSDR::ThreadName
// (src/core/ThreadName.h), which is the canonical helper and the one every
// other caller uses. The reason is build cost, not preference: this file is
// compiled into 51 test targets and ThreadName.cpp into 6, so routing through
// it would mean adding a source to 45 unrelated targets. Keep the two in step
// if the platform calls ever change.
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#if defined(__MINGW32__)
// This mingw-w64 header snapshot doesn't declare SetThreadDescription even
// though kernel32.dll exports it (Windows 10 1607+, MSVC's SDK already has
// it). Widening _WIN32_WINNT doesn't help — the prototype is absent outright.
extern "C" __declspec(dllimport) HRESULT WINAPI
    SetThreadDescription(HANDLE hThread, PCWSTR lpThreadDescription);
#endif
#elif defined(__APPLE__)
#include <pthread.h>
#elif defined(__linux__)
#include <sys/prctl.h>
#endif

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
namespace {

// The value grammar every keyword rule shares. Covers bare tokens, single- and
// double-quoted values, and the backslash-escaped spelling QDebug produces when
// it formats a QByteArray or QString payload (\"value\"). Keeping this in ONE
// place is the point of the table: the old hand-written rules disagreed about
// which of these forms they accepted.
constexpr const char* kSeparator = R"((\\?["']?\s*[:=]\s*))";
constexpr const char* kValue =
    R"((?:\\"[^"\\]*\\"|"[^"]*"|'[^']*'|[^\s#|,;&}\]]+))";

// An Authorization value may lead with a scheme word that is NOT the secret.
// Consuming it as the value is worse than not matching at all: it redacts
// "Basic" and leaves the credential standing.
constexpr const char* kAuthScheme = R"((?:(bearer|basic|digest)(\s+))?)";

// Replace every match's value group, keeping `keepPrefix` leading characters
// only when the value is strictly longer than that. A prefix as long as the
// value is not a redaction, and that is exactly how a short value used to
// survive: the old rule required four characters plus at least one more, so
// anything four characters or shorter never matched at all.
QString redactField(QString in, const QString& keyword, int keepPrefix)
{
    const QRegularExpression re(
        QStringLiteral(R"(\b(%1)%2%3(%4))").arg(keyword, QLatin1String(kSeparator),
                                                QLatin1String(kAuthScheme),
                                                QLatin1String(kValue)),
        QRegularExpression::CaseInsensitiveOption);
    QString out;
    qsizetype last = 0;
    auto it = re.globalMatch(in);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        out += in.mid(last, m.capturedStart() - last);
        // Strip the quoting/escaping so the prefix decision is made against
        // the value itself, not against its punctuation.
        QString value = m.captured(5);
        QString open;
        if (value.startsWith(QLatin1String("\\\""))) { open = QStringLiteral("\\\""); }
        else if (value.startsWith('"') || value.startsWith('\'')) { open = value.left(1); }
        if (!open.isEmpty() && value.size() >= 2 * open.size()) {
            value = value.mid(open.size(), value.size() - 2 * open.size());
        }
        const QString prefix = value.size() > keepPrefix ? value.left(keepPrefix) : QString();
        out += m.captured(1) + m.captured(2) + m.captured(3) + m.captured(4)
             + open + prefix + QStringLiteral("***REDACTED***") + open;
        last = m.capturedEnd();
    }
    out += in.mid(last);
    return out;
}

}  // namespace

QString redactPii(const QString& msg)
{
    QString out = msg;

    // ORDER MATTERS in this first block. IPv6 runs before IPv4 so an
    // IPv4-mapped address (::ffff:192.0.2.7) is scrubbed as one address rather
    // than having its tail rewritten in place and its v6 prefix left standing.
    // Home paths run before anything that could match inside a user name.

    // Home directory prefix -> ~. Both slash styles, and the literal
    // /home/<user>, /Users/<user> and C:\Users\<user> roots, because the
    // running process's QDir::homePath() is not always the path's own root:
    // a Flatpak sandbox, an elevated run, or a log copied off another machine
    // all produce a home path this process never had. User names may contain
    // spaces, so the segment runs to the next separator, not the next space.
    const QString home = QDir::homePath();
    if (!home.isEmpty()) {
        QString nativeHome = home;
        nativeHome.replace('/', '\\');
        for (const QString& form : {home, nativeHome}) {
            if (form.size() > 3)
                out.replace(form, QStringLiteral("~"));
        }
    }
    static const QRegularExpression* homeRootRe = new QRegularExpression(
        R"((?:/home/|/Users/|[A-Za-z]:\\{1,2}Users\\{1,2})([^/\\:*?"<>|\r\n]+))",
        QRegularExpression::CaseInsensitiveOption);
    out.replace(*homeRootRe, QStringLiteral("~"));

    // MAC addresses: 00-1C-2D-05-37-2A -> **-**-**-**-**-2A
    //                00:1C:2D:05:37:2A -> **:**:**:**:**:2A
    static const QRegularExpression* macRe = new QRegularExpression(
        R"(([0-9A-Fa-f]{2})([:-])([0-9A-Fa-f]{2})\2([0-9A-Fa-f]{2})\2([0-9A-Fa-f]{2})\2([0-9A-Fa-f]{2})\2([0-9A-Fa-f]{2}))");
    out.replace(*macRe, QStringLiteral("**\\2**\\2**\\2**\\2**\\2\\7"));

    // IPv6 literals. Two rules, because the only shapes that are unambiguously
    // an address are "contains ::" and "exactly eight groups". A looser
    // "two or more hex groups" pattern also matches a MAC address and an
    // elapsed-time value like 12:34:56, both of which are diagnostic and both
    // of which it silently destroyed. Covers compressed, bracketed-with-port,
    // IPv4-mapped and %scope forms; the last group is not kept because a v6
    // interface identifier is commonly derived from the MAC.
    static const QRegularExpression* ipv6FullRe = new QRegularExpression(
        R"(\b(?:[0-9A-Fa-f]{1,4}:){7}[0-9A-Fa-f]{1,4}\b)");
    out.replace(*ipv6FullRe, QStringLiteral("[v6-redacted]"));
    static const QRegularExpression* ipv6CompressedRe = new QRegularExpression(
        R"(\[?(?:[0-9A-Fa-f]{1,4}:(?!:))*[0-9A-Fa-f]{0,4}::(?:[0-9A-Fa-f]{1,4}:)*(?:\d{1,3}(?:\.\d{1,3}){3}|[0-9A-Fa-f]{1,4})?(?:%[0-9A-Za-z]+)?\]?)");
    out.replace(*ipv6CompressedRe, QStringLiteral("[v6-redacted]"));

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

    // Bare email addresses, wherever they appear in prose. The keyword-shaped
    // spellings (email=, mail_address:) come from the table below; this covers
    // SmartLinkClient's call-site truncation, which the redactor now owns.
    static const QRegularExpression* emailRe = new QRegularExpression(
        R"([A-Za-z0-9._%+\-]+@[A-Za-z0-9.\-]+\.[A-Za-z]{2,})");
    out.replace(*emailRe, QStringLiteral("***REDACTED***"));

    // Every keyword rule, generated from the one table (#5480).
    for (const auto& f : LogRedactionPolicy::kOpaqueValueFields)
        out = redactField(std::move(out), QLatin1String(f.keyword), f.keepPrefixChars);
    for (const auto& f : LogRedactionPolicy::kSensitiveValueFields)
        out = redactField(std::move(out), QLatin1String(f.keyword), f.keepPrefixChars);

    // The standalone "bearer <token>" scheme, which carries no separator and so
    // cannot come from the table.
    static const QRegularExpression* bearerRe = new QRegularExpression(
        R"(\b(bearer|basic|digest)(\s+)([A-Za-z0-9_\-\.]{4})[A-Za-z0-9_\-\.+/=]+)",
        QRegularExpression::CaseInsensitiveOption);
    out.replace(*bearerRe, QStringLiteral("\\1\\2\\3***REDACTED***"));

    // A numeric coordinate PAIR carried in a location=/gps= field, where the
    // field name names no axis. Quoted forms included: location="47.6,-122.3"
    // is how a JSON status frame spells it.
    static const QRegularExpression* coordinatePairRe = new QRegularExpression(
        R"(\b(gps(?:[_-]?location)?|location|coord(?:inates)?)(\\?["']?\s*[:=]\s*)\\?["']?[-+]?\d{1,3}(?:\.\d+)?\s*[,/]\s*[-+]?\d{1,3}(?:\.\d+)?\\?["']?)",
        QRegularExpression::CaseInsensitiveOption);
    out.replace(*coordinatePairRe, QStringLiteral("\\1\\2***REDACTED***"));

    // Maidenhead grid as a bare word after a "grid" keyword, which is how
    // FreeDvClient logs it. Anchored on the keyword and on the strict
    // 2-letter/2-digit[/2-letter] shape so ordinary prose is not eaten.
    static const QRegularExpression* gridWordRe = new QRegularExpression(
        R"(\b(grid|locator|maidenhead)(\s+)\\?"?[A-Za-z]{2}\d{2}(?:[A-Za-z]{2})?\\?"?\b)",
        QRegularExpression::CaseInsensitiveOption);
    out.replace(*gridWordRe, QStringLiteral("\\1\\2***REDACTED***"));

    // Peer hostnames after a connection keyword (WanConnection, MqttClient,
    // PgxlConnection, GreenHeronModel). The :port is preserved because it is
    // diagnostic and is not the operator's to hide; a trailing "timed out" or
    // similar prose keeps reading because the host token stops at whitespace.
    for (const char* kw : LogRedactionPolicy::kHostContextKeywords) {
        const QRegularExpression re(
            QStringLiteral(R"(\b(%1)(\s+)\\?"?([A-Za-z0-9\-]+(?:\.[A-Za-z0-9\-]+)*)\\?"?(?=[\s:,]|$))")
                .arg(QLatin1String(kw)),
            QRegularExpression::CaseInsensitiveOption);
        out.replace(re, QStringLiteral("\\1\\2***REDACTED***"));
    }

    // "user <name>" as logged by the Icom control-stream login line, where the
    // separator is whitespace rather than "=".
    static const QRegularExpression* userWordRe = new QRegularExpression(
        R"(\b(username|user)(\s+)(?!(?:name|id|agent)\b)\\?"?[A-Za-z0-9._\-]+\\?"?(?=[\s,.:]|$))",
        QRegularExpression::CaseInsensitiveOption);
    out.replace(*userWordRe, QStringLiteral("\\1\\2***REDACTED***"));

    // URL userinfo (scheme://user:pass@host) and the host authority itself.
    static const QRegularExpression* urlAuthorityRe = new QRegularExpression(
        R"(([A-Za-z][A-Za-z0-9+.\-]*://)(?:[^/\s:@]+(?::[^/\s@]*)?@)?([^/\s:?#]+))");
    out.replace(*urlAuthorityRe, QStringLiteral("\\1***REDACTED***"));

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

namespace {

// See the include block above for why this is not AetherSDR::setCurrentThreadName.
void nameThisThread()
{
#if defined(__linux__)
    prctl(PR_SET_NAME, "AsyncLogWriter", 0, 0, 0);   // 14 chars, inside the kernel's 15
#elif defined(__APPLE__)
    pthread_setname_np("AsyncLogWriter");
#elif defined(_WIN32)
    SetThreadDescription(GetCurrentThread(), L"AsyncLogWriter");
#endif
}

}  // namespace

void AsyncLogWriter::run(std::promise<bool> opened)
{
    // The one thread AetherSDR starts that Qt cannot name for us: a raw
    // std::thread never passes through QThreadPrivate::start(), so it read as
    // an unnamed row in the System Info thread table (#2554).
    nameThisThread();

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

    auto reopenForAppendOrMirrorToStderr = [&](const QString& reopenPath) {
        QDir().mkpath(QFileInfo(reopenPath).absolutePath());
        file.setFileName(reopenPath);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
            mirrorToStderr = true;
            return false;
        }
        file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        return true;
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
            if (!reopenForAppendOrMirrorToStderr(oldPath)) {
                maxFileBytes = 0;
                return;
            }
            maxFileBytes = 0;
            return;
        }

        QDir().mkpath(QFileInfo(newPath).absolutePath());
        file.setFileName(newPath);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            if (!reopenForAppendOrMirrorToStderr(oldPath)) {
                maxFileBytes = 0;
                return;
            }
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
                if (file.isOpen()) {
                    buffer.append(line);
                    ++bufferedLineCount;
                }
                if (mirrorToStderr) {
                    fwrite(line.constData(), 1, static_cast<size_t>(line.size()), stderr);
                    if (!file.isOpen()) {
                        fflush(stderr);
                    }
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
