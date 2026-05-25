// AetherSDR rigctld protocol integration test.
// Requires a running AetherSDR instance with rigctld enabled (default: localhost:4532).
//
// Build:  cmake --build build --target rigctld_test
// Run:    ./build/rigctld_test [--host HOST] [--port PORT] [--ptt] [--cw] [--pty PATH]
//
// PTT tests (section 6) and CW/Morse tests (section 11) are disabled by default.
// Enable only when a dummy load or antenna is connected.
// PTY test (section 16) runs automatically; skips if the per-user cat-A symlink cannot be opened.

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QTcpSocket>
#include <QThread>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>

#if defined(Q_OS_UNIX)
#include <cerrno>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace {

// ── Per-user PTY symlink path (matches CatPort::defaultSymlinkPath) ───────────

static QString defaultPtyPath(int portIndex)
{
    const char letter = static_cast<char>('A' + portIndex);
    const QString leaf = QStringLiteral("cat-") + QChar(letter);
    QString base = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (base.isEmpty())
        base = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (base.isEmpty())
        base = QDir::homePath() + QStringLiteral("/.cache/aethersdr");
    if (!base.contains(QStringLiteral("aethersdr"), Qt::CaseInsensitive))
        base += QStringLiteral("/aethersdr");
    return base + QLatin1Char('/') + leaf;
}

// ── ANSI colour ───────────────────────────────────────────────────────────────

static bool g_tty = false;

QString ansi(const char* code, const QString& s)
{
    if (!g_tty) return s;
    return QLatin1String("\033[") + QLatin1String(code) + 'm' + s + QLatin1String("\033[0m");
}

static QString green(const QString& s)  { return ansi("92", s); }
static QString red(const QString& s)    { return ansi("91", s); }
static QString yellow(const QString& s) { return ansi("93", s); }
static QString cyan(const QString& s)   { return ansi("96", s); }
static QString bold(const QString& s)   { return ansi("1",  s); }

// ── RigctlClient ─────────────────────────────────────────────────────────────

class RigctlClient
{
public:
    explicit RigctlClient(int timeoutMs = 3000)
        : m_timeout(timeoutMs) {}

    bool connectToServer(const QString& host, quint16 port)
    {
        m_sock.connectToHost(host, port);
        if (!m_sock.waitForConnected(m_timeout)) {
            std::cerr << "Connection failed: "
                      << m_sock.errorString().toStdString() << '\n';
            return false;
        }
        m_sock.setSocketOption(QAbstractSocket::LowDelayOption, 1);
        return true;
    }

    // Send in extended mode ('+' prefix); read until RPRT line or 64-line limit.
    QStringList send(const QString& cmd)
    {
        m_sock.write('+' + cmd.toUtf8() + '\n');
        m_sock.flush();
        QStringList lines;
        while (lines.size() < 64) {
            QString line = readline();
            if (line.isNull()) break;
            lines.append(line);
            if (line.startsWith(QLatin1String("RPRT")))
                break;
        }
        return lines;
    }

    // Send without extended-mode prefix; read exactly n lines.
    QStringList sendRaw(const QString& cmd, int n)
    {
        m_sock.write(cmd.toUtf8() + '\n');
        m_sock.flush();
        QStringList lines;
        for (int i = 0; i < n; ++i) {
            QString line = readline();
            if (line.isNull()) break;
            lines.append(line);
        }
        return lines;
    }

    // Write raw bytes without framing (used by the two-line morse test).
    void writeBytes(const QByteArray& data)
    {
        m_sock.write(data);
        m_sock.flush();
    }

    // Read lines until RPRT or limit, skipping empty lines.
    QStringList readUntilRprt(int limit = 8)
    {
        QStringList lines;
        while (lines.size() < limit) {
            QString line = readline();
            if (line.isNull()) break;
            if (!line.isEmpty()) lines.append(line);
            if (line.startsWith(QLatin1String("RPRT"))) break;
        }
        return lines;
    }

    // Extract "Label: value" from an extended-mode response, or null QString.
    QString field(const QStringList& lines, const QString& label) const
    {
        const QString prefix = label + QLatin1String(": ");
        for (const QString& l : lines) {
            if (l.startsWith(prefix))
                return l.mid(prefix.size());
        }
        return {};
    }

    int rprt(const QStringList& lines) const
    {
        static const QRegularExpression kRprt(QStringLiteral(R"(RPRT\s+(-?\d+))"));
        for (const QString& l : lines) {
            const QRegularExpressionMatch m = kRprt.match(l);
            if (m.hasMatch())
                return m.captured(1).toInt();
        }
        return -999;
    }

    bool ok(const QStringList& lines) const { return rprt(lines) == 0; }

    void close()
    {
        m_sock.write("q\n");
        m_sock.flush();
        m_sock.disconnectFromHost();
    }

    // Public so callers can drive custom read loops (e.g. section 11.3).
    QString readline()
    {
        while (!m_buf.contains('\n')) {
            if (!m_sock.waitForReadyRead(m_timeout))
                return {};
            m_buf += m_sock.readAll();
        }
        const int nl = m_buf.indexOf('\n');
        QString line = QString::fromUtf8(m_buf.left(nl)).trimmed();
        m_buf.remove(0, nl + 1);
        return line;
    }

private:
    QTcpSocket m_sock;
    QByteArray m_buf;
    int        m_timeout;
};

// ── Runner ────────────────────────────────────────────────────────────────────

class Runner
{
public:
    void section(const QString& title)
    {
        std::cout << '\n' << bold(cyan(title)).toStdString() << '\n'
                  << std::string(64, '-') << '\n';
    }

    bool check(const QString& name, bool cond, const QString& detail = {})
    {
        if (cond) {
            ++m_passed;
            std::cout << "  " << green(QStringLiteral("PASS")).toStdString()
                      << "  " << name.toStdString() << '\n';
        } else {
            ++m_failed;
            std::cout << "  " << red(QStringLiteral("FAIL")).toStdString()
                      << "  " << name.toStdString();
            if (!detail.isEmpty())
                std::cout << "  " << yellow(QStringLiteral("→")).toStdString()
                          << ' ' << detail.toStdString();
            std::cout << '\n';
        }
        return cond;
    }

    void skip(const QString& name, const QString& reason = {})
    {
        ++m_skipped;
        std::cout << "  " << yellow(QStringLiteral("SKIP")).toStdString()
                  << "  " << name.toStdString();
        if (!reason.isEmpty())
            std::cout << "  (" << reason.toStdString() << ')';
        std::cout << '\n';
    }

    bool summary() const
    {
        const int total = m_passed + m_failed;
        std::cout << '\n' << std::string(64, '=') << '\n'
                  << bold(QStringLiteral("Results: %1/%2 passed")
                          .arg(m_passed).arg(total)).toStdString() << '\n';
        if (m_skipped)
            std::cout << "  Skipped: " << m_skipped << '\n';
        if (m_failed)
            std::cout << red(QStringLiteral("  Failed:  %1").arg(m_failed))
                             .toStdString() << '\n';
        std::cout << std::string(64, '=') << '\n';
        return m_failed == 0;
    }

    int failed() const { return m_failed; }

private:
    int m_passed{0};
    int m_failed{0};
    int m_skipped{0};
};

// ── Validators ────────────────────────────────────────────────────────────────

static bool isInt(const QString& s)
{
    if (s.isEmpty()) return false;
    bool ok = false;
    s.toLongLong(&ok);
    return ok;
}

static bool isFloat(const QString& s)
{
    if (s.isEmpty()) return false;
    bool ok = false;
    s.toDouble(&ok);
    return ok;
}

static const QSet<QString> kKnownModes = {
    QStringLiteral("USB"),    QStringLiteral("LSB"),    QStringLiteral("CW"),
    QStringLiteral("CWR"),    QStringLiteral("AM"),     QStringLiteral("AMS"),
    QStringLiteral("SAM"),    QStringLiteral("DSB"),    QStringLiteral("FM"),
    QStringLiteral("WFM"),    QStringLiteral("FMN"),    QStringLiteral("PKTUSB"),
    QStringLiteral("PKTLSB"), QStringLiteral("PKTFM"),  QStringLiteral("RTTY"),
    QStringLiteral("RTTYR"),  QStringLiteral("None"),
};

// ═════════════════════════════════════════════════════════════════════════════
// Section 1 — Connection & Initialization
// ═════════════════════════════════════════════════════════════════════════════

void section1(RigctlClient& c, Runner& r)
{
    r.section(QStringLiteral("Section 1 — Connection & Initialization"));

    // 1.2  dump_state
    QStringList lines = c.send(QStringLiteral("\\dump_state"));
    bool hasDone = std::any_of(lines.cbegin(), lines.cend(),
                               [](const QString& l){ return l.contains(QLatin1String("done")); });
    r.check(QStringLiteral(R"(1.2  dump_state returns capability block ending with "done")"),
            c.ok(lines) && hasDone,
            hasDone ? QString() : lines.join(QStringLiteral(" | ")).right(80));

    // 1.3  get_info (no extended-mode support — use sendRaw)
    QStringList raw = c.sendRaw(QStringLiteral("\\get_info"), 1);
    r.check(QStringLiteral("1.3  get_info returns a non-empty string"),
            !raw.isEmpty() && !raw[0].trimmed().isEmpty(),
            raw.isEmpty() ? QStringLiteral("(no response)") : raw[0]);

    // 1.4  get_rig_info
    lines = c.send(QStringLiteral("\\get_rig_info"));
    const QString info = c.field(lines, QStringLiteral("Info"));
    r.check(QStringLiteral("1.4  get_rig_info returns RPRT 0 with Info field"),
            c.ok(lines) && !info.isEmpty(), info);
}

// ═════════════════════════════════════════════════════════════════════════════
// Section 1b — Hamlib Init Probe Commands  (WSJT-X / client compatibility)
// ═════════════════════════════════════════════════════════════════════════════

