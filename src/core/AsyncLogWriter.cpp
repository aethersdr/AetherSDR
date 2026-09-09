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
#include <vector>

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

// ---------------------------------------------------------------------------
// The value grammar every keyword rule shares.
//
// ESCAPE-AWARE ON PURPOSE, and the two quoted branches escape DIFFERENTLY.
//
// In ordinary text a quoted value may contain an escaped quote, so the value
// runs to its real closing quote: `[^"\\]|\\.` consumes an escaped character
// as one unit. A grammar that stopped at the first `"` ended the match early
// and left the tail of the secret standing — worse than not matching, because
// the line then looks redacted.
//
// In QDebug's spelling the DELIMITER is itself `\"`, so the same trick
// over-consumes: `\\.` happily eats the closing `\"` and runs on into the
// next field, which swallowed one JSON key and left the value after it
// unredacted. That branch therefore stops at the first real delimiter — but it
// must first consume an ENCODED escaped quote as a unit. QDebug doubles the
// backslash, so a JSON `\"` inside the value arrives as `\\` + `\"`; without
// that alternative the branch terminated inside it and left the value's tail
// in the log.
//
// The leading negative lookahead keeps redaction IDEMPOTENT: without it a
// second pass treats the marker `***REDACTED***` as a fresh value and eats its
// own prefix, so `token=***REDACTED***` became `token=***R***REDACTED***`.
// Idempotence is load-bearing — SupportBundle re-scrubs already-clean logs on
// the way into an archive.
constexpr const char* kSeparator = R"((\\?["']?\s*[:=]\s*))";
constexpr const char* kValue =
    R"((?!\*\*\*REDACTED\*\*\*)(?!(?:bearer|basic|digest)\s+\*\*\*REDACTED\*\*\*))"
    R"((?:\\"(?:\\\\\\"|(?!\\").)*\\"|"(?:[^"\\]|\\.)*"|'(?:[^'\\]|\\.)*'|[^\s#|,;&}\]]+))";

// An Authorization value may lead with a scheme word that is NOT the secret.
// Consuming it as the value redacts "Basic" and leaves the credential standing.
constexpr const char* kAuthScheme = R"((?:(bearer|basic|digest)(\s+))?)";

// Build once, use forever. These patterns are immutable, and compiling them
// per call cost ~1 ms per log line on the writer's hot path — which
// SupportBundle's per-line export loop then inherited synchronously.
struct FieldRule {
    QRegularExpression re;
    int                keepPrefix;
};

const std::vector<FieldRule>& fieldRules()
{
    static const std::vector<FieldRule>* rules = [] {
        auto* v = new std::vector<FieldRule>;
        const auto add = [&v](const char* keyword, int keep) {
            v->push_back({QRegularExpression(
                              QStringLiteral(R"(\b(%1)%2%3(%4))")
                                  .arg(QLatin1String(keyword), QLatin1String(kSeparator),
                                       QLatin1String(kAuthScheme), QLatin1String(kValue)),
                              QRegularExpression::CaseInsensitiveOption),
                          keep});
        };
        for (const auto& f : LogRedactionPolicy::kOpaqueValueFields)
            add(f.keyword, f.keepPrefixChars);
        for (const auto& f : LogRedactionPolicy::kSensitiveValueFields)
            add(f.keyword, f.keepPrefixChars);
        return v;
    }();
    return *rules;
}

const std::vector<QRegularExpression>& hostContextRules()
{
    static const std::vector<QRegularExpression>* rules = [] {
        auto* v = new std::vector<QRegularExpression>;
        for (const char* kw : LogRedactionPolicy::kHostContextKeywords) {
            // The captured token must LOOK like a host: a dotted name, or a
            // single label immediately followed by ":port". Without that the
            // keywords match ordinary prose — "disconnected from PipeWire" and
            // "resolving multiFLEX conflict" both lost their next word.
            v->push_back(QRegularExpression(
                QStringLiteral(
                    R"(\b(%1)(\s+)(\\?"?)(?:[A-Za-z0-9\-]+(?:\.[A-Za-z0-9\-]+)+|[A-Za-z0-9\-]+(?=:\d)))"
                    R"((\\?"?))")
                    .arg(QLatin1String(kw)),
                QRegularExpression::CaseInsensitiveOption));
        }
        return v;
    }();
    return *rules;
}