void section1b(RigctlClient& c, Runner& r)
{
    r.section(QStringLiteral("Section 1b — Hamlib Init Probe Commands (WSJT-X compatibility)"));

    // 1b.1 / 1b.2  get/set_powerstat — was RPRT -4 before fix; WSJT-X checks this on init
    QStringList lines = c.send(QStringLiteral("\\get_powerstat"));
    const QString pwr = c.field(lines, QStringLiteral("Power Status"));
    r.check(QStringLiteral("1b.1  get_powerstat returns Power Status: 1"),
            c.ok(lines) && pwr == QLatin1String("1"), pwr);

    lines = c.send(QStringLiteral("\\set_powerstat 1"));
    r.check(QStringLiteral("1b.2  set_powerstat 1 accepted (RPRT 0)"), c.ok(lines));
    lines = c.send(QStringLiteral("\\set_powerstat 0"));
    r.check(QStringLiteral("1b.2b set_powerstat 0 (power off) rejected (RPRT -1)"),
            c.rprt(lines) == -1, lines.join(QStringLiteral(" | ")));

    // 1b.3 / 1b.4  get/set_lock_mode — was RPRT -4; WSJT-X interprets -4 as "locked"
    lines = c.send(QStringLiteral("\\get_lock_mode"));
    const QString lock = c.field(lines, QStringLiteral("Lock Mode"));
    r.check(QStringLiteral("1b.3  get_lock_mode returns Lock Mode: 0 (not locked)"),
            c.ok(lines) && lock == QLatin1String("0"), lock);

    lines = c.send(QStringLiteral("\\set_lock_mode 0"));
    r.check(QStringLiteral("1b.4  set_lock_mode 0 (unlock) accepted (RPRT 0)"), c.ok(lines));
    lines = c.send(QStringLiteral("\\set_lock_mode 1"));
    r.check(QStringLiteral("1b.4b set_lock_mode 1 (lock) rejected (RPRT -1)"),
            c.rprt(lines) == -1, lines.join(QStringLiteral(" | ")));

    // 1b.5  set_vfo_opt — VFO-prefix mode flag; chk_vfo=0 so this is a no-op
    lines = c.send(QStringLiteral("\\set_vfo_opt 0"));
    r.check(QStringLiteral("1b.5  set_vfo_opt 0 returns RPRT 0"), c.ok(lines));

    // 1b.6  hamlib_version — version info string
    lines = c.send(QStringLiteral("\\hamlib_version"));
    const QString hv = c.field(lines, QStringLiteral("Hamlib Version"));
    r.check(QStringLiteral("1b.6  hamlib_version returns Hamlib Version field"),
            c.ok(lines) && !hv.isEmpty(), hv);

    // 1b.7  get_vfo_list — must contain VFOA and VFOB
    lines = c.send(QStringLiteral("\\get_vfo_list"));
    const QString vfoList = c.field(lines, QStringLiteral("VFO List"));
    r.check(QStringLiteral("1b.7  get_vfo_list contains VFOA and VFOB"),
            c.ok(lines)
                && vfoList.contains(QLatin1String("VFOA"))
                && vfoList.contains(QLatin1String("VFOB")),
            vfoList);

    // 1b.8  get_modes — must contain USB and LSB
    lines = c.send(QStringLiteral("\\get_modes"));
    const QString modeList = c.field(lines, QStringLiteral("Modes"));
    r.check(QStringLiteral("1b.8  get_modes contains USB and LSB"),
            c.ok(lines)
                && modeList.contains(QLatin1String("USB"))
                && modeList.contains(QLatin1String("LSB")),
            modeList);

    // 1b.9  wait_morse — CWX queue depth is not queryable; returns RPRT -4 (not implemented)
    //        so clients that depend on serialized CW get an honest error rather than
    //        a false "done" that races with the actual transmission.
    lines = c.send(QStringLiteral("\\wait_morse"));
    r.check(QStringLiteral("1b.9  wait_morse returns RPRT -4 (not implementable on CWX)"),
            c.rprt(lines) == -4, lines.join(QStringLiteral(" | ")));
}

// ═════════════════════════════════════════════════════════════════════════════
// Section 2 — Frequency
// ═════════════════════════════════════════════════════════════════════════════

void section2(RigctlClient& c, Runner& r, qint64 origFreq)
{
    r.section(QStringLiteral("Section 2 — Frequency"));

    // 2.1  get_freq long form
    QStringList lines = c.send(QStringLiteral("\\get_freq"));
    const QString freqStr = c.field(lines, QStringLiteral("Frequency"));
    const qint64 gotFreq = freqStr.toLongLong();
    r.check(QStringLiteral("2.1  get_freq returns numeric Hz value"),
            c.ok(lines) && gotFreq > 0, freqStr);

    // 2.2 + 2.3  set then verify
    const qint64 testFreq = origFreq + 1000;
    lines = c.send(QStringLiteral("\\set_freq %1").arg(testFreq));
    r.check(QStringLiteral("2.2  set_freq returns RPRT 0"), c.ok(lines));
    QThread::msleep(150);

    lines = c.send(QStringLiteral("\\get_freq"));
    const qint64 confirmed = c.field(lines, QStringLiteral("Frequency")).toLongLong();
    r.check(QStringLiteral("2.3  get_freq confirms new frequency (%1 Hz)").arg(testFreq),
            qAbs(confirmed - testFreq) < 10,
            QStringLiteral("got %1").arg(confirmed));

    c.send(QStringLiteral("\\set_freq %1").arg(origFreq));
    QThread::msleep(100);

    // 2.4  short form get
    lines = c.send(QStringLiteral("f"));
    r.check(QStringLiteral("2.4  short form \"f\" (get_freq) returns RPRT 0 with Frequency field"),
            c.ok(lines) && isInt(c.field(lines, QStringLiteral("Frequency"))));

    // 2.5  short form set
    lines = c.send(QStringLiteral("F %1").arg(testFreq));
    r.check(QStringLiteral("2.5  short form \"F\" (set_freq) returns RPRT 0"), c.ok(lines));
    c.send(QStringLiteral("F %1").arg(origFreq));
    QThread::msleep(100);

    // 2.6  extended-mode format: send '+' manually via sendRaw, inspect structure
    QStringList raw = c.sendRaw(QStringLiteral("+\\get_freq"), 3);
    bool hasEcho  = std::any_of(raw.cbegin(), raw.cend(),
                                [](const QString& l){ return l.contains(QLatin1String("get_freq")); });
    bool hasLabel = std::any_of(raw.cbegin(), raw.cend(),
                                [](const QString& l){ return l.startsWith(QLatin1String("Frequency:")); });
    bool hasRprt  = std::any_of(raw.cbegin(), raw.cend(),
                                [](const QString& l){ return l.startsWith(QLatin1String("RPRT")); });
    r.check(QStringLiteral("2.6  extended-mode response has echo, \"Frequency:\" label, and RPRT"),
            hasEcho && hasLabel && hasRprt, raw.join(QStringLiteral(" | ")));
}

// ═════════════════════════════════════════════════════════════════════════════
// Section 3 — Mode
// ═════════════════════════════════════════════════════════════════════════════

void section3(RigctlClient& c, Runner& r, const QString& origMode, int origPb)
{
    r.section(QStringLiteral("Section 3 — Mode"));

    // 3.1  get_mode
    QStringList lines = c.send(QStringLiteral("\\get_mode"));
    const QString mode = c.field(lines, QStringLiteral("Mode"));
    const QString pb   = c.field(lines, QStringLiteral("Passband"));
    r.check(QStringLiteral("3.1  get_mode returns known mode string and numeric passband"),
            c.ok(lines) && kKnownModes.contains(mode) && isInt(pb),
            QStringLiteral("mode=%1 passband=%2").arg(mode, pb));

    struct ModeCase { const char* mode; int pb; const char* label; };
    static constexpr ModeCase kModes[] = {
        {"USB",    2700, "3.2"},
        {"LSB",    2700, "3.3"},
        {"CW",      500, "3.4"},
        {"PKTUSB", 3000, "3.5"},
        {"AM",     6000, "3.6"},
        {"FM",    10000, "3.7"},
    };
    for (const auto& t : kModes) {
        lines = c.send(QStringLiteral("\\set_mode %1 %2")
                           .arg(QLatin1String(t.mode)).arg(t.pb));
        r.check(QStringLiteral("%1  set_mode %2 %3 returns RPRT 0")
                    .arg(QLatin1String(t.label)).arg(QLatin1String(t.mode)).arg(t.pb),
                c.ok(lines));
    }

    // 3.8  passband 0 → radio picks default
    lines = c.send(QStringLiteral("\\set_mode USB 0"));
    r.check(QStringLiteral("3.8  set_mode USB 0 (radio default passband) returns RPRT 0"),
            c.ok(lines));

    // 3.9 / 3.10  set_mode ? capability probe — must return mode list, NOT change current mode
    //              (MacLoggerDX regression: missing guard caused set_mode ? to set mode to USB)
    {
        const QString modeBeforeProbe =
            c.field(c.send(QStringLiteral("\\get_mode")), QStringLiteral("Mode"));
        QStringList probe = c.send(QStringLiteral("\\set_mode ?"));
        const bool hasModes = std::any_of(probe.cbegin(), probe.cend(),
                                  [](const QString& l){
                                      return l.contains(QLatin1String("USB"))
                                          && l.contains(QLatin1String("LSB")); });
        r.check(QStringLiteral("3.9  set_mode ? returns capability list (USB and LSB present)"),
                hasModes, probe.join(QStringLiteral(" | ")));
        // No sleep: check immediately to stay within the synchronous round-trip window
        // and avoid false failures from unrelated async radio status updates.
        const QString modeAfterProbe =
            c.field(c.send(QStringLiteral("\\get_mode")), QStringLiteral("Mode"));
        r.check(QStringLiteral("3.10 set_mode ? does NOT change current mode (regression guard)"),
                !modeBeforeProbe.isEmpty() && modeBeforeProbe == modeAfterProbe,
                QStringLiteral("before=%1 after=%2").arg(modeBeforeProbe, modeAfterProbe));
    }

    c.send(QStringLiteral("\\set_mode %1 %2").arg(origMode).arg(origPb));
    QThread::msleep(100);
}

// ═════════════════════════════════════════════════════════════════════════════
// Section 4 — VFO
// ═════════════════════════════════════════════════════════════════════════════

void section4(RigctlClient& c, Runner& r)
{
    r.section(QStringLiteral("Section 4 — VFO"));

    static const QSet<QString> kValidVfos = {
        QStringLiteral("VFOA"), QStringLiteral("VFOB"),
        QStringLiteral("Main"), QStringLiteral("Sub"), QStringLiteral("currVFO"),
    };

    // 4.1  get_vfo
    QStringList lines = c.send(QStringLiteral("\\get_vfo"));
    const QString vfo = c.field(lines, QStringLiteral("VFO"));
    r.check(QStringLiteral("4.1  get_vfo returns VFOA/VFOB/Main/Sub"),
            c.ok(lines) && kValidVfos.contains(vfo), vfo);

    // 4.2  set_vfo
    lines = c.send(QStringLiteral("\\set_vfo VFOA"));
    r.check(QStringLiteral("4.2  set_vfo VFOA returns RPRT 0"), c.ok(lines));

    // 4.3 / 4.4  vfo_op UP then DOWN — verify step-size movement
    const QStringList stepLines = c.send(QStringLiteral("\\get_ts"));
    const QString stepStr = c.field(stepLines, QStringLiteral("Tuning Step"));
    const qint64 step = stepStr.toLongLong();

    const qint64 freqBefore =
        c.field(c.send(QStringLiteral("\\get_freq")), QStringLiteral("Frequency")).toLongLong();

    c.send(QStringLiteral("\\vfo_op UP"));
    qint64 freqUp = 0;
    {
        QElapsedTimer t; t.start();
        do {
            freqUp = c.field(c.send(QStringLiteral("\\get_freq")),
                             QStringLiteral("Frequency")).toLongLong();
            if (qAbs(freqUp - (freqBefore + step)) <= 1 || t.elapsed() >= 1000) break;
            QThread::msleep(100);
        } while (true);
    }
    r.check(QStringLiteral("4.3  vfo_op UP increases frequency by one tuning step"),
            step > 0 && qAbs(freqUp - (freqBefore + step)) <= 1,
            QStringLiteral("before=%1 after=%2 step=%3").arg(freqBefore).arg(freqUp).arg(step));

    c.send(QStringLiteral("\\vfo_op DOWN"));
    qint64 freqDown = 0;
    {
        QElapsedTimer t; t.start();
        do {
            freqDown = c.field(c.send(QStringLiteral("\\get_freq")),
                               QStringLiteral("Frequency")).toLongLong();
            if (qAbs(freqDown - freqBefore) <= 1 || t.elapsed() >= 2000) break;
            QThread::msleep(100);
        } while (true);
    }
    r.check(QStringLiteral("4.4  vfo_op DOWN restores frequency"),
            qAbs(freqDown - freqBefore) <= 1,
            QStringLiteral("expected=%1 got=%2").arg(freqBefore).arg(freqDown));

    // 4.5 / 4.6  get_vfo_info VFOA and VFOB
    static const QPair<const char*, const char*> kVfoInfoTests[] = {
        {"VFOA", "4.5"}, {"VFOB", "4.6"},
    };
    static const QStringList kVfoFields = {
        QStringLiteral("Freq"), QStringLiteral("Mode"),
        QStringLiteral("Width"), QStringLiteral("Split"), QStringLiteral("SatMode"),
    };
    for (const auto& [vfoName, label] : kVfoInfoTests) {
        const QString vfoQ = QLatin1String(vfoName);
        lines = c.send(QStringLiteral("\\get_vfo_info ") + vfoQ);
        bool allPresent = std::all_of(kVfoFields.cbegin(), kVfoFields.cend(),
                                      [&](const QString& f){ return !c.field(lines, f).isNull(); });
        bool echoOk = std::any_of(lines.cbegin(), lines.cend(), [&](const QString& l){
            return l.contains(QLatin1String("get_vfo_info: ") + vfoQ);
        });
        r.check(QStringLiteral("%1  get_vfo_info %2 — all 5 fields present, echo includes VFO")
                    .arg(QLatin1String(label), vfoQ),
                c.ok(lines) && allPresent && echoOk,
                QStringLiteral("allPresent=%1 echoOk=%2")
                    .arg(allPresent ? "yes" : "no", echoOk ? "yes" : "no"));
    }

    // 4.7 / 4.8  get_ts / set_ts
    r.check(QStringLiteral("4.7  get_ts returns numeric tuning step (Hz)"),
            c.ok(stepLines) && isInt(stepStr), stepStr);

    lines = c.send(QStringLiteral("\\set_ts 100"));
    r.check(QStringLiteral("4.8  set_ts 100 returns RPRT 0"), c.ok(lines));

    // 4.8b  get_ts read-back confirms 100 Hz (poll: command is async via QueuedConnection)
    QString tsConfirmed;
    {
        QElapsedTimer t; t.start();
        do {
            lines = c.send(QStringLiteral("\\get_ts"));
            tsConfirmed = c.field(lines, QStringLiteral("Tuning Step"));
            if (tsConfirmed.toLongLong() == 100 || t.elapsed() >= 1000) break;
            QThread::msleep(100);
        } while (true);
    }
    if (c.ok(lines) && tsConfirmed.toLongLong() == 100) {
        r.check(QStringLiteral("4.8b get_ts confirms 100 Hz tuning step after set"), true);
    } else {
        // Radio may snap to a band-specific valid step or reject the command silently.
        r.skip(QStringLiteral("4.8b get_ts read-back after set_ts 100"),
               QStringLiteral("radio returned %1 Hz (band/mode may constrain valid steps)").arg(tsConfirmed));
    }

    if (isInt(stepStr))
        c.send(QStringLiteral("\\set_ts %1").arg(stepStr));

    // 4.9  set_ts ? capability probe
    lines = c.send(QStringLiteral("\\set_ts ?"));
    const bool hasTsSteps = std::any_of(lines.cbegin(), lines.cend(),
                                [](const QString& l){
                                    return l.contains(QLatin1String("Tuning Steps")); });
    r.check(QStringLiteral("4.9  set_ts ? returns Tuning Steps capability list"),
            c.ok(lines) && hasTsSteps, lines.join(QStringLiteral(" | ")));
}

// ═════════════════════════════════════════════════════════════════════════════
// Section 5 — Split VFO
// ═════════════════════════════════════════════════════════════════════════════

void section5(RigctlClient& c, Runner& r, qint64 origFreq)
{
    r.section(QStringLiteral("Section 5 — Split VFO"));
    const qint64 splitTxFreq = origFreq + 1000;

    // 5.1  baseline split state
    QStringList lines = c.send(QStringLiteral("\\get_split_vfo"));
    const QString splitVal = c.field(lines, QStringLiteral("Split"));
    const QString txVfo    = c.field(lines, QStringLiteral("TX VFO"));
    r.check(QStringLiteral("5.1  get_split_vfo returns Split (0/1) and TX VFO fields"),
            c.ok(lines)
                && (splitVal == QLatin1String("0") || splitVal == QLatin1String("1"))
                && (txVfo == QLatin1String("VFOA") || txVfo == QLatin1String("VFOB")),
            QStringLiteral("Split=%1 TX VFO=%2").arg(splitVal, txVfo));

    // 5.2  enable split
    lines = c.send(QStringLiteral("\\set_split_vfo 1 VFOB"));
    r.check(QStringLiteral("5.2  set_split_vfo 1 VFOB returns RPRT 0"), c.ok(lines));

    // 5.3  poll until split is confirmed active (creating a new slice takes variable time)
    QString splitOn;
    {
        QElapsedTimer t;
        t.start();
        do {
            lines = c.send(QStringLiteral("\\get_split_vfo"));
            splitOn = c.field(lines, QStringLiteral("Split"));
            if (splitOn == QLatin1String("1") || t.elapsed() >= 5000) break;
            QThread::msleep(200);
        } while (true);
    }
    r.check(QStringLiteral("5.3  get_split_vfo reports Split: 1  [needs 2 slices in GUI]"),
            c.ok(lines) && splitOn == QLatin1String("1"),
            QStringLiteral("Split=%1 — open a second slice in AetherSDR if this fails")
                .arg(splitOn));

    // 5.4 / 5.5  set and get split frequency
    lines = c.send(QStringLiteral("\\set_split_freq %1").arg(splitTxFreq));
    r.check(QStringLiteral("5.4  set_split_freq returns RPRT 0"), c.ok(lines));
    QThread::msleep(100);

    lines = c.send(QStringLiteral("\\get_split_freq"));
    const QString sf = c.field(lines, QStringLiteral("TX Frequency"));
    r.check(QStringLiteral("5.5  get_split_freq returns %1 Hz").arg(splitTxFreq),
            c.ok(lines) && isInt(sf) && qAbs(sf.toLongLong() - splitTxFreq) < 10, sf);

    // 5.5b  VFOA (RX) and VFOB (TX) frequencies must be independent
    //       (MacLoggerDX race regression: m_pendingTxSlice not set synchronously meant
    //        set_split_freq wrote to the wrong slice and get_split_freq reported VFOA freq)
    {
        const qint64 rxNow =
            c.field(c.send(QStringLiteral("\\get_freq")), QStringLiteral("Frequency")).toLongLong();
        r.check(QStringLiteral("5.5b VFOA (RX) and VFOB (TX) are independent  [needs 2 slices]"),
                rxNow > 0 && isInt(sf) && qAbs(rxNow - sf.toLongLong()) >= 500,
                QStringLiteral("VFOA=%1 VFOB=%2").arg(rxNow).arg(sf));
    }

    // 5.6 / 5.7  set and get split mode
    lines = c.send(QStringLiteral("\\set_split_mode USB 2700"));
    r.check(QStringLiteral("5.6  set_split_mode USB 2700 returns RPRT 0"), c.ok(lines));

    lines = c.send(QStringLiteral("\\get_split_mode"));
    const QString sm = c.field(lines, QStringLiteral("TX Mode"));
    r.check(QStringLiteral("5.7  get_split_mode returns TX Mode field in known modes"),
            c.ok(lines) && kKnownModes.contains(sm), sm);

    // 5.7b  get_split_freq_mode returns TX Frequency and TX Mode together
    lines = c.send(QStringLiteral("\\get_split_freq_mode"));
    const QString sfmFreq = c.field(lines, QStringLiteral("TX Frequency"));
    const QString sfmMode = c.field(lines, QStringLiteral("TX Mode"));
    r.check(QStringLiteral("5.7b get_split_freq_mode returns TX Frequency and TX Mode"),
            c.ok(lines) && isInt(sfmFreq) && kKnownModes.contains(sfmMode),
            QStringLiteral("TX Frequency=%1 TX Mode=%2").arg(sfmFreq, sfmMode));

    // 5.7c / 5.7d  set_split_freq_mode round-trip
    const qint64 sfmTestFreq = origFreq + 2000;
    lines = c.send(QStringLiteral("\\set_split_freq_mode %1 USB 2700").arg(sfmTestFreq));
    r.check(QStringLiteral("5.7c set_split_freq_mode returns RPRT 0"), c.ok(lines));
    QThread::msleep(100);
    lines = c.send(QStringLiteral("\\get_split_freq_mode"));
    const QString sfmFreq2 = c.field(lines, QStringLiteral("TX Frequency"));
    r.check(QStringLiteral("5.7d get_split_freq_mode confirms TX freq after set_split_freq_mode"),
            c.ok(lines) && isInt(sfmFreq2) && qAbs(sfmFreq2.toLongLong() - sfmTestFreq) < 10,
            QStringLiteral("expected=%1 got=%2").arg(sfmTestFreq).arg(sfmFreq2));

    // 5.8  get_vfo_info VFOB reflects split  [needs 2 slices]
    lines = c.send(QStringLiteral("\\get_vfo_info VFOB"));
    const QString splitF = c.field(lines, QStringLiteral("Split"));
    r.check(QStringLiteral("5.8  get_vfo_info VFOB shows Split: 1 when active  [needs 2 slices]"),
            c.ok(lines) && splitF == QLatin1String("1"),
            QStringLiteral("Split=%1").arg(splitF));

    // 5.9  disable split and verify
    lines = c.send(QStringLiteral("\\set_split_vfo 0 VFOA"));
    r.check(QStringLiteral("5.9  set_split_vfo 0 VFOA returns RPRT 0"), c.ok(lines));

    // 5.9b  poll until split is confirmed off (slice teardown takes variable time)
    QString splitOff;
    {
        QElapsedTimer t;
        t.start();
        do {
            lines = c.send(QStringLiteral("\\get_split_vfo"));
            splitOff = c.field(lines, QStringLiteral("Split"));
            if (splitOff == QLatin1String("0") || t.elapsed() >= 2000) break;
            QThread::msleep(200);
        } while (true);
    }
    r.check(QStringLiteral("5.9b get_split_vfo confirms Split: 0 after disable"),
            c.ok(lines) && splitOff == QLatin1String("0"), splitOff);

    // 5.10 / 5.10b  deferred stash path: enable split then immediately set freq + mode.
    //               Single-slice: stashed in m_pendingSplitFreqMHz / m_pendingSplitMode,
    //               applied by tryPromoteTxSlice() when the new slice appears.
    //               Two-slice: applied immediately to the existing TX slice.
    //               Both paths must produce the correct TX frequency on get_split_freq.
    lines = c.send(QStringLiteral("\\set_split_vfo 1 VFOB"));
    r.check(QStringLiteral("5.10 set_split_vfo 1 VFOB (deferred/stash path) returns RPRT 0"),
            c.ok(lines));
    const qint64 stashFreq = origFreq + 3700;
    c.send(QStringLiteral("\\set_split_freq %1").arg(stashFreq));
    c.send(QStringLiteral("\\set_split_mode USB 2700"));
    qint64 stashConfirm = 0;
    {
        QElapsedTimer t; t.start();
        do {
            stashConfirm = c.field(c.send(QStringLiteral("\\get_split_freq")),
                                   QStringLiteral("TX Frequency")).toLongLong();
            if ((stashConfirm > 0 && qAbs(stashConfirm - stashFreq) < 100)
                    || t.elapsed() >= 500)
                break;
            QThread::msleep(50);
        } while (true);
    }
    r.check(QStringLiteral("5.10b stashed split freq applied to TX slice"),
            stashConfirm > 0 && qAbs(stashConfirm - stashFreq) < 100,
            QStringLiteral("expected≈%1 got=%2").arg(stashFreq).arg(stashConfirm));
    c.send(QStringLiteral("\\set_split_vfo 0 VFOA"));
}