// Replace every match's value group, keeping `keepPrefix` leading characters
// only when the value is strictly longer than that. A prefix as long as the
// value is not a redaction.
QString redactField(QString in, const FieldRule& rule)
{
    QString out;
    qsizetype last = 0;
    auto it = rule.re.globalMatch(in);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        out += in.mid(last, m.capturedStart() - last);
        // Strip the quoting/escaping so the prefix decision is made against the
        // value itself, not against its punctuation.
        QString value = m.captured(5);
        QString open;
        if (value.startsWith(QLatin1String("\\\""))) { open = QStringLiteral("\\\""); }
        else if (value.startsWith('"') || value.startsWith('\'')) { open = value.left(1); }
        if (!open.isEmpty() && value.size() >= 2 * open.size())
            value = value.mid(open.size(), value.size() - 2 * open.size());
        // Already redacted (in any quoting) — re-emit verbatim. The regex
        // lookahead sits before the opening quote, so it cannot see a marker
        // inside one, and a quoted short value had its own marker re-eaten:
        // {"token":"ab"} became {"token":"***R***REDACTED***"} on the second
        // pass. SupportBundle re-scrubs already-clean logs, so this must hold.
        if (value.startsWith(QLatin1String("***REDACTED***"))) {
            out += m.captured(0);
            last = m.capturedEnd();
            continue;
        }
        const QString prefix =
            value.size() > rule.keepPrefix ? value.left(rule.keepPrefix) : QString();
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

    // ORDER MATTERS through the structural rules below. Eight-group IPv6 runs
    // before MAC (a MAC has six groups and cannot match it, but an address of
    // two-digit hextets would otherwise be eaten by the MAC rule first), MAC
    // runs before compressed IPv6, and home paths run before anything that
    // could match inside a user name.

    // Home directory prefix -> ~. Both slash styles, and the literal
    // /home/<user>, /Users/<user> and C:\Users\<user> roots, because the
    // running process's QDir::homePath() is not always the path's own root: a
    // Flatpak sandbox, an elevated run, or a log copied off another machine all
    // produce a home path this process never had.
    //
    // THE USER SEGMENT IS BOUNDED. A Windows user name may contain spaces, but
    // a rule that simply runs to the next separator swallows whatever follows
    // the path on the line: "home=/home/auditor status=connected" collapsed to
    // "home=~", deleting the diagnostic. Spaces are therefore accepted only
    // when a path separator actually follows the run.
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
        R"((?:/home/|/Users/|[A-Za-z]:\\{1,2}Users\\{1,2}))"
        R"((?:[^/\\:*?"<>|\r\n\s=]+(?:[ ]+[^/\\:*?"<>|\r\n\s=]+)*(?=[/\\"'])|[^/\\:*?"<>|\r\n\s=]+))",
        QRegularExpression::CaseInsensitiveOption);
    out.replace(*homeRootRe, QStringLiteral("~"));

    // IPv6, full eight-group form. Runs ahead of the MAC rule so an address
    // written as two-digit hextets (20:01:0d:b8:00:00:00:01) is recognised as
    // an address rather than half-consumed as a MAC.
    static const QRegularExpression* ipv6FullRe = new QRegularExpression(
        R"(\b(?:[0-9A-Fa-f]{1,4}:){7}[0-9A-Fa-f]{1,4}\b)");
    out.replace(*ipv6FullRe, QStringLiteral("[v6-redacted]"));

    // MAC addresses: 00-1C-2D-05-37-2A -> **-**-**-**-**-2A
    //                00:1C:2D:05:37:2A -> **:**:**:**:**:2A
    static const QRegularExpression* macRe = new QRegularExpression(
        R"(([0-9A-Fa-f]{2})([:-])([0-9A-Fa-f]{2})\2([0-9A-Fa-f]{2})\2([0-9A-Fa-f]{2})\2([0-9A-Fa-f]{2})\2([0-9A-Fa-f]{2}))");
    out.replace(*macRe, QStringLiteral("**\\2**\\2**\\2**\\2**\\2\\7"));

    // IPv6, compressed form. ANCHORED ON A HEXTET ADJACENT TO "::" — every
    // group around the "::" used to be optional, so the shortest string the
    // pattern matched was "::" on its own and any C++ qualified name in a
    // message was rewritten: "WanConnection::sendCommand" became
    // "WanConnection[v6-redacted]sendCommand". 48 log sites stream
    // Class::method as the literal start of their message.
    //
    // A hextet is required on at least one side, and the token must not be
    // flanked by other name characters, so a qualified name cannot match while
    // "::1", "fe80::1%eth0" and "::ffff:192.0.2.7" still do.
    static const QRegularExpression* ipv6CompressedRe = new QRegularExpression(
        R"((?<![0-9A-Za-z_:.\-])\[?(?:)"
        R"((?:[0-9A-Fa-f]{1,4}:(?!:))*[0-9A-Fa-f]{1,4}::(?:[0-9A-Fa-f]{1,4}:)*(?:\d{1,3}(?:\.\d{1,3}){3}|[0-9A-Fa-f]{1,4})?)"
        R"(|::(?:[0-9A-Fa-f]{1,4}:)*(?:\d{1,3}(?:\.\d{1,3}){3}|[0-9A-Fa-f]{1,4}))"
        R"()(?:%[0-9A-Za-z]+)?\]?(?![0-9A-Za-z_]))");
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


    // A DIGEST HEADER IS A PARAMETER LIST, not a single value, and the generic
    // one-value grammar left every parameter after the first standing —
    // including nonce and response. Redact the whole comma-separated tail.
    static const QRegularExpression* digestRe = new QRegularExpression(
        R"((\bauthorization\s*[:=]\s*digest\s+|\bdigest\s+(?=[A-Za-z]+\s*=))[^\r\n]*)",
        QRegularExpression::CaseInsensitiveOption);
    out.replace(*digestRe, QStringLiteral("\\1***REDACTED***"));

    // Every keyword rule, generated once from the one table (#5480).
    for (const FieldRule& rule : fieldRules())
        out = redactField(std::move(out), rule);

    // Bare email addresses in prose, AFTER the keyword rules.
    //
    // Running this first was an ordering bug: it rewrote the head of a field's
    // value to the marker, and the marker exclusion in kValue then made the
    // field rule skip the whole field — so "password=person@example.com!tail"
    // kept its tail. Keyword fields are redacted whole first; whatever address
    // is left is genuinely loose in prose.
    static const QRegularExpression* emailRe = new QRegularExpression(
        R"([A-Za-z0-9._%+\-]+@[A-Za-z0-9.\-]+\.[A-Za-z]{2,})");
    out.replace(*emailRe, QStringLiteral("***REDACTED***"));

    // The standalone "bearer <token>" scheme, which carries no separator and so
    // cannot come from the table. Digest is handled above; basic/bearer only
    // here, and only when the following token looks like a credential blob
    // rather than an ordinary word.
    static const QRegularExpression* bearerRe = new QRegularExpression(
        R"(\b(bearer|basic)(\s+)([A-Za-z0-9_\-\.]{4})[A-Za-z0-9_\-\.+/=]{4,})",
        QRegularExpression::CaseInsensitiveOption);
    out.replace(*bearerRe, QStringLiteral("\\1\\2\\3***REDACTED***"));

    // A numeric coordinate PAIR carried in a location=/gps= field.
    static const QRegularExpression* coordinatePairRe = new QRegularExpression(
        R"(\b(gps(?:[_-]?location)?|location|coord(?:inates)?)(\\?["']?\s*[:=]\s*)\\?["']?[-+]?\d{1,3}(?:\.\d+)?\s*[,/]\s*[-+]?\d{1,3}(?:\.\d+)?\\?["']?)",
        QRegularExpression::CaseInsensitiveOption);
    out.replace(*coordinatePairRe, QStringLiteral("\\1\\2***REDACTED***"));

    // Maidenhead grid as a bare word after a "grid" keyword.
    static const QRegularExpression* gridWordRe = new QRegularExpression(
        R"(\b(grid|locator|maidenhead)(\s+)\\?"?[A-Za-z]{2}\d{2}(?:[A-Za-z]{2})?\\?"?\b)",
        QRegularExpression::CaseInsensitiveOption);
    out.replace(*gridWordRe, QStringLiteral("\\1\\2***REDACTED***"));

    // Peer hostnames after a connection keyword.
    for (const QRegularExpression& re : hostContextRules())
        out.replace(re, QStringLiteral("\\1\\2\\3***REDACTED***\\4"));

    // "user <name>" as logged by the Icom control-stream login line.
    // "user <name>" as logged by the Icom control-stream login line
    // (IcomSession.cpp:273), which streams a QString and therefore QUOTES it.
    // The quotes are the discriminator and are required: "user" is ordinary
    // English before a bare word, and an unquoted rule ate the next word at
    // real sites ("user settings received", "user themes").
    static const QRegularExpression* userWordRe = new QRegularExpression(
        R"(\b(username|user)(\s+)(\\?["'])(?!\*\*\*REDACTED)[A-Za-z0-9._\-@]+(\\?["']))",
        QRegularExpression::CaseInsensitiveOption);
    out.replace(*userWordRe, QStringLiteral("\\1\\2\\3***REDACTED***\\4"));

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