// ═════════════════════════════════════════════════════════════════════════════
// Section 6 — PTT  (optional — requires dummy load or antenna)
// ═════════════════════════════════════════════════════════════════════════════

void section6Ptt(RigctlClient& c, Runner& r)
{
    r.section(QStringLiteral("Section 6 — PTT  ⚠  TX ACTIVE"));

    // Power safety: read current power, drop to 0.01 (≈ 1 W) and confirm before keying
    QStringList lines = c.send(QStringLiteral("\\get_level RFPOWER"));
    const QString origRfpower = c.field(lines, QStringLiteral("RFPOWER"));
    r.check(QStringLiteral("6.0  read RFPOWER before PTT tests"),
            c.ok(lines) && isFloat(origRfpower), origRfpower);

    lines = c.send(QStringLiteral("\\set_level RFPOWER 0.01"));
    r.check(QStringLiteral("6.0b set RFPOWER 0.01 (≈ 1 W) returns RPRT 0"), c.ok(lines));
    QString safePwr;
    {
        QElapsedTimer t; t.start();
        do {
            lines = c.send(QStringLiteral("\\get_level RFPOWER"));
            safePwr = c.field(lines, QStringLiteral("RFPOWER"));
            if ((isFloat(safePwr) && safePwr.toDouble() <= 0.03) || t.elapsed() >= 1000) break;
            QThread::msleep(100);
        } while (true);
    }
    r.check(QStringLiteral("6.0c confirm RFPOWER ≤ 0.03 before keying"),
            c.ok(lines) && isFloat(safePwr) && safePwr.toDouble() <= 0.03, safePwr);

    lines = c.send(QStringLiteral("\\get_ptt"));
    const QString ptt = c.field(lines, QStringLiteral("PTT"));
    r.check(QStringLiteral("6.1  get_ptt returns 0 (RX) before keying"),
            c.ok(lines) && ptt == QLatin1String("0"), ptt);

    lines = c.send(QStringLiteral("\\set_ptt 1"));
    r.check(QStringLiteral("6.2  set_ptt 1 returns RPRT 0"), c.ok(lines));
    QThread::msleep(1000);  // hold TX 1 s so carrier is audible on dummy load

    lines = c.send(QStringLiteral("\\get_ptt"));
    const QString pttTx = c.field(lines, QStringLiteral("PTT"));
    r.check(QStringLiteral("6.3  get_ptt returns 1 while transmitting"),
            c.ok(lines) && pttTx == QLatin1String("1"), pttTx);

    lines = c.send(QStringLiteral("\\set_ptt 0"));
    r.check(QStringLiteral("6.4  set_ptt 0 returns RPRT 0"), c.ok(lines));
    QThread::msleep(250);

    lines = c.send(QStringLiteral("\\get_ptt"));
    const QString pttRx = c.field(lines, QStringLiteral("PTT"));
    r.check(QStringLiteral("6.4b get_ptt returns 0 after releasing PTT"),
            c.ok(lines) && pttRx == QLatin1String("0"), pttRx);

    lines = c.send(QStringLiteral("\\get_dcd"));
    const QString dcd = c.field(lines, QStringLiteral("DCD"));
    r.check(QStringLiteral("6.5  get_dcd returns 0 or 1"),
            c.ok(lines) && (dcd == QLatin1String("0") || dcd == QLatin1String("1")), dcd);

    // Restore power and confirm
    if (isFloat(origRfpower)) {
        lines = c.send(QStringLiteral("\\set_level RFPOWER ") + origRfpower);
        r.check(QStringLiteral("6.6  restore RFPOWER returns RPRT 0"), c.ok(lines));
        QString pwrRestored;
        {
            QElapsedTimer t; t.start();
            do {
                lines = c.send(QStringLiteral("\\get_level RFPOWER"));
                pwrRestored = c.field(lines, QStringLiteral("RFPOWER"));
                if ((isFloat(pwrRestored) && qAbs(pwrRestored.toDouble() - origRfpower.toDouble()) < 0.02)
                        || t.elapsed() >= 1000) break;
                QThread::msleep(100);
            } while (true);
        }
        r.check(QStringLiteral("6.6b confirm RFPOWER restored"),
                c.ok(lines) && isFloat(pwrRestored)
                    && qAbs(pwrRestored.toDouble() - origRfpower.toDouble()) < 0.02,
                pwrRestored);
    }

    // Safety: watch 2 s — firmware may briefly re-assert PTT after set_ptt 0
    {
        QElapsedTimer safeTimer; safeTimer.start();
        bool sawTxOn = false;
        c.send(QStringLiteral("\\set_ptt 0"));
        do {
            QThread::msleep(250);
            lines = c.send(QStringLiteral("\\get_ptt"));
            const QString pttSafe = c.field(lines, QStringLiteral("PTT"));
            if (pttSafe == QLatin1String("1")) { sawTxOn = true; c.send(QStringLiteral("\\set_ptt 0")); }
        } while (safeTimer.elapsed() < 2000);
        if (sawTxOn)
            qWarning("6.S: PTT re-asserted during 2-s safety watch — forced set_ptt 0; possible firmware bug");
        r.check(QStringLiteral("6.S  safety: PTT confirmed 0 after 2-s watch"),
                c.ok(lines) && c.field(lines, QStringLiteral("PTT")) == QLatin1String("0"),
                c.field(lines, QStringLiteral("PTT")));
    }
}

void section6Skip(Runner& r)
{
    r.section(QStringLiteral("Section 6 — PTT  (skipped — pass --ptt to enable)"));
    for (const auto* name : {
             "6.0  read RFPOWER", "6.0b set RFPOWER 0.01", "6.0c confirm RFPOWER",
             "6.1  get_ptt baseline", "6.2  set_ptt 1", "6.3  get_ptt TX",
             "6.4  set_ptt 0",        "6.4b get_ptt RX", "6.5  get_dcd",
             "6.6  restore RFPOWER", "6.6b confirm RFPOWER restored",
             "6.S  safety: PTT 0 after 2-s watch",
         })
        r.skip(QLatin1String(name), QStringLiteral("--ptt not set"));
}

// ═════════════════════════════════════════════════════════════════════════════
// Section 7 — Levels
// ═════════════════════════════════════════════════════════════════════════════

void section7(RigctlClient& c, Runner& r)
{
    r.section(QStringLiteral("Section 7 — Levels"));

    // 7.5  STRENGTH is read-only
    QStringList lines = c.send(QStringLiteral("\\get_level STRENGTH"));
    const QString strength = c.field(lines, QStringLiteral("STRENGTH"));
    r.check(QStringLiteral("7.5  get_level STRENGTH returns numeric dB value"),
            c.ok(lines) && isFloat(strength), strength);

    struct LevelCase { const char* name; const char* getId; const char* setId; double testVal; };
    static constexpr LevelCase kLevels[] = {
        {"AF",  "7.1", "7.2", 0.75},
        {"RF",  "7.3", "7.4", 1.0},
        {"SQL", "7.6", "7.7", 0.3},
    };
    for (const auto& lv : kLevels) {
        const QString lvName = QLatin1String(lv.name);
        lines = c.send(QStringLiteral("\\get_level ") + lvName);
        const QString val = c.field(lines, lvName);
        r.check(QStringLiteral("%1  get_level %2 returns numeric value")
                    .arg(QLatin1String(lv.getId), lvName),
                c.ok(lines) && isFloat(val), val);

        const QString orig = val;
        lines = c.send(QStringLiteral("\\set_level %1 %2").arg(lvName).arg(lv.testVal));
        r.check(QStringLiteral("%1  set_level %2 %3 returns RPRT 0")
                    .arg(QLatin1String(lv.setId), lvName).arg(lv.testVal),
                c.ok(lines));
        if (isFloat(orig))
            c.send(QStringLiteral("\\set_level %1 %2").arg(lvName, orig));
    }

    // 7.8 / 7.9 / 7.9b / 7.9c / 7.9d  RFPOWER — set to 50 W, confirm, restore, confirm
    lines = c.send(QStringLiteral("\\get_level RFPOWER"));
    const QString origRfpower = c.field(lines, QStringLiteral("RFPOWER"));
    r.check(QStringLiteral("7.8  get_level RFPOWER returns numeric value"),
            c.ok(lines) && isFloat(origRfpower), origRfpower);

    lines = c.send(QStringLiteral("\\set_level RFPOWER 0.5"));
    r.check(QStringLiteral("7.9  set_level RFPOWER 0.5 (50 W) returns RPRT 0"), c.ok(lines));
    QString pwrSet;
    {
        QElapsedTimer t; t.start();
        do {
            lines = c.send(QStringLiteral("\\get_level RFPOWER"));
            pwrSet = c.field(lines, QStringLiteral("RFPOWER"));
            if ((isFloat(pwrSet) && qAbs(pwrSet.toDouble() - 0.5) < 0.02) || t.elapsed() >= 1000) break;
            QThread::msleep(100);
        } while (true);
    }
    r.check(QStringLiteral("7.9b confirm RFPOWER ≈ 0.5 (50 W)"),
            c.ok(lines) && isFloat(pwrSet) && qAbs(pwrSet.toDouble() - 0.5) < 0.02, pwrSet);

    if (isFloat(origRfpower)) {
        lines = c.send(QStringLiteral("\\set_level RFPOWER ") + origRfpower);
        r.check(QStringLiteral("7.9c restore RFPOWER returns RPRT 0"), c.ok(lines));
        QString pwrRestored;
        {
            QElapsedTimer t; t.start();
            do {
                lines = c.send(QStringLiteral("\\get_level RFPOWER"));
                pwrRestored = c.field(lines, QStringLiteral("RFPOWER"));
                if ((isFloat(pwrRestored) && qAbs(pwrRestored.toDouble() - origRfpower.toDouble()) < 0.02)
                        || t.elapsed() >= 1000) break;
                QThread::msleep(100);
            } while (true);
        }
        r.check(QStringLiteral("7.9d confirm RFPOWER restored"),
                c.ok(lines) && isFloat(pwrRestored)
                    && qAbs(pwrRestored.toDouble() - origRfpower.toDouble()) < 0.02,
                pwrRestored);
    }

    // 7.10 / 7.11  KEYSPD (WPM)
    lines = c.send(QStringLiteral("\\get_level KEYSPD"));
    const QString ks = c.field(lines, QStringLiteral("KEYSPD"));
    r.check(QStringLiteral("7.10 get_level KEYSPD returns numeric WPM value"),
            c.ok(lines) && isFloat(ks), ks);

    lines = c.send(QStringLiteral("\\set_level KEYSPD 20"));
    r.check(QStringLiteral("7.11 set_level KEYSPD 20 returns RPRT 0"), c.ok(lines));
    if (isFloat(ks))
        c.send(QStringLiteral("\\set_level KEYSPD %1").arg(qRound(ks.toDouble())));
}

// ═════════════════════════════════════════════════════════════════════════════
// Section 8 — Functions
// ═════════════════════════════════════════════════════════════════════════════

void section8(RigctlClient& c, Runner& r)
{
    r.section(QStringLiteral("Section 8 — Functions"));

    struct FuncCase { const char* name; const char* getId; const char* setOnId; const char* setOffId; };
    static constexpr FuncCase kFuncs[] = {
        {"NB",   "8.1",  "8.2",  "8.3"},
        {"NR",   "8.4",  "8.5",  nullptr},
        {"ANF",  "8.6",  "8.7",  nullptr},
        {"VOX",  "8.8",  "8.9",  nullptr},
        {"MUTE", "8.10", nullptr, nullptr},
    };
    for (const auto& fn : kFuncs) {
        const QString fnName = QLatin1String(fn.name);
        QStringList lines = c.send(QStringLiteral("\\get_func ") + fnName);
        const QString val = c.field(lines, fnName);
        r.check(QStringLiteral("%1  get_func %2 returns 0 or 1")
                    .arg(QLatin1String(fn.getId), fnName),
                c.ok(lines) && (val == QLatin1String("0") || val == QLatin1String("1")), val);

        const QString orig = val;
        if (fn.setOnId) {
            lines = c.send(QStringLiteral("\\set_func %1 1").arg(fnName));
            r.check(QStringLiteral("%1  set_func %2 1 returns RPRT 0")
                        .arg(QLatin1String(fn.setOnId), fnName),
                    c.ok(lines));
        }
        if (fn.setOffId) {
            lines = c.send(QStringLiteral("\\set_func %1 0").arg(fnName));
            r.check(QStringLiteral("%1  set_func %2 0 returns RPRT 0")
                        .arg(QLatin1String(fn.setOffId), fnName),
                    c.ok(lines));
        }
        if (fn.setOnId && !orig.isNull())
            c.send(QStringLiteral("\\set_func %1 %2").arg(fnName, orig));
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Section 9 — RIT / XIT
// ═════════════════════════════════════════════════════════════════════════════

void section9(RigctlClient& c, Runner& r)
{
    r.section(QStringLiteral("Section 9 — RIT / XIT"));

    struct RitCase {
        const char* name; const char* cmdGet; const char* cmdSet;
        const char* getId; const char* setId; const char* clrId;
    };
    static constexpr RitCase kTests[] = {
        {"RIT", "\\get_rit", "\\set_rit", "9.1", "9.2", "9.3"},
        {"XIT", "\\get_xit", "\\set_xit", "9.4", "9.5", nullptr},
    };
    for (const auto& t : kTests) {
        const QString cmdGet = QLatin1String(t.cmdGet);
        const QString cmdSet = QLatin1String(t.cmdSet);
        const QString nm     = QLatin1String(t.name);

        QStringList lines = c.send(cmdGet);
        const QString val = c.field(lines, nm);
        r.check(QStringLiteral("%1  %2 returns numeric offset in Hz")
                    .arg(QLatin1String(t.getId), cmdGet),
                c.ok(lines) && isInt(val), val);

        lines = c.send(cmdSet + QStringLiteral(" 500"));
        r.check(QStringLiteral("%1  %2 500 returns RPRT 0")
                    .arg(QLatin1String(t.setId), cmdSet),
                c.ok(lines));

        // Read-back: confirm 500 Hz was applied
        QThread::msleep(100);
        lines = c.send(cmdGet);
        const QString setConf = c.field(lines, nm);
        r.check(QStringLiteral("%1b %2 confirms 500 Hz after set")
                    .arg(QLatin1String(t.setId), nm),
                c.ok(lines) && isInt(setConf) && setConf.toLongLong() == 500, setConf);

        if (t.clrId) {
            lines = c.send(cmdSet + QStringLiteral(" 0"));
            r.check(QStringLiteral("%1  %2 0 clears %3")
                        .arg(QLatin1String(t.clrId), cmdSet, nm),
                    c.ok(lines));

            // Read-back: confirm offset returned to 0
            QThread::msleep(100);
            lines = c.send(cmdGet);
            const QString clrConf = c.field(lines, nm);
            r.check(QStringLiteral("%1b %2 confirms 0 Hz after clear")
                        .arg(QLatin1String(t.clrId), nm),
                    c.ok(lines) && isInt(clrConf) && clrConf.toLongLong() == 0, clrConf);
        } else {
            c.send(cmdSet + QStringLiteral(" 0"));
        }
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Section 10 — Antenna
// ═════════════════════════════════════════════════════════════════════════════

void section10(RigctlClient& c, Runner& r)
{
    r.section(QStringLiteral("Section 10 — Antenna"));

    QStringList lines = c.send(QStringLiteral("\\get_ant"));
    const QString ant = c.field(lines, QStringLiteral("Ant"));
    r.check(QStringLiteral("10.1 get_ant returns antenna mask (1=ANT1, 2=ANT2, 4=ANT3, 8=XVTR)"),
            c.ok(lines) && isInt(ant) && ant.toInt() > 0, ant);

    const QString origAnt = ant;
    const QString target = (ant == QLatin1String("2"))
        ? QStringLiteral("1") : QStringLiteral("2");

    lines = c.send(QStringLiteral("\\set_ant ") + target);
    r.check(QStringLiteral("10.2 set_ant %1 returns RPRT 0").arg(target), c.ok(lines));
    QThread::msleep(100);

    lines = c.send(QStringLiteral("\\get_ant"));
    const QString newAnt = c.field(lines, QStringLiteral("Ant"));
    r.check(QStringLiteral("10.3 get_ant confirms switch to antenna %1").arg(target),
            c.ok(lines) && newAnt == target, newAnt);

    // Restore to ANT1 (dummy load) and confirm
    QThread::msleep(100);
    c.send(QStringLiteral("\\set_ant 1"));
    QThread::msleep(100);
    lines = c.send(QStringLiteral("\\get_ant"));
    const QString restored = c.field(lines, QStringLiteral("Ant"));
    r.check(QStringLiteral("10.4 set_ant 1 restores to ANT1"),
            c.ok(lines) && restored == QLatin1String("1"), restored);

    // 10.5  set_ant ? capability probe
    lines = c.send(QStringLiteral("\\set_ant ?"));
    const bool hasAnts = std::any_of(lines.cbegin(), lines.cend(),
                              [](const QString& l){ return l.contains(QLatin1String("Ants")); });
    r.check(QStringLiteral("10.5 set_ant ? returns Ants capability list"),
            c.ok(lines) && hasAnts, lines.join(QStringLiteral(" | ")));
}

// ═════════════════════════════════════════════════════════════════════════════
// Section 11 — CW / Morse  (optional — requires PTT and CW mode)
// ═════════════════════════════════════════════════════════════════════════════

void section11Cw(RigctlClient& c, Runner& r)
{
    r.section(QStringLiteral("Section 11 — CW / Morse  ⚠  TX ACTIVE"));

    // Power safety: read current power, drop to 0.01 (≈ 1 W) and confirm before keying
    QStringList lines = c.send(QStringLiteral("\\get_level RFPOWER"));
    const QString origRfpower = c.field(lines, QStringLiteral("RFPOWER"));
    r.check(QStringLiteral("11.0  read RFPOWER before CW tests"),
            c.ok(lines) && isFloat(origRfpower), origRfpower);

    lines = c.send(QStringLiteral("\\set_level RFPOWER 0.01"));
    r.check(QStringLiteral("11.0b set RFPOWER 0.01 (≈ 1 W) returns RPRT 0"), c.ok(lines));
    QString safePwr;
    {
        QElapsedTimer t; t.start();
        do {
            lines = c.send(QStringLiteral("\\get_level RFPOWER"));
            safePwr = c.field(lines, QStringLiteral("RFPOWER"));
            if ((isFloat(safePwr) && safePwr.toDouble() <= 0.03) || t.elapsed() >= 1000) break;
            QThread::msleep(100);
        } while (true);
    }
    r.check(QStringLiteral("11.0c confirm RFPOWER ≤ 0.03 before keying"),
            c.ok(lines) && isFloat(safePwr) && safePwr.toDouble() <= 0.03, safePwr);

    // CWX keys TX based on the TX slice, not the RX slice.  If section 5
    // left the TX slice as VFOB, cwx send would key the wrong slice (USB mode)
    // and CW would never transmit.  Force split off and poll until confirmed.
    c.send(QStringLiteral("\\set_split_vfo 0 VFOA"));
    {
        QElapsedTimer t; t.start();
        do {
            lines = c.send(QStringLiteral("\\get_split_vfo"));
            if (c.field(lines, QStringLiteral("Split")) == QStringLiteral("0"))
                break;
            c.send(QStringLiteral("\\set_split_vfo 0 VFOA"));  // retry if TX still on VFOB
            QThread::msleep(300);
        } while (t.elapsed() < 2000);
    }

    // CW mode required: cwx send only keys TX when slice is in a CW mode.
    // Save current mode and passband so we can restore after the section.
    const QStringList modeLines = c.send(QStringLiteral("\\get_mode"));
    const QString origCwMode = c.field(modeLines, QStringLiteral("Mode"));
    const QString origCwPb   = c.field(modeLines, QStringLiteral("Passband"));
    lines = c.send(QStringLiteral("\\set_mode CW 500"));
    r.check(QStringLiteral("11.0d set_mode CW 500 returns RPRT 0"), c.ok(lines));
    // Poll until mode confirms as CW — don't rely on a fixed sleep
    QString modeConfirm;
    {
        QElapsedTimer t; t.start();
        do {
            const QStringList mc = c.send(QStringLiteral("\\get_mode"));
            modeConfirm = c.field(mc, QStringLiteral("Mode"));
            if (modeConfirm == QLatin1String("CW") || t.elapsed() >= 2000) break;
            QThread::msleep(100);
        } while (true);
    }
    r.check(QStringLiteral("11.0d2 mode confirmed CW before CWX queue starts"),
            modeConfirm == QLatin1String("CW"), modeConfirm);

    // Full break-in required: without FBKIN, cwx send queues CW but the radio
    // waits for manual PTT instead of keying automatically.
    lines = c.send(QStringLiteral("\\get_func FBKIN"));
    const QString origFbkin = c.field(lines, QStringLiteral("FBKIN"));
    r.check(QStringLiteral("11.0e get_func FBKIN returns 0 or 1"),
            c.ok(lines) && (origFbkin == QLatin1String("0") || origFbkin == QLatin1String("1")),
            origFbkin);
    lines = c.send(QStringLiteral("\\set_func FBKIN 1"));
    r.check(QStringLiteral("11.0f set_func FBKIN 1 (enable full break-in) returns RPRT 0"),
            c.ok(lines));

    // 11.1  send_morse with inline text
    lines = c.send(QStringLiteral("\\send_morse DE TEST"));
    r.check(QStringLiteral("11.1 send_morse \"DE TEST\" returns RPRT 0"), c.ok(lines));
    QThread::msleep(3000);  // "DE TEST" at 20 WPM takes ~2.2 s; allow full message to play

    // 11.2  stop_morse
    lines = c.send(QStringLiteral("\\stop_morse"));
    r.check(QStringLiteral("11.2 stop_morse clears CWX buffer (RPRT 0)"), c.ok(lines));
    QThread::msleep(300);

    // 11.3  two-line form: bare 'b' then text on next line (Not1MM / some clients).
    //        Both lines are written in a single write to guarantee ordering.
    c.writeBytes("b\nTEST\n");
    const QStringList resp = c.readUntilRprt(8);
    r.check(QStringLiteral("11.3 two-line morse form (bare b + text on next line) returns RPRT 0"),
            std::any_of(resp.cbegin(), resp.cend(),
                        [](const QString& l){ return l == QLatin1String("RPRT 0"); }),
            resp.join(QStringLiteral(" | ")));

    QThread::msleep(500);
    c.send(QStringLiteral("\\stop_morse"));

    // 11.4  Safety: watch 2 s — CW TX-tail may keep PTT on after stop_morse
    {
        QElapsedTimer safeTimer; safeTimer.start();
        bool sawTxOn = false;
        c.send(QStringLiteral("\\set_ptt 0"));
        do {
            QThread::msleep(250);
            lines = c.send(QStringLiteral("\\get_ptt"));
            const QString pttSafe = c.field(lines, QStringLiteral("PTT"));
            if (pttSafe == QLatin1String("1")) {
                sawTxOn = true;
                c.send(QStringLiteral("\\set_ptt 0"));
                c.send(QStringLiteral("\\stop_morse"));
            }
        } while (safeTimer.elapsed() < 2000);
        if (sawTxOn)
            qWarning("11.4: PTT re-asserted during 2-s safety watch — forced set_ptt 0; possible firmware bug");
        r.check(QStringLiteral("11.4 safety: PTT confirmed 0 after 2-s watch"),
                c.ok(lines) && c.field(lines, QStringLiteral("PTT")) == QLatin1String("0"),
                c.field(lines, QStringLiteral("PTT")));
    }

    // Restore break-in state and mode
    if (origFbkin == QLatin1String("0") || origFbkin == QLatin1String("1"))
        c.send(QStringLiteral("\\set_func FBKIN ") + origFbkin);
    if (!origCwMode.isEmpty()) {
        const QString pb = origCwPb.isEmpty() ? QStringLiteral("0") : origCwPb;
        c.send(QStringLiteral("\\set_mode %1 %2").arg(origCwMode, pb));
        QThread::msleep(100);
    }

    // Restore power and confirm
    if (isFloat(origRfpower)) {
        lines = c.send(QStringLiteral("\\set_level RFPOWER ") + origRfpower);
        r.check(QStringLiteral("11.5  restore RFPOWER returns RPRT 0"), c.ok(lines));
        QString pwrRestored;
        {
            QElapsedTimer t; t.start();
            do {
                lines = c.send(QStringLiteral("\\get_level RFPOWER"));
                pwrRestored = c.field(lines, QStringLiteral("RFPOWER"));
                if ((isFloat(pwrRestored) && qAbs(pwrRestored.toDouble() - origRfpower.toDouble()) < 0.02)
                        || t.elapsed() >= 1000) break;
                QThread::msleep(100);
            } while (true);
        }
        r.check(QStringLiteral("11.5b confirm RFPOWER restored"),
                c.ok(lines) && isFloat(pwrRestored)
                    && qAbs(pwrRestored.toDouble() - origRfpower.toDouble()) < 0.02,
                pwrRestored);
    }
}

void section11Skip(Runner& r)
{
    r.section(QStringLiteral("Section 11 — CW / Morse  (skipped — pass --cw to enable)"));
    for (const auto* name : {
             "11.0 read RFPOWER", "11.0b set RFPOWER 0.01", "11.0c confirm RFPOWER",
             "11.0d set_mode CW 500", "11.0d2 mode confirmed CW",
             "11.0e get_func FBKIN", "11.0f set_func FBKIN 1",
             "11.1 send_morse inline", "11.2 stop_morse",
             "11.3 two-line form",     "11.4 safety: PTT 0 after 2-s watch",
             "11.5 restore RFPOWER",   "11.5b confirm RFPOWER restored",
         })
        r.skip(QLatin1String(name), QStringLiteral("--cw not set"));
}

// ═════════════════════════════════════════════════════════════════════════════
// Section 12 — Power Conversion
// ═════════════════════════════════════════════════════════════════════════════

void section12(RigctlClient& c, Runner& r)
{
    r.section(QStringLiteral("Section 12 — Power Conversion"));

    // 12.1  power2mW: ratio 0.5 at 14 MHz USB → milliwatts
    QStringList lines = c.send(QStringLiteral("\\power2mW 14.0 0.5 USB"));
    const QString mwStr = c.field(lines, QStringLiteral("Power mW"));
    const double mw = mwStr.toDouble();
    r.check(QStringLiteral("12.1 power2mW 14.0 0.5 USB returns positive mW value"),
            c.ok(lines) && isFloat(mwStr) && mw > 0, mwStr);

    // 12.2  mW2power: round-trip should recover ≈ 0.5
    if (isFloat(mwStr)) {
        QStringList lines2 = c.send(QStringLiteral("\\mW2power 14.0 %1 USB").arg(mwStr));
        const QString ratioStr = c.field(lines2, QStringLiteral("Power"));
        const double ratio = ratioStr.toDouble();
        r.check(QStringLiteral("12.2 mW2power round-trip returns ratio ≈ 0.5"),
                c.ok(lines2) && isFloat(ratioStr) && std::abs(ratio - 0.5) < 0.02,
                ratioStr);
    } else {
        r.skip(QStringLiteral("12.2 mW2power round-trip"),
               QStringLiteral("12.1 power2mW failed"));
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Section 13 — Non-Extended Mode  (bare protocol used by WSJT-X, fldigi, etc.)
//
// m_extended is a session-level flag: once '+' is sent on a connection it stays
// true for the life of that connection.  This section opens a fresh connection
// that never sends '+', exactly as WSJT-X or fldigi would connect.
// ═════════════════════════════════════════════════════════════════════════════

void section13(const QString& host, quint16 port, int timeout,
               Runner& r, qint64 origFreq, const QString& origMode, int origPb)
{
    r.section(QStringLiteral("Section 13 — Non-Extended Mode  (bare protocol, fresh connection)"));

    RigctlClient c(timeout);
    if (!c.connectToServer(host, port)) {
        for (int i = 1; i <= 10; ++i)
            r.skip(QStringLiteral("13.%1").arg(i), QStringLiteral("could not connect"));
        return;
    }

    // 13.1  get_freq → single bare integer, no label, no RPRT
    QStringList raw = c.sendRaw(QStringLiteral("\\get_freq"), 1);
    const QString freqStr = raw.value(0);
    r.check(QStringLiteral("13.1  get_freq (bare) returns single integer line, no label or RPRT"),
            isInt(freqStr) && !freqStr.startsWith(QLatin1String("Frequency")),
            raw.join(QStringLiteral(" | ")));

    // 13.2  get_mode → two bare lines: mode string then passband integer, no RPRT
    raw = c.sendRaw(QStringLiteral("\\get_mode"), 2);
    const QString modeBare = raw.value(0);
    const QString pbBare   = raw.value(1);
    r.check(QStringLiteral("13.2  get_mode (bare) returns mode then passband, no labels or RPRT"),
            kKnownModes.contains(modeBare) && isInt(pbBare),
            raw.join(QStringLiteral(" | ")));

    // 13.3  set_freq → single RPRT 0 line
    raw = c.sendRaw(QStringLiteral("\\set_freq %1").arg(origFreq + 500), 1);
    r.check(QStringLiteral("13.3  set_freq (bare) returns single RPRT 0"),
            raw == QStringList{QStringLiteral("RPRT 0")},
            raw.join(QStringLiteral(" | ")));

    // 13.4  set_mode → single RPRT 0 line
    raw = c.sendRaw(QStringLiteral("\\set_mode USB 2700"), 1);
    r.check(QStringLiteral("13.4  set_mode (bare) returns single RPRT 0"),
            raw == QStringList{QStringLiteral("RPRT 0")},
            raw.join(QStringLiteral(" | ")));

    // 13.5  get_ptt → single bare 0 or 1, no RPRT
    raw = c.sendRaw(QStringLiteral("\\get_ptt"), 1);
    const QString pttBare = raw.value(0);
    r.check(QStringLiteral("13.5  get_ptt (bare) returns single 0 or 1, no label or RPRT"),
            pttBare == QLatin1String("0") || pttBare == QLatin1String("1"),
            raw.join(QStringLiteral(" | ")));

    // 13.6  get_vfo → single bare VFO name, no RPRT
    static const QSet<QString> kBareVfos = {
        QStringLiteral("VFOA"), QStringLiteral("VFOB"),
        QStringLiteral("Main"), QStringLiteral("Sub"), QStringLiteral("currVFO"),
    };
    raw = c.sendRaw(QStringLiteral("\\get_vfo"), 1);
    r.check(QStringLiteral("13.6  get_vfo (bare) returns single VFO name, no label or RPRT"),
            kBareVfos.contains(raw.value(0)),
            raw.join(QStringLiteral(" | ")));

    // 13.7  short form 'f' → single bare integer
    raw = c.sendRaw(QStringLiteral("f"), 1);
    r.check(QStringLiteral("13.7  short form \"f\" (bare) returns single integer line"),
            isInt(raw.value(0)), raw.join(QStringLiteral(" | ")));

    // 13.8  short form 'F' set → single RPRT 0
    raw = c.sendRaw(QStringLiteral("F %1").arg(origFreq), 1);
    r.check(QStringLiteral("13.8  short form \"F\" (bare) returns single RPRT 0"),
            raw == QStringList{QStringLiteral("RPRT 0")},
            raw.join(QStringLiteral(" | ")));

    // 13.9  unknown command → RPRT -4 (same in both modes)
    raw = c.sendRaw(QStringLiteral("\\nonexistent_bare_xyz"), 1);
    r.check(QStringLiteral("13.9  unknown command (bare) returns RPRT -4"),
            raw == QStringList{QStringLiteral("RPRT -4")},
            raw.join(QStringLiteral(" | ")));

    // 13.10  WSJT-X/Hamlib startup probe: value plus status terminator.
    // Hamlib's NET rigctl backend waits for RPRT after this long-form getter;
    // if the terminator is missing, startup CAT work queues behind a timeout.
    raw = c.sendRaw(QStringLiteral("\\get_lock_mode"), 2);
    r.check(QStringLiteral("13.10 get_lock_mode (bare) returns value and RPRT 0"),
            raw == QStringList{QStringLiteral("0"), QStringLiteral("RPRT 0")},
            raw.join(QStringLiteral(" | ")));

    // 13.11–13.20  Hamlib protocol-compliance sweep for the remaining bare-mode
    // getters that were missing the trailing RPRT terminator (issue #3120).
    // Mirrors 13.10 / #3115 for every long-form bare-branch getter listed in
    // the audit.  Hamlib's reference NET rigctl appends RPRT N on every
    // response in both bare and extended modes; clients other than WSJT-X
    // (fldigi, JTDX, hamlib-cli, …) may stall waiting for it.
    raw = c.sendRaw(QStringLiteral("\\chk_vfo"), 2);
    r.check(QStringLiteral("13.11 chk_vfo (bare) returns value and RPRT 0"),
            raw == QStringList{QStringLiteral("0"), QStringLiteral("RPRT 0")},
            raw.join(QStringLiteral(" | ")));

    raw = c.sendRaw(QStringLiteral("\\get_powerstat"), 2);
    r.check(QStringLiteral("13.12 get_powerstat (bare) returns value and RPRT 0"),
            raw == QStringList{QStringLiteral("1"), QStringLiteral("RPRT 0")},
            raw.join(QStringLiteral(" | ")));

    raw = c.sendRaw(QStringLiteral("\\hamlib_version"), 2);
    r.check(QStringLiteral("13.13 hamlib_version (bare) returns value and RPRT 0"),
            raw == QStringList{QStringLiteral("AetherSDR"), QStringLiteral("RPRT 0")},
            raw.join(QStringLiteral(" | ")));

    raw = c.sendRaw(QStringLiteral("\\get_vfo_list"), 2);
    r.check(QStringLiteral("13.14 get_vfo_list (bare) returns value and RPRT 0"),
            raw == QStringList{QStringLiteral("VFOA VFOB"), QStringLiteral("RPRT 0")},
            raw.join(QStringLiteral(" | ")));

    raw = c.sendRaw(QStringLiteral("\\get_modes"), 2);
    r.check(QStringLiteral("13.15 get_modes (bare) returns value and RPRT 0"),
            raw == QStringList{QStringLiteral("USB LSB CW CWR AM AMS FM PKTUSB PKTLSB RTTY"),
                               QStringLiteral("RPRT 0")},
            raw.join(QStringLiteral(" | ")));

    raw = c.sendRaw(QStringLiteral("\\get_rptr_shift"), 2);
    r.check(QStringLiteral("13.16 get_rptr_shift (bare) returns value and RPRT 0"),
            raw == QStringList{QStringLiteral("+"), QStringLiteral("RPRT 0")},
            raw.join(QStringLiteral(" | ")));

    raw = c.sendRaw(QStringLiteral("\\get_rptr_offs"), 2);
    r.check(QStringLiteral("13.17 get_rptr_offs (bare) returns value and RPRT 0"),
            raw == QStringList{QStringLiteral("0"), QStringLiteral("RPRT 0")},
            raw.join(QStringLiteral(" | ")));

    raw = c.sendRaw(QStringLiteral("\\get_ctcss_tone"), 2);
    r.check(QStringLiteral("13.18 get_ctcss_tone (bare) returns value and RPRT 0"),
            raw == QStringList{QStringLiteral("0"), QStringLiteral("RPRT 0")},
            raw.join(QStringLiteral(" | ")));

    raw = c.sendRaw(QStringLiteral("\\get_dcs_code"), 2);
    r.check(QStringLiteral("13.19 get_dcs_code (bare) returns value and RPRT 0"),
            raw == QStringList{QStringLiteral("0"), QStringLiteral("RPRT 0")},
            raw.join(QStringLiteral(" | ")));

    // get_split_freq_mode has three value lines (freq, mode, passband) plus the
    // RPRT 0 terminator — 4 lines total in bare mode.  Value lines depend on
    // TX-slice state, so only assert shape: integer freq, known mode, integer
    // passband, RPRT 0.  If no TX slice exists the response collapses to a
    // single "RPRT -1" line, which we accept as the alternate valid shape.
    raw = c.sendRaw(QStringLiteral("\\get_split_freq_mode"), 4);
    const bool sfmOk =
        (raw.size() == 4
         && isInt(raw.value(0))
         && kKnownModes.contains(raw.value(1))
         && isInt(raw.value(2))
         && raw.value(3) == QLatin1String("RPRT 0"))
        || (raw.size() == 1 && raw.value(0) == QLatin1String("RPRT -1"));
    r.check(QStringLiteral("13.20 get_split_freq_mode (bare) ends with RPRT terminator"),
            sfmOk, raw.join(QStringLiteral(" | ")));

    // Best-effort restore
    c.sendRaw(QStringLiteral("\\set_freq %1").arg(origFreq), 1);
    c.sendRaw(QStringLiteral("\\set_mode %1 %2").arg(origMode).arg(origPb), 1);
    c.close();
}

// ═════════════════════════════════════════════════════════════════════════════
// Section 14 — Short-Form Character Mapping  (Hamlib source audit regression)
// ═════════════════════════════════════════════════════════════════════════════

void section14(RigctlClient& c, Runner& r)
{
    r.section(QStringLiteral("Section 14 — Short-Form Character Mapping (Hamlib audit regression)"));

    // 14.1  'n' → get_ts  (was misassigned to get_dcd before Hamlib source audit)
    QStringList lines = c.send(QStringLiteral("n"));
    const QString tsVal = c.field(lines, QStringLiteral("Tuning Step"));
    r.check(QStringLiteral("14.1 'n' → get_ts returns Tuning Step field"),
            c.ok(lines) && isInt(tsVal), tsVal);

    // 14.2 / 14.2b  'G UP' → vfo_op UP  (was set_ts before audit; must move frequency)
    const qint64 freqBefore14 =
        c.field(c.send(QStringLiteral("\\get_freq")), QStringLiteral("Frequency")).toLongLong();
    lines = c.send(QStringLiteral("G UP"));
    r.check(QStringLiteral("14.2 'G UP' → vfo_op UP returns RPRT 0"), c.ok(lines));
    qint64 freqAfterUp = freqBefore14;
    {
        QElapsedTimer t; t.start();
        do {
            freqAfterUp = c.field(c.send(QStringLiteral("\\get_freq")),
                                  QStringLiteral("Frequency")).toLongLong();
            if (freqAfterUp != freqBefore14 || t.elapsed() >= 1000) break;
            QThread::msleep(50);
        } while (true);
    }
    r.check(QStringLiteral("14.2b 'G UP' moved frequency upward by one tuning step"),
            freqAfterUp > freqBefore14,
            QStringLiteral("before=%1 after=%2").arg(freqBefore14).arg(freqAfterUp));
    c.send(QStringLiteral("G DOWN"));
    QThread::msleep(100);

    // 14.3  'a' → get_trn  (not get_ant — 'a' and 'y' were transposed before audit)
    lines = c.send(QStringLiteral("a"));
    const QString trnVal = c.field(lines, QStringLiteral("Transceive"));
    r.check(QStringLiteral("14.3 'a' → get_trn returns Transceive field"),
            c.ok(lines) && !trnVal.isNull(), trnVal);

    // 14.4  'y' → get_ant  (was missing before audit)
    lines = c.send(QStringLiteral("y"));
    const QString antVal = c.field(lines, QStringLiteral("Ant"));
    r.check(QStringLiteral("14.4 'y' → get_ant returns Ant field"),
            c.ok(lines) && isInt(antVal), antVal);

    // 14.5 / 14.6  'j' and 'z' → get_rit / get_xit  (verify not disturbed by rewrite)
    lines = c.send(QStringLiteral("j"));
    const QString ritVal = c.field(lines, QStringLiteral("RIT"));
    r.check(QStringLiteral("14.5 'j' → get_rit returns RIT field"),
            c.ok(lines) && isInt(ritVal), ritVal);

    lines = c.send(QStringLiteral("z"));
    const QString xitVal = c.field(lines, QStringLiteral("XIT"));
    r.check(QStringLiteral("14.6 'z' → get_xit returns XIT field"),
            c.ok(lines) && isInt(xitVal), xitVal);

    // 14.7  'k' → get_split_freq_mode  (was missing before audit)
    lines = c.send(QStringLiteral("k"));
    r.check(QStringLiteral("14.7 'k' → get_split_freq_mode returns RPRT 0"),
            c.ok(lines), lines.join(QStringLiteral(" | ")));
}

// ═════════════════════════════════════════════════════════════════════════════
// Section 15 — FM / Repeater Stubs
// ═════════════════════════════════════════════════════════════════════════════

void section15(RigctlClient& c, Runner& r)
{
    r.section(QStringLiteral("Section 15 — FM / Repeater Stubs"));

    // 15.1 / 15.2  get/set_rptr_shift
    QStringList lines = c.send(QStringLiteral("\\get_rptr_shift"));
    const QString shift = c.field(lines, QStringLiteral("Rptr Shift"));
    r.check(QStringLiteral("15.1 get_rptr_shift returns Rptr Shift: +"),
            c.ok(lines) && shift == QLatin1String("+"), shift);
    lines = c.send(QStringLiteral("\\set_rptr_shift +"));
    r.check(QStringLiteral("15.2 set_rptr_shift + returns RPRT 0"), c.ok(lines));

    // 15.3 / 15.4  get/set_rptr_offs
    lines = c.send(QStringLiteral("\\get_rptr_offs"));
    const QString offs = c.field(lines, QStringLiteral("Rptr Offset"));
    r.check(QStringLiteral("15.3 get_rptr_offs returns Rptr Offset: 0"),
            c.ok(lines) && offs == QLatin1String("0"), offs);
    lines = c.send(QStringLiteral("\\set_rptr_offs 0"));
    r.check(QStringLiteral("15.4 set_rptr_offs 0 returns RPRT 0"), c.ok(lines));

    // 15.5 / 15.6  get/set_ctcss_tone
    lines = c.send(QStringLiteral("\\get_ctcss_tone"));
    const QString ctcss = c.field(lines, QStringLiteral("CTCSS Tone"));
    r.check(QStringLiteral("15.5 get_ctcss_tone returns CTCSS Tone: 0"),
            c.ok(lines) && ctcss == QLatin1String("0"), ctcss);
    lines = c.send(QStringLiteral("\\set_ctcss_tone 0"));
    r.check(QStringLiteral("15.6 set_ctcss_tone 0 returns RPRT 0"), c.ok(lines));

    // 15.7 / 15.8  get/set_dcs_code
    lines = c.send(QStringLiteral("\\get_dcs_code"));
    const QString dcs = c.field(lines, QStringLiteral("DCS Code"));
    r.check(QStringLiteral("15.7 get_dcs_code returns DCS Code: 0"),
            c.ok(lines) && dcs == QLatin1String("0"), dcs);
    lines = c.send(QStringLiteral("\\set_dcs_code 0"));
    r.check(QStringLiteral("15.8 set_dcs_code 0 returns RPRT 0"), c.ok(lines));
}

// ═════════════════════════════════════════════════════════════════════════════
// Edge cases
// ═════════════════════════════════════════════════════════════════════════════

void sectionEdge(RigctlClient& c, Runner& r)
{
    r.section(QStringLiteral("Edge Cases"));

    // E1  Unknown command → RPRT -4
    QStringList lines = c.send(QStringLiteral("\\nonexistent_command_xyz"));
    r.check(QStringLiteral("E1  unknown command returns RPRT -4"),
            c.rprt(lines) == -4, lines.join(QStringLiteral(" | ")));

    // E2  Batch commands via ';' — extended mode: get_freq=3 lines + get_mode=4 lines = 7 total
    QStringList raw = c.sendRaw(QStringLiteral("\\get_freq;\\get_mode"), 7);
    r.check(QStringLiteral("E2  batch commands (;-separated) return multiple values"),
            !c.field(raw, QStringLiteral("Frequency")).isEmpty()
                && !c.field(raw, QStringLiteral("Mode")).isEmpty(),
            raw.join(QStringLiteral(" | ")));

    // E3  chk_vfo → 0 (VFO mode not used)
    lines = c.send(QStringLiteral("\\chk_vfo"));
    r.check(QStringLiteral("E3  chk_vfo returns 0 (VFO mode disabled)"),
            c.ok(lines) && c.field(lines, QStringLiteral("VFO Mode")) == QLatin1String("0"),
            lines.join(QStringLiteral(" | ")));

    // E4  get_trn → transceive always off
    lines = c.send(QStringLiteral("\\get_trn"));
    const QString trn = c.field(lines, QStringLiteral("Transceive"));
    r.check(QStringLiteral("E4  get_trn returns Transceive: 0 (unsupported)"),
            c.ok(lines) && trn == QLatin1String("0"), trn);

    // E5  set_trn 1 → silently accepted
    lines = c.send(QStringLiteral("\\set_trn 1"));
    r.check(QStringLiteral("E5  set_trn 1 accepted silently (RPRT 0)"), c.ok(lines));

    // E6  send_morse smoke test — verify protocol accepts it without --cw flag
    //     Does not verify radio keying; just that the command parses and queues
    lines = c.send(QStringLiteral("\\send_morse TEST"));
    r.check(QStringLiteral("E6  send_morse \"TEST\" accepted (RPRT 0, no --cw required)"),
            c.ok(lines), lines.join(QStringLiteral(" | ")));
    c.send(QStringLiteral("\\stop_morse"));
}

// ═════════════════════════════════════════════════════════════════════════════
// Section 16 — PTY round-trip  (Unix only; skips if device cannot be opened)
// ═════════════════════════════════════════════════════════════════════════════

#if defined(Q_OS_UNIX)
class PtyClient
{
public:
    explicit PtyClient(int timeoutMs = 3000) : m_timeout(timeoutMs) {}
    ~PtyClient() { if (m_fd >= 0) ::close(m_fd); }

    bool openDevice(const QString& path)
    {
        errno = 0;
        m_fd = ::open(path.toLocal8Bit().constData(), O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (m_fd < 0) return false;
        struct termios tio;
        if (::tcgetattr(m_fd, &tio) == 0) {
            ::cfmakeraw(&tio);
            tio.c_cc[VMIN]  = 0;
            tio.c_cc[VTIME] = 0;
            ::tcsetattr(m_fd, TCSANOW, &tio);
        }
        return true;
    }

    void send(const QString& cmd)
    {
        QByteArray ba = ('+' + cmd + QLatin1Char('\n')).toUtf8();
        ::write(m_fd, ba.constData(), ba.size());
    }

    QStringList readUntilRprt()
    {
        QStringList result;
        QByteArray lineBuf;
        QElapsedTimer timer; timer.start();
        while (timer.elapsed() < m_timeout) {
            char ch;
            if (::read(m_fd, &ch, 1) == 1) {
                if (ch == '\n') {
                    QString line = QString::fromUtf8(lineBuf).trimmed();
                    lineBuf.clear();
                    if (!line.isEmpty()) {
                        result.append(line);
                        if (line.startsWith(QLatin1String("RPRT"))) return result;
                    }
                } else if (ch != '\r') {
                    lineBuf.append(ch);
                }
            } else {
                QThread::msleep(10);
            }
        }
        return result;
    }

    QStringList query(const QString& cmd) { send(cmd); return readUntilRprt(); }

    static QString field(const QStringList& lines, const QString& label)
    {
        const QString prefix = label + QLatin1String(": ");
        for (const QString& l : lines) {
            if (l.startsWith(prefix)) return l.mid(prefix.size());
        }
        return {};
    }

    static bool ok(const QStringList& lines)
    {
        return lines.contains(QStringLiteral("RPRT 0"));
    }

private:
    int m_fd{-1};
    int m_timeout;
};

void sectionPty(Runner& r, const QString& ptyPath)
{
    r.section(QStringLiteral("Section 16 — PTY round-trip  (skips if device cannot be opened)"));

    PtyClient p;
    if (!p.openDevice(ptyPath)) {
        const QString why = QStringLiteral("cannot open %1 (%2)")
                                .arg(ptyPath, QString::fromLocal8Bit(::strerror(errno)));
        for (const char* name : { "16.1 get_freq via PTY", "16.2 get_mode via PTY",
                                   "16.3 get_info via PTY" })
            r.skip(QString::fromLatin1(name), why);
        return;
    }

    QStringList lines = p.query(QStringLiteral("\\get_freq"));
    const QString freq = PtyClient::field(lines, QStringLiteral("Frequency"));
    r.check(QStringLiteral("16.1 get_freq via PTY → Frequency: <integer>"),
            PtyClient::ok(lines) && isInt(freq),
            freq.isEmpty() ? QStringLiteral("(no response)") : freq);

    lines = p.query(QStringLiteral("\\get_mode"));
    const QString mode = PtyClient::field(lines, QStringLiteral("Mode"));
    r.check(QStringLiteral("16.2 get_mode via PTY → Mode: <string>"),
            PtyClient::ok(lines) && !mode.isEmpty(),
            mode.isEmpty() ? QStringLiteral("(no response)") : mode);

    lines = p.query(QStringLiteral("\\get_rig_info"));
    const QString info = PtyClient::field(lines, QStringLiteral("Info"));
    r.check(QStringLiteral("16.3 get_rig_info via PTY → Info: <string>"),
            PtyClient::ok(lines) && !info.isEmpty(),
            info.isEmpty() ? QStringLiteral("(no response)") : info);
}
#else
void sectionPty(Runner& r, const QString&)
{
    r.section(QStringLiteral("Section 16 — PTY round-trip  (skipped — Unix only)"));
    for (const char* name : { "16.1 get_freq via PTY", "16.2 get_mode via PTY",
                               "16.3 get_info via PTY" })
        r.skip(QString::fromLatin1(name), QStringLiteral("not Unix"));
}
#endif

} // namespace

// ═════════════════════════════════════════════════════════════════════════════
// main
// ═════════════════════════════════════════════════════════════════════════════

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("rigctld_test"));

#if defined(Q_OS_UNIX)
    g_tty = isatty(STDOUT_FILENO);
#endif

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("AetherSDR rigctld protocol integration test.\n"
                        "Requires a running AetherSDR instance with rigctld enabled.\n"
                        "\nPTT tests (section 6) and CW/Morse tests (section 11) are\n"
                        "disabled by default — enable only when a dummy load or antenna\n"
                        "is connected."));
    parser.addHelpOption();

    QCommandLineOption hostOpt({QStringLiteral("host")},
        QStringLiteral("rigctld host (default: localhost)"),
        QStringLiteral("HOST"), QStringLiteral("localhost"));
    QCommandLineOption portOpt({QStringLiteral("port")},
        QStringLiteral("rigctld TCP port (default: 4532)"),
        QStringLiteral("PORT"), QStringLiteral("4532"));
    QCommandLineOption timeoutOpt({QStringLiteral("timeout")},
        QStringLiteral("socket read timeout in milliseconds (default: 3000)"),
        QStringLiteral("MS"), QStringLiteral("3000"));
    QCommandLineOption pttOpt({QStringLiteral("ptt")},
        QStringLiteral("enable PTT tests (section 6) — requires dummy load or antenna"));
    QCommandLineOption cwOpt({QStringLiteral("cw")},
        QStringLiteral("enable CW/Morse tests (section 11) — requires --ptt and CW mode"));
    const QString defaultPty0 = defaultPtyPath(0);
    QCommandLineOption ptyOpt({QStringLiteral("pty")},
        QStringLiteral("PTY device path for section 16 (default: %1)").arg(defaultPty0),
        QStringLiteral("PATH"), defaultPty0);

    parser.addOption(hostOpt);
    parser.addOption(portOpt);
    parser.addOption(timeoutOpt);
    parser.addOption(pttOpt);
    parser.addOption(cwOpt);
    parser.addOption(ptyOpt);
    parser.process(app);

    const QString  host    = parser.value(hostOpt);
    const quint16  port    = static_cast<quint16>(parser.value(portOpt).toUShort());
    const int      timeout = parser.value(timeoutOpt).toInt();
    const bool     doPtt   = parser.isSet(pttOpt);
    const bool     doCw    = parser.isSet(cwOpt);
    const QString  ptyPath = parser.value(ptyOpt);

    std::cout << '\n' << bold(QStringLiteral("AetherSDR rigctld Test Suite")).toStdString() << '\n'
              << "Connecting to " << host.toStdString() << ':' << port << " ...\n";

    RigctlClient c(timeout);
    if (!c.connectToServer(host, port))
        return 1;

    std::cout << green(QStringLiteral("Connected.")).toStdString() << '\n';
    if (doPtt)
        std::cout << yellow(QStringLiteral(
            "⚠  PTT tests enabled — ensure a dummy load or antenna is connected"))
            .toStdString() << '\n';
    if (doCw)
        std::cout << yellow(QStringLiteral(
            "⚠  CW tests enabled — ensure radio is in CW mode"))
            .toStdString() << '\n';

    Runner r;

    // Save radio state before tests so we can restore on exit.
    qint64  origFreq = 14074000;
    QString origMode = QStringLiteral("USB");
    int     origPb   = 2700;
    {
        const QStringList fl = c.send(QStringLiteral("\\get_freq"));
        const QString fs = c.field(fl, QStringLiteral("Frequency"));
        if (!fs.isEmpty()) origFreq = fs.toLongLong();

        const QStringList ml = c.send(QStringLiteral("\\get_mode"));
        const QString ms = c.field(ml, QStringLiteral("Mode"));
        const QString ps = c.field(ml, QStringLiteral("Passband"));
        if (!ms.isEmpty()) origMode = ms;
        if (!ps.isEmpty()) origPb = ps.toInt();
    }

    section1(c, r);
    section1b(c, r);
    section2(c, r, origFreq);
    section3(c, r, origMode, origPb);
    section4(c, r);
    section5(c, r, origFreq);

    if (doPtt) section6Ptt(c, r);
    else        section6Skip(r);

    section7(c, r);
    section8(c, r);
    section9(c, r);
    section10(c, r);

    if (doCw) section11Cw(c, r);
    else       section11Skip(r);

    section12(c, r);
    section13(host, port, timeout, r, origFreq, origMode, origPb);
    section14(c, r);
    section15(c, r);
    sectionEdge(c, r);
    sectionPty(r, ptyPath);

    // Best-effort state restore.
    c.send(QStringLiteral("\\set_ptt 0"));
    c.send(QStringLiteral("\\stop_morse"));
    c.send(QStringLiteral("\\set_split_vfo 0 VFOA"));
    c.send(QStringLiteral("\\set_freq %1").arg(origFreq));
    c.send(QStringLiteral("\\set_mode %1 %2").arg(origMode).arg(origPb));
    c.close();

    return r.summary() ? 0 : 1;
}
