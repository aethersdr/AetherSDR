// AetherSDR TS-2000 dialect CAT integration test.
// Requires a running AetherSDR instance with a TS-2000 CAT port enabled.
//
// Build:  cmake --build build --target CAT_TS-2000_test
// Run:    ./build/CAT_TS-2000_test [--host HOST] [--port PORT] [--ptt] [--cw] [--pty PATH]
//
// Mirrors tests/test_ts2000.py.  PTT tests (section 9) are disabled by default;
// enable only when a dummy load or antenna is connected.

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QSet>
#include <QStandardPaths>
#include <QTcpSocket>
#include <QThread>

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

static QString ansi(const char* code, const QString& s)
{
    if (!g_tty) return s;
    return QLatin1String("\033[") + QLatin1String(code) + 'm' + s + QLatin1String("\033[0m");
}

static QString green(const QString& s)  { return ansi("92", s); }
static QString red(const QString& s)    { return ansi("91", s); }
static QString yellow(const QString& s) { return ansi("93", s); }
static QString cyan(const QString& s)   { return ansi("96", s); }
static QString bold(const QString& s)   { return ansi("1",  s); }

// ── CatClient ─────────────────────────────────────────────────────────────────

class CatClient
{
public:
    explicit CatClient(int timeoutMs = 3000)
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

    // Send "cmd;" and read response up to next ';'; returns string without ';'.
    QString query(const QString& cmd)
    {
        m_sock.write((cmd + ';').toUtf8());
        m_sock.flush();
        return readUntilSemi();
    }

    // Send "cmd;" with no response expected (set commands).
    void send(const QString& cmd)
    {
        m_sock.write((cmd + ';').toUtf8());
        m_sock.flush();
    }

    // Read until ';' with a custom timeout; returns null QString on timeout.
    QString tryRead(int msTimeout)
    {
        const int saved = m_timeout;
        m_timeout = msTimeout;
        QString result = readUntilSemi();
        m_timeout = saved;
        return result;
    }

    void close() { m_sock.disconnectFromHost(); }

    // Public for manual poll loops.
    QString readUntilSemi()
    {
        while (!m_buf.contains(';')) {
            if (!m_sock.waitForReadyRead(m_timeout))
                return {};
            m_buf += m_sock.readAll();
        }
        const int idx = m_buf.indexOf(';');
        QString resp = QString::fromUtf8(m_buf.left(idx));
        m_buf.remove(0, idx + 1);
        return resp;
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

// ── Helpers ───────────────────────────────────────────────────────────────────

static QString repr(const QString& s)
{
    if (s.isNull()) return QStringLiteral("(timeout)");
    return '"' + s + '"';
}

static QString hz11(qint64 hz)
{
    return QStringLiteral("%1").arg(hz, 11, 10, QChar('0'));
}

static bool isDigits(const QString& s, int n)
{
    if (s.size() != n) return false;
    for (const QChar c : s) {
        if (!c.isDigit()) return false;
    }
    return true;
}

static bool isValidAgcCode(const QString& s)
{
    return s == QLatin1String("0") || s == QLatin1String("2") ||
           s == QLatin1String("3") || s == QLatin1String("4");
}

struct IfFields {
    QString freq_hz;
    QString step;
    QString rit_hz;
    QChar   rit_on{'?'};
    QChar   xit{'?'};
    QString mem;
    QChar   tx{'?'};
    QChar   mode{'?'};
    QChar   func{'?'};
    QChar   scan{'?'};
    QChar   split{'?'};
    QChar   tone{'?'};
    QString ctcss;
    QChar   dcs{'?'};
    bool    valid{false};
};

static IfFields parseIfBody(const QString& body)
{
    IfFields f;
    if (body.size() != 35) return f;
    f.freq_hz = body.mid(0, 11);
    f.step    = body.mid(11, 5);
    f.rit_hz  = body.mid(16, 6);
    f.rit_on  = body[22];
    f.xit     = body[23];
    f.mem     = body.mid(24, 2);
    f.tx      = body[26];
    f.mode    = body[27];
    f.func    = body[28];
    f.scan    = body[29];
    f.split   = body[30];
    f.tone    = body[31];
    f.ctcss   = body.mid(32, 2);
    f.dcs     = body[34];
    f.valid   = true;
    return f;
}

static const QSet<QString> kValidIDs = {
    QStringLiteral("ID019"),  // Kenwood TS-2000 (TS-2000 dialect)
    QStringLiteral("ID904"), QStringLiteral("ID905"), QStringLiteral("ID906"),
    QStringLiteral("ID907"), QStringLiteral("ID908"), QStringLiteral("ID909"),
    QStringLiteral("ID910"), QStringLiteral("ID911"), QStringLiteral("ID919"),
    QStringLiteral("ID930"), QStringLiteral("ID931"),
};

struct ModeEntry { char code; const char* name; };
static const ModeEntry kKenwoodModes[] = {
    { '1', "LSB" }, { '2', "USB" }, { '3', "CW" },
    { '4', "FM"  }, { '5', "AM"  }, { '6', "DIGL" }, { '9', "DIGU" },
};

// ═════════════════════════════════════════════════════════════════════════════
// Section 1 — Connection & Identification
// ═════════════════════════════════════════════════════════════════════════════

void section1(CatClient& c, Runner& r)
{
    r.section(QStringLiteral("Section 1 — Connection & Identification"));

    QString resp = c.query(QStringLiteral("ID"));
    r.check(QStringLiteral("1.1  ID; → valid model ID from CAT guide table"),
            kValidIDs.contains(resp), repr(resp));

    resp = c.query(QStringLiteral("PS"));
    r.check(QStringLiteral("1.2  PS; → PS1 (power on)"),
            resp == QLatin1String("PS1"), repr(resp));

    resp = c.query(QStringLiteral("SM0"));
    r.check(QStringLiteral("1.3  SM0; → SM00000 (S-meter zero)"),
            resp == QLatin1String("SM00000"), repr(resp));
}

// ═════════════════════════════════════════════════════════════════════════════
// Section 2 — VFO-A Frequency (FA)
// ═════════════════════════════════════════════════════════════════════════════

qint64 section2(CatClient& c, Runner& r, qint64 origHz)
{
    r.section(QStringLiteral("Section 2 — VFO-A Frequency (FA)"));

    QString resp = c.query(QStringLiteral("FA"));
    r.check(QStringLiteral("2.1  FA; returns \"FA\" + 11-digit Hz"),
            resp.startsWith(QLatin1String("FA")) && isDigits(resp.mid(2), 11),
            repr(resp));
    qint64 currentHz = (resp.startsWith(QLatin1String("FA")) && isDigits(resp.mid(2), 11))
                       ? resp.mid(2).toLongLong()
                       : origHz;

    const qint64 testHz = 14'074'000;
    c.send(QStringLiteral("FA") + hz11(testHz));
    QThread::msleep(150);
    resp = c.query(QStringLiteral("FA"));
    r.check(QStringLiteral("2.2  set FA 14.074 MHz → FA; confirms new frequency"),
            resp == QStringLiteral("FA") + hz11(testHz),
            QStringLiteral("got %1").arg(repr(resp)));

    const qint64 test2Hz = 7'074'000;
    c.send(QStringLiteral("FA") + hz11(test2Hz));
    QThread::msleep(150);
    resp = c.query(QStringLiteral("FA"));
    r.check(QStringLiteral("2.3  second set FA (different band) confirms correctly"),
            resp == QStringLiteral("FA") + hz11(test2Hz),
            QStringLiteral("got %1").arg(repr(resp)));

    c.send(QStringLiteral("FA") + hz11(origHz));
    QThread::msleep(100);
    return currentHz;
}

// ═════════════════════════════════════════════════════════════════════════════
// Section 3 — VFO-B Frequency (FB)
// ═════════════════════════════════════════════════════════════════════════════

void section3(CatClient& c, Runner& r)
{
    r.section(QStringLiteral("Section 3 — VFO-B Frequency (FB)"));

    QString resp = c.query(QStringLiteral("FB"));
    r.check(QStringLiteral("3.1  FB; returns \"FB\" + 11-digit Hz"),
            resp.startsWith(QLatin1String("FB")) && isDigits(resp.mid(2), 11),
            repr(resp));

    QString faResp = c.query(QStringLiteral("FA"));
    const QString faHz = faResp.startsWith(QLatin1String("FA")) ? faResp.mid(2) : QString();
    const QString fbHz = resp.startsWith(QLatin1String("FB")) ? resp.mid(2) : QString();
    const bool vfoBMapped = (!fbHz.isEmpty() && !faHz.isEmpty() && fbHz != faHz);

    if (!vfoBMapped) {
        r.skip(QStringLiteral("3.2  set FB 14.225 MHz → FB; confirms"),
               QStringLiteral("VFO B not mapped to a second slice — configure port VFO B first"));
    } else {
        const qint64 testHz = 14'225'000;
        c.send(QStringLiteral("FB") + hz11(testHz));
        QElapsedTimer timer;
        timer.start();
        QString pollResp;
        do {
            pollResp = c.query(QStringLiteral("FB"));
            if (pollResp == QStringLiteral("FB") + hz11(testHz)) break;
            if (timer.elapsed() < 2000) QThread::msleep(100);
        } while (timer.elapsed() < 2000);
        r.check(QStringLiteral("3.2  set FB 14.225 MHz → FB; confirms"),
                pollResp == QStringLiteral("FB") + hz11(testHz),
                QStringLiteral("got %1").arg(repr(pollResp)));
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Section 4 — Mode (MD) — Kenwood 1-digit codes
// ═════════════════════════════════════════════════════════════════════════════

void section4(CatClient& c, Runner& r, const QString& origModeDigit)
{
    r.section(QStringLiteral("Section 4 — Mode (MD) — Kenwood 1-digit codes"));

    QString resp = c.query(QStringLiteral("MD"));
    const QString digit = resp.startsWith(QLatin1String("MD")) ? resp.mid(2) : QString();
    bool validDigit = false;
    for (const ModeEntry& m : kKenwoodModes) {
        if (digit == QString(QLatin1Char(m.code))) { validDigit = true; break; }
    }
    r.check(QStringLiteral("4.1  MD; returns \"MD\" + valid Kenwood mode digit"),
            resp.startsWith(QLatin1String("MD")) && validDigit, repr(resp));

    int checkNum = 2;
    for (const ModeEntry& mode : kKenwoodModes) {
        const QString codeStr = QString(QLatin1Char(mode.code));
        c.send(QStringLiteral("MD") + codeStr);
        QElapsedTimer timer;
        timer.start();
        QString pollResp;
        do {
            pollResp = c.query(QStringLiteral("MD"));
            if (pollResp == QStringLiteral("MD") + codeStr) break;
            if (timer.elapsed() < 2000) QThread::msleep(100);
        } while (timer.elapsed() < 2000);
        r.check(QStringLiteral("4.%1  set MD%2 (%3) → MD; round-trips correctly")
                    .arg(checkNum).arg(codeStr, QString::fromLatin1(mode.name)),
                pollResp == QStringLiteral("MD") + codeStr,
                QStringLiteral("got %1").arg(repr(pollResp)));
        ++checkNum;
    }

    c.send(QStringLiteral("MD") + origModeDigit);
    QThread::msleep(200);
}

// ═════════════════════════════════════════════════════════════════════════════
// Section 5 — IF Composite Status
// ═════════════════════════════════════════════════════════════════════════════

void section5(CatClient& c, Runner& r)
{
    r.section(QStringLiteral("Section 5 — IF Composite Status"));

    c.send(QStringLiteral("FA") + hz11(14'225'000));
    c.send(QStringLiteral("MD2"));
    c.send(QStringLiteral("FT0"));
    QThread::msleep(200);

    QString resp = c.query(QStringLiteral("IF"));
    const QString body = resp.startsWith(QLatin1String("IF")) ? resp.mid(2) : QString();
    const IfFields fields = parseIfBody(body);

    r.check(QStringLiteral("5.1  IF; returns \"IF\" + exactly 35-char body"),
            resp.startsWith(QLatin1String("IF")) && body.size() == 35,
            QStringLiteral("body len=%1 body=%2").arg(body.size()).arg(repr(body)));

    r.check(QStringLiteral("5.2  IF body freq field (0-10) is all digits"),
            isDigits(fields.freq_hz, 11), repr(fields.freq_hz));

    QString faResp = c.query(QStringLiteral("FA"));
    const QString faHz = faResp.startsWith(QLatin1String("FA")) ? faResp.mid(2) : QString();
    r.check(QStringLiteral("5.3  IF body freq field matches FA; response"),
            fields.freq_hz == faHz,
            QStringLiteral("IF=%1 FA=%2").arg(repr(fields.freq_hz), repr(faHz)));

    r.check(QStringLiteral("5.4  IF body TX field (pos 26) is '0' while receiving"),
            fields.tx == QChar('0'), repr(QString(fields.tx)));

    QString mdResp = c.query(QStringLiteral("MD"));
    const QString mdDigit = mdResp.startsWith(QLatin1String("MD")) ? mdResp.mid(2) : QString();
    r.check(QStringLiteral("5.5  IF body mode field (pos 27) matches MD; digit"),
            QString(fields.mode) == mdDigit,
            QStringLiteral("IF mode=%1 MD=%2").arg(repr(QString(fields.mode)), repr(mdDigit)));

    r.check(QStringLiteral("5.6  IF body split field (pos 30) is '0' (FT0 set above)"),
            fields.split == QChar('0'), repr(QString(fields.split)));

    r.check(QStringLiteral("5.7  IF body step (pos 11-15) is 5 digits"),
            isDigits(fields.step, 5), repr(fields.step));
}

// ═════════════════════════════════════════════════════════════════════════════
// Section 6 — AI Mode (async unsolicited updates)
// ═════════════════════════════════════════════════════════════════════════════

void section6(CatClient& c, Runner& r)
{
    r.section(QStringLiteral("Section 6 — AI Mode (async unsolicited updates)"));

    QString resp = c.query(QStringLiteral("AI"));
    r.check(QStringLiteral("6.1  AI; → AI0 (disabled by default)"),
            resp == QLatin1String("AI0"), repr(resp));

    c.send(QStringLiteral("AI1"));
    resp = c.query(QStringLiteral("AI"));
    r.check(QStringLiteral("6.2  AI1; enables AI; AI; → AI1"),
            resp == QLatin1String("AI1"), repr(resp));

    const qint64 newHz = 14'100'000;
    c.send(QStringLiteral("FA") + hz11(newHz));
    QString push = c.tryRead(2000);
    r.check(QStringLiteral("6.3  AI push after FA set — received FA%1").arg(hz11(newHz)),
            push == QStringLiteral("FA") + hz11(newHz), repr(push));

    // Drain any async state pushes that may arrive alongside or after the FA
    // change (mode-by-band, filter notifications, etc.).  Use a 500 ms window
    // and keep looping until the stream is quiet.
    { QString p; do { p = c.tryRead(500); } while (!p.isNull()); }

    c.send(QStringLiteral("MD1"));
    // Scan incoming pushes for MD1; skip any stale state pushes that may
    // have been queued before our command was processed.
    push = {};
    {
        QElapsedTimer t; t.start();
        while (t.elapsed() < 3000) {
            QString p = c.tryRead(500);
            if (p.isNull()) break;
            if (p == QLatin1String("MD1")) { push = p; break; }
        }
    }
    r.check(QStringLiteral("6.4  AI push after MD set — received MD1 (LSB)"),
            push == QLatin1String("MD1"), repr(push));

    c.send(QStringLiteral("AI0"));
    resp = c.query(QStringLiteral("AI"));
    r.check(QStringLiteral("6.5  AI0; disables AI; AI; → AI0"),
            resp == QLatin1String("AI0"), repr(resp));

    c.send(QStringLiteral("FA") + hz11(14'225'000));
    QString silence = c.tryRead(500);
    r.check(QStringLiteral("6.6  no AI push after AI0 (set FA, expect silence)"),
            silence.isNull(),
            QStringLiteral("got unexpected push: %1").arg(repr(silence)));
}

// ═════════════════════════════════════════════════════════════════════════════
// Section 7 — Split (FT)
// ═════════════════════════════════════════════════════════════════════════════

void section7(CatClient& c, Runner& r)
{
    r.section(QStringLiteral("Section 7 — Split (FT)"));

    QString resp = c.query(QStringLiteral("FT"));
    r.check(QStringLiteral("7.1  FT; → FT0 (split off initially)"),
            resp == QLatin1String("FT0"), repr(resp));

    c.send(QStringLiteral("FT1"));
    resp = c.query(QStringLiteral("FT"));
    r.check(QStringLiteral("7.2  FT1; enables split; FT; → FT1"),
            resp == QLatin1String("FT1"), repr(resp));

    QString respIf = c.query(QStringLiteral("IF"));
    IfFields fields = parseIfBody(respIf.startsWith(QLatin1String("IF")) ? respIf.mid(2) : QString());
    r.check(QStringLiteral("7.3  IF body split field (pos 30) is '1' while split on"),
            fields.split == QChar('1'),
            QStringLiteral("split=%1").arg(repr(QString(fields.split))));

    c.send(QStringLiteral("FT0"));
    resp = c.query(QStringLiteral("FT"));
    r.check(QStringLiteral("7.4  FT0; clears split; FT; → FT0"),
            resp == QLatin1String("FT0"), repr(resp));

    respIf = c.query(QStringLiteral("IF"));
    fields = parseIfBody(respIf.startsWith(QLatin1String("IF")) ? respIf.mid(2) : QString());
    r.check(QStringLiteral("7.5  IF body split field returns to '0' after FT0"),
            fields.split == QChar('0'), repr(QString(fields.split)));
}

// ═════════════════════════════════════════════════════════════════════════════
// Section 8 — FR (RX VFO select — accepted, no-op)
// ═════════════════════════════════════════════════════════════════════════════

void section8(CatClient& c, Runner& r)
{
    r.section(QStringLiteral("Section 8 — FR (RX VFO select — accepted, no-op)"));

    c.send(QStringLiteral("FR0"));
    c.send(QStringLiteral("FR1"));
    QThread::msleep(50);
    QString resp = c.query(QStringLiteral("FA"));
    r.check(QStringLiteral("8.1  FR0; FR1; produce no response; FA; works normally after"),
            resp.startsWith(QLatin1String("FA")) && isDigits(resp.mid(2), 11),
            repr(resp));
}

// ═════════════════════════════════════════════════════════════════════════════
// Section 9 — PTT  (optional — requires dummy load or antenna)
// ═════════════════════════════════════════════════════════════════════════════

void section9ptt(CatClient& c, Runner& r)
{
    r.section(QStringLiteral("Section 9 — PTT  ⚠  TX ACTIVE"));

    QString resp = c.query(QStringLiteral("IF"));
    IfFields fields = parseIfBody(resp.startsWith(QLatin1String("IF")) ? resp.mid(2) : QString());
    r.check(QStringLiteral("9.0  IF confirms TX=0 before keying"),
            fields.tx == QChar('0'), repr(QString(fields.tx)));

    c.send(QStringLiteral("TX"));
    QThread::msleep(1000);

    resp = c.query(QStringLiteral("IF"));
    fields = parseIfBody(resp.startsWith(QLatin1String("IF")) ? resp.mid(2) : QString());
    const bool txOn = (fields.tx == QChar('1'));

    if (!txOn) {
        r.skip(QStringLiteral("9.1  TX; keys radio; IF TX='1'"),
               QStringLiteral("TX blocked — check interlock/inhibit line"));
        r.skip(QStringLiteral("9.2  RX; unkeys radio; IF TX='0'"),
               QStringLiteral("TX blocked"));
        r.skip(QStringLiteral("9.3  TX0; alt unkey; IF TX='0'"),
               QStringLiteral("TX blocked"));
        c.send(QStringLiteral("RX"));
        return;
    }

    r.check(QStringLiteral("9.1  TX; keys radio; IF body TX field = '1'"),
            txOn, repr(QString(fields.tx)));

    c.send(QStringLiteral("RX"));
    QThread::msleep(250);

    resp = c.query(QStringLiteral("IF"));
    fields = parseIfBody(resp.startsWith(QLatin1String("IF")) ? resp.mid(2) : QString());
    r.check(QStringLiteral("9.2  RX; unkeys radio; IF body TX field = '0'"),
            fields.tx == QChar('0'), repr(QString(fields.tx)));

    c.send(QStringLiteral("TX0"));
    QThread::msleep(150);
    resp = c.query(QStringLiteral("IF"));
    fields = parseIfBody(resp.startsWith(QLatin1String("IF")) ? resp.mid(2) : QString());
    r.check(QStringLiteral("9.3  TX0; (alternate unkey) accepted; IF TX field = '0'"),
            fields.tx == QChar('0'), repr(QString(fields.tx)));

    // Safety: watch 2 s — firmware may briefly re-assert TX after unkey
    {
        QElapsedTimer safeTimer; safeTimer.start();
        bool sawTxOn = false;
        c.send(QStringLiteral("RX"));
        do {
            QThread::msleep(250);
            resp = c.query(QStringLiteral("IF"));
            fields = parseIfBody(resp.startsWith(QLatin1String("IF")) ? resp.mid(2) : QString());
            if (fields.tx != QChar('0')) { sawTxOn = true; c.send(QStringLiteral("RX")); }
        } while (safeTimer.elapsed() < 2000);
        if (sawTxOn)
            qWarning("9.S: TX re-asserted during 2-s safety watch — forced RX; possible firmware bug");
        r.check(QStringLiteral("9.S  safety: TX confirmed off after 2-s watch"),
                fields.tx == QChar('0'), repr(QString(fields.tx)));
    }
}

void section9skip(Runner& r)
{
    r.section(QStringLiteral("Section 9 — PTT  (skipped — pass --ptt to enable)"));
    for (const char* name : { "9.0 IF TX=0 baseline", "9.1 TX; keys",
                               "9.2 RX; unkeys", "9.3 TX0; alt form" }) {
        r.skip(QString::fromLatin1(name), QStringLiteral("--ptt not set"));
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Section 10 — ZZ Extensions Blocked (TS-2000 dialect)
// ═════════════════════════════════════════════════════════════════════════════

void section10(CatClient& c, Runner& r)
{
    r.section(QStringLiteral("Section 10 — ZZ Extensions Blocked (TS-2000 dialect)"));

    struct Entry { const char* cmd; const char* label; };
    static const Entry entries[] = {
        { "ZZFA",  "10.1" }, { "ZZFB",  "10.2" }, { "ZZMD",  "10.3" },
        { "ZZME",  "10.4" }, { "ZZIF",  "10.5" }, { "ZZAI",  "10.6" },
        { "ZZSW",  "10.7" }, { "ZZSM0", "10.8" },
    };
    for (const Entry& e : entries) {
        QString resp = c.query(QString::fromLatin1(e.cmd));
        r.check(QStringLiteral("%1  %2; → ?; (ZZ blocked in TS-2000)")
                    .arg(QString::fromLatin1(e.label), QString::fromLatin1(e.cmd)),
                resp == QLatin1String("?"), repr(resp));
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Section 11 — Unknown / Invalid Commands
// ═════════════════════════════════════════════════════════════════════════════

void section11(CatClient& c, Runner& r)
{
    r.section(QStringLiteral("Section 11 — Unknown / Invalid Commands"));

    struct Entry { const char* cmd; const char* label; };
    static const Entry entries[] = {
        { "XQ",   "11.1" }, { "ZZ",   "11.2" }, { "ABCD", "11.3" },
    };
    for (const Entry& e : entries) {
        QString resp = c.query(QString::fromLatin1(e.cmd));
        r.check(QStringLiteral("%1  %2; → ?;")
                    .arg(QString::fromLatin1(e.label), QString::fromLatin1(e.cmd)),
                resp == QLatin1String("?"), repr(resp));
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Section 12 — Tier-2 Commands (AG, GT, PC, NB, KS, PT, KY, RT, RG, RC, RD, RU, XT)
// ═════════════════════════════════════════════════════════════════════════════

void section12(CatClient& c, Runner& r)
{
    r.section(QStringLiteral("Section 12 — Tier-2 Commands (AG, GT, PC, NB, KS, PT, KY, RT, RG, XT)"));

    // ── AG: VFO A audio gain (0-100, 3-digit) ──────────────────────────────
    QString origAg = c.query(QStringLiteral("AG"));
    r.check(QStringLiteral("12.1  AG; → \"AG\" + 3-digit gain"),
            origAg.startsWith(QLatin1String("AG")) && isDigits(origAg.mid(2), 3), repr(origAg));

    c.send(QStringLiteral("AG075"));
    QThread::msleep(100);
    QString resp = c.query(QStringLiteral("AG"));
    r.check(QStringLiteral("12.2  set AG075 → AG; confirms AG075"),
            resp == QLatin1String("AG075"), repr(resp));

    if (origAg.startsWith(QLatin1String("AG"))) { c.send(origAg); QThread::msleep(50); }

    // ── GT: AGC mode (0=Off, 2=Slow, 3=Med, 4=Fast) ────────────────────────
    QString origGt = c.query(QStringLiteral("GT"));
    r.check(QStringLiteral("12.3  GT; → \"GT\" + valid AGC code"),
            origGt.startsWith(QLatin1String("GT")) && isValidAgcCode(origGt.mid(2)), repr(origGt));

    c.send(QStringLiteral("GT3"));
    QThread::msleep(150);
    resp = c.query(QStringLiteral("GT"));
    r.check(QStringLiteral("12.4  set GT3 (Med) → GT; confirms GT3"),
            resp == QLatin1String("GT3"), repr(resp));

    if (origGt.startsWith(QLatin1String("GT"))) { c.send(origGt); QThread::msleep(100); }

    // ── PC: RF power (0-100, 3-digit) ──────────────────────────────────────
    QString origPc = c.query(QStringLiteral("PC"));
    r.check(QStringLiteral("12.5  PC; → \"PC\" + 3-digit power"),
            origPc.startsWith(QLatin1String("PC")) && isDigits(origPc.mid(2), 3), repr(origPc));

    c.send(QStringLiteral("PC080"));
    QThread::msleep(100);
    resp = c.query(QStringLiteral("PC"));
    r.check(QStringLiteral("12.6  set PC080 → PC; confirms PC080"),
            resp == QLatin1String("PC080"), repr(resp));

    if (origPc.startsWith(QLatin1String("PC"))) { c.send(origPc); QThread::msleep(50); }

    // ── NB: Wide Noise Blanker state (0/1) ─────────────────────────────────
    QString origNb = c.query(QStringLiteral("NB"));
    r.check(QStringLiteral("12.7  NB; → NB0 or NB1"),
            origNb == QLatin1String("NB0") || origNb == QLatin1String("NB1"), repr(origNb));

    QString newNb = (origNb == QLatin1String("NB0")) ? QStringLiteral("NB1") : QStringLiteral("NB0");
    c.send(newNb);
    QThread::msleep(100);
    resp = c.query(QStringLiteral("NB"));
    r.check(QStringLiteral("12.8  toggle NB → confirms %1").arg(newNb),
            resp == newNb, repr(resp));

    c.send(origNb);
    QThread::msleep(50);

    // ── KS: CW keying speed (005-100, 3-digit) ──────────────────────────────
    QString origKs = c.query(QStringLiteral("KS"));
    r.check(QStringLiteral("12.9  KS; → \"KS\" + 3-digit WPM"),
            origKs.startsWith(QLatin1String("KS")) && isDigits(origKs.mid(2), 3), repr(origKs));

    c.send(QStringLiteral("KS025"));
    QThread::msleep(100);
    resp = c.query(QStringLiteral("KS"));
    r.check(QStringLiteral("12.10 set KS025 (25 WPM) → KS; confirms KS025"),
            resp == QLatin1String("KS025"), repr(resp));

    if (origKs.startsWith(QLatin1String("KS"))) { c.send(origKs); QThread::msleep(50); }

    // ── PT: CW pitch (100-999, 3-digit) ─────────────────────────────────────
    QString origPt = c.query(QStringLiteral("PT"));
    r.check(QStringLiteral("12.11 PT; → \"PT\" + 3-digit Hz"),
            origPt.startsWith(QLatin1String("PT")) && isDigits(origPt.mid(2), 3), repr(origPt));

    c.send(QStringLiteral("PT600"));
    QThread::msleep(100);
    resp = c.query(QStringLiteral("PT"));
    r.check(QStringLiteral("12.12 set PT600 (600 Hz) → PT; confirms PT600"),
            resp == QLatin1String("PT600"), repr(resp));

    if (origPt.startsWith(QLatin1String("PT"))) { c.send(origPt); QThread::msleep(50); }

    // ── KY: CW send / busy query ─────────────────────────────────────────────
    resp = c.query(QStringLiteral("KY"));
    r.check(QStringLiteral("12.13 KY; (query) → KY0 (not busy) or KY1 (busy)"),
            resp == QLatin1String("KY0") || resp == QLatin1String("KY1"), repr(resp));

    // ── RT: RIT state (0/1) ──────────────────────────────────────────────────
    resp = c.query(QStringLiteral("RT"));
    r.check(QStringLiteral("12.14 RT; → RT0 or RT1"),
            resp == QLatin1String("RT0") || resp == QLatin1String("RT1"), repr(resp));

    c.send(QStringLiteral("RT1"));
    QThread::msleep(100);
    resp = c.query(QStringLiteral("RT"));
    r.check(QStringLiteral("12.14b RT1; enables RIT → RT; confirms RT1"),
            resp == QLatin1String("RT1"), repr(resp));

    c.send(QStringLiteral("RT0"));
    QThread::msleep(50);
    resp = c.query(QStringLiteral("RT"));
    r.check(QStringLiteral("12.14c RT0; clears RIT → RT; confirms RT0"),
            resp == QLatin1String("RT0"), repr(resp));

    // ── RG: RIT frequency (±NNNNN Hz) ────────────────────────────────────────
    c.send(QStringLiteral("RG+00500"));
    QThread::msleep(100);
    resp = c.query(QStringLiteral("RG"));
    r.check(QStringLiteral("12.15 set RG+00500 → RG; returns \"RG+00500\""),
            resp == QLatin1String("RG+00500"), repr(resp));

    // ── RC: clear RIT to 0 ───────────────────────────────────────────────────
    c.send(QStringLiteral("RC"));
    QThread::msleep(50);
    resp = c.query(QStringLiteral("RG"));
    r.check(QStringLiteral("12.16 RC; clears RIT → RG; returns \"RG+00000\""),
            resp == QLatin1String("RG+00000"), repr(resp));

    // ── RU / RD: increment / decrement ───────────────────────────────────────
    c.send(QStringLiteral("RU00100"));
    QThread::msleep(50);
    resp = c.query(QStringLiteral("RG"));
    r.check(QStringLiteral("12.17 RU00100; increments RIT by 100 Hz → RG+00100"),
            resp == QLatin1String("RG+00100"), repr(resp));

    c.send(QStringLiteral("RD00050"));
    QThread::msleep(50);
    resp = c.query(QStringLiteral("RG"));
    r.check(QStringLiteral("12.18 RD00050; decrements RIT by 50 Hz → RG+00050"),
            resp == QLatin1String("RG+00050"), repr(resp));

    c.send(QStringLiteral("RC"));
    c.send(QStringLiteral("RT0"));
    QThread::msleep(50);

    // ── XT: XIT state (0/1) ──────────────────────────────────────────────────
    resp = c.query(QStringLiteral("XT"));
    r.check(QStringLiteral("12.19 XT; → XT0 or XT1"),
            resp == QLatin1String("XT0") || resp == QLatin1String("XT1"), repr(resp));

    c.send(QStringLiteral("XT1"));
    QThread::msleep(100);
    resp = c.query(QStringLiteral("XT"));
    r.check(QStringLiteral("12.20 XT1; enables XIT → XT; confirms XT1"),
            resp == QLatin1String("XT1"), repr(resp));

    c.send(QStringLiteral("XT0"));
    QThread::msleep(100);
    resp = c.query(QStringLiteral("XT"));
    r.check(QStringLiteral("12.21 XT0; clears XIT → XT; confirms XT0"),
            resp == QLatin1String("XT0"), repr(resp));
}

// ═════════════════════════════════════════════════════════════════════════════
// Section 13 — CW Keyer (KY send)  (optional — requires antenna/dummy load)
// ═════════════════════════════════════════════════════════════════════════════

void section13cw(CatClient& c, Runner& r)
{
    r.section(QStringLiteral("Section 13 — CW Keyer (KY)  ⚠  TX ACTIVE"));

    QString origMd = c.query(QStringLiteral("MD"));
    c.send(QStringLiteral("MD3"));  // CW mode required for keying
    QThread::msleep(200);

    QString resp = c.query(QStringLiteral("KY"));
    r.check(QStringLiteral("13.1 KY; → KY0 (buffer idle before send)"),
            resp == QLatin1String("KY0"), repr(resp));

    // 18 chars: ~2 s at 100 WPM, ~8 s at 25 WPM — recognisable on-air if accidentally transmitted
    c.send(QStringLiteral("KY CQ CQ CQ TEST TEST"));  // P1 = mandatory space, text follows

    QElapsedTimer timer;
    timer.start();
    QString pollResp;
    do {
        pollResp = c.query(QStringLiteral("KY"));
        if (pollResp == QLatin1String("KY1")) break;
        if (timer.elapsed() < 5000) QThread::msleep(20);
    } while (timer.elapsed() < 5000);
    if (pollResp != QLatin1String("KY1")) {
        r.skip(QStringLiteral("13.2 KY CQ CQ CQ TEST TEST; → KY1 (busy)"),
               QStringLiteral("keyer did not start — check interlock/inhibit line"));
        r.skip(QStringLiteral("13.3 KY; → KY0 (complete)"),
               QStringLiteral("keyer did not start"));
        if (origMd.startsWith(QLatin1String("MD"))) { c.send(origMd); QThread::msleep(100); }
        c.send(QStringLiteral("RX"));
        return;
    }
    r.check(QStringLiteral("13.2 KY CQ CQ CQ TEST TEST; → KY; → KY1 (keyer busy / transmitting)"),
            true, {});

    timer.restart();
    do {
        pollResp = c.query(QStringLiteral("KY"));
        if (pollResp == QLatin1String("KY0")) break;
        if (timer.elapsed() < 15000) QThread::msleep(200);
    } while (timer.elapsed() < 15000);
    r.check(QStringLiteral("13.3 KY; → KY0 (keyer idle after transmission complete)"),
            pollResp == QLatin1String("KY0"), repr(pollResp));

    if (origMd.startsWith(QLatin1String("MD"))) { c.send(origMd); QThread::msleep(100); }

    // Safety: watch 2 s — CW TX-tail may keep PA on after keyer reports KY0
    {
        QElapsedTimer safeTimer; safeTimer.start();
        bool sawTxOn = false;
        QString safeResp; IfFields sf;
        c.send(QStringLiteral("RX"));
        do {
            QThread::msleep(250);
            safeResp = c.query(QStringLiteral("IF"));
            sf = parseIfBody(safeResp.startsWith(QLatin1String("IF")) ? safeResp.mid(2) : QString());
            if (sf.tx != QChar('0')) { sawTxOn = true; c.send(QStringLiteral("RX")); }
        } while (safeTimer.elapsed() < 2000);
        if (sawTxOn)
            qWarning("13.S: TX re-asserted during 2-s safety watch — forced RX; possible firmware bug");
        r.check(QStringLiteral("13.S safety: TX confirmed off after 2-s watch"),
                sf.tx == QChar('0'), repr(QString(sf.tx)));
    }
}

void section13skip(Runner& r)
{
    r.section(QStringLiteral("Section 13 — CW Keyer (skipped — pass --cw to enable)"));
    for (const char* name : { "13.1 KY; → KY0 baseline",
                               "13.2 KY CQ CQ CQ TEST TEST; → KY1 (busy)",
                               "13.3 KY; → KY0 (complete)" }) {
        r.skip(QString::fromLatin1(name), QStringLiteral("--cw not set"));
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Section 14 — PTY round-trip  (Unix only; skips if device cannot be opened)
// ═════════════════════════════════════════════════════════════════════════════

#if defined(Q_OS_UNIX)
class PtyClient
{
public:
    explicit PtyClient(int timeoutMs = 3000) : m_timeout(timeoutMs) {}
    ~PtyClient() { closeDevice(); }

    bool openDevice(const QString& path)
    {
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

    void closeDevice() { if (m_fd >= 0) { ::close(m_fd); m_fd = -1; } m_buf.clear(); }

    void send(const QString& cmd)
    {
        QByteArray d = (cmd + ';').toUtf8();
        ::write(m_fd, d.constData(), d.size());
    }

    QString query(const QString& cmd) { send(cmd); return readUntilSemi(); }

    QString readUntilSemi()
    {
        QElapsedTimer t; t.start();
        while (!m_buf.contains(';')) {
            char tmp[256];
            ssize_t n = ::read(m_fd, tmp, sizeof(tmp));
            if (n > 0) m_buf.append(tmp, static_cast<int>(n));
            else { if (t.elapsed() >= m_timeout) return {}; QThread::msleep(10); }
        }
        const int idx = m_buf.indexOf(';');
        QString resp = QString::fromUtf8(m_buf.left(idx));
        m_buf.remove(0, idx + 1);
        return resp;
    }

private:
    int        m_fd{-1};
    QByteArray m_buf;
    int        m_timeout;
};

void section14pty(Runner& r, const QString& ptyPath)
{
    r.section(QStringLiteral("Section 14 — PTY round-trip (%1)").arg(ptyPath));

    PtyClient p;
    if (!p.openDevice(ptyPath)) {
        for (const char* name : { "14.1 PTY ID;", "14.2 PTY FA;", "14.3 PTY PS;" })
            r.skip(QString::fromLatin1(name),
                   QStringLiteral("cannot open %1").arg(ptyPath));
        return;
    }

    QString resp = p.query(QStringLiteral("ID"));
    r.check(QStringLiteral("14.1 ID; via PTY → valid model ID"),
            kValidIDs.contains(resp), repr(resp));

    resp = p.query(QStringLiteral("FA"));
    r.check(QStringLiteral("14.2 FA; via PTY → \"FA\" + 11-digit Hz"),
            resp.startsWith(QLatin1String("FA")) && isDigits(resp.mid(2), 11), repr(resp));

    resp = p.query(QStringLiteral("PS"));
    r.check(QStringLiteral("14.3 PS; via PTY → PS1"),
            resp == QLatin1String("PS1"), repr(resp));
}
#else
void section14pty(Runner& r, const QString& ptyPath)
{
    r.section(QStringLiteral("Section 14 — PTY round-trip (Unix only)"));
    for (const char* name : { "14.1 PTY ID;", "14.2 PTY FA;", "14.3 PTY PS;" })
        r.skip(QString::fromLatin1(name), QStringLiteral("Unix only"));
    (void)ptyPath;
}
#endif

// ═════════════════════════════════════════════════════════════════════════════
// Section 15 — Squelch, NR/NL/NT/RL, MG, FW, OI, UP/DN, stubs
// ═════════════════════════════════════════════════════════════════════════════

void section15(CatClient& c, Runner& r)
{
    r.section(QStringLiteral("Section 15 — SQ / NR / NL / NT / RL / MG / FW / OI / UP / DN / stubs"));

    // ── SQ: Squelch ──────────────────────────────────────────────────────────
    // MacLoggerDX uses "SQ0;" (P1 channel selector = 0) to poll squelch.
    QString resp = c.query(QStringLiteral("SQ"));
    r.check(QStringLiteral("15.1  SQ; → SQ0 + 3-digit level"),
            resp.startsWith(QLatin1String("SQ0")) && isDigits(resp.mid(3), 3), repr(resp));

    resp = c.query(QStringLiteral("SQ0"));
    r.check(QStringLiteral("15.2  SQ0; (P1 form) → SQ0 + 3-digit level"),
            resp.startsWith(QLatin1String("SQ0")) && isDigits(resp.mid(3), 3), repr(resp));

    QString origSq = resp;
    c.send(QStringLiteral("SQ0050"));
    QThread::msleep(100);
    resp = c.query(QStringLiteral("SQ0"));
    r.check(QStringLiteral("15.3  set SQ0050 → SQ0; confirms SQ0050"),
            resp == QLatin1String("SQ0050"), repr(resp));

    if (origSq.startsWith(QLatin1String("SQ"))) { c.send(origSq); QThread::msleep(50); }

    // ── NR: Noise Reduction (0=off, 1=NR1, 2=NR2) ────────────────────────────
    QString origNr = c.query(QStringLiteral("NR"));
    r.check(QStringLiteral("15.4  NR; → NR0, NR1, or NR2"),
            origNr == QLatin1String("NR0") || origNr == QLatin1String("NR1") ||
            origNr == QLatin1String("NR2"), repr(origNr));

    c.send(QStringLiteral("NR1"));
    QThread::msleep(100);
    resp = c.query(QStringLiteral("NR"));
    r.check(QStringLiteral("15.5  set NR1 → NR; confirms NR1"),
            resp == QLatin1String("NR1"), repr(resp));

    c.send(QStringLiteral("NR0"));
    QThread::msleep(50);

    // ── NL: Noise Blanker level (001-010) ─────────────────────────────────────
    QString origNl = c.query(QStringLiteral("NL"));
    r.check(QStringLiteral("15.6  NL; → NL + 3-digit level (001-010)"),
            origNl.startsWith(QLatin1String("NL")) && isDigits(origNl.mid(2), 3) &&
            origNl.mid(2).toInt() >= 1 && origNl.mid(2).toInt() <= 10, repr(origNl));

    c.send(QStringLiteral("NL005"));
    QThread::msleep(100);
    resp = c.query(QStringLiteral("NL"));
    r.check(QStringLiteral("15.7  set NL005 → NL; confirms NL005"),
            resp == QLatin1String("NL005"), repr(resp));

    if (origNl.startsWith(QLatin1String("NL"))) { c.send(origNl); QThread::msleep(50); }

    // ── NT: Auto Notch / ANF (0=off, 1=on) ───────────────────────────────────
    QString origNt = c.query(QStringLiteral("NT"));
    r.check(QStringLiteral("15.8  NT; → NT0 or NT1"),
            origNt == QLatin1String("NT0") || origNt == QLatin1String("NT1"), repr(origNt));

    QString newNt = (origNt == QLatin1String("NT0")) ? QStringLiteral("NT1") : QStringLiteral("NT0");
    c.send(newNt);
    QThread::msleep(100);
    resp = c.query(QStringLiteral("NT"));
    r.check(QStringLiteral("15.9  toggle NT → confirms %1").arg(newNt),
            resp == newNt, repr(resp));

    c.send(origNt);
    QThread::msleep(50);

    // ── RL: Noise Reduction level (00-09) ────────────────────────────────────
    QString origRl = c.query(QStringLiteral("RL"));
    r.check(QStringLiteral("15.10 RL; → RL + 2-digit level (00-09)"),
            origRl.startsWith(QLatin1String("RL")) && isDigits(origRl.mid(2), 2) &&
            origRl.mid(2).toInt() <= 9, repr(origRl));

    c.send(QStringLiteral("RL04"));
    QThread::msleep(100);
    resp = c.query(QStringLiteral("RL"));
    r.check(QStringLiteral("15.11 set RL04 → RL; confirms RL04"),
            resp == QLatin1String("RL04"), repr(resp));

    if (origRl.startsWith(QLatin1String("RL"))) { c.send(origRl); QThread::msleep(50); }

    // ── MG: Microphone gain (000-100) ─────────────────────────────────────────
    QString origMg = c.query(QStringLiteral("MG"));
    r.check(QStringLiteral("15.12 MG; → MG + 3-digit gain"),
            origMg.startsWith(QLatin1String("MG")) && isDigits(origMg.mid(2), 3), repr(origMg));

    c.send(QStringLiteral("MG050"));
    QThread::msleep(100);
    resp = c.query(QStringLiteral("MG"));
    r.check(QStringLiteral("15.13 set MG050 → MG; confirms MG050"),
            resp == QLatin1String("MG050"), repr(resp));

    if (origMg.startsWith(QLatin1String("MG"))) { c.send(origMg); QThread::msleep(50); }

    // ── FW: Filter width (valid for CW/FM/AM; returns ?; for SSB) ────────────
    // Radio is typically in USB at this point in tests; expect ?;.
    resp = c.query(QStringLiteral("FW"));
    r.check(QStringLiteral("15.14 FW; in SSB mode → ?, or FW + 4-digit Hz in CW/FM/AM"),
            resp == QLatin1String("?") ||
            (resp.startsWith(QLatin1String("FW")) && isDigits(resp.mid(2), 4)), repr(resp));

    // ── OI: Opposite IF (VFO B composite status, same layout as IF) ───────────
    resp = c.query(QStringLiteral("OI"));
    r.check(QStringLiteral("15.15 OI; → OI + 35-char body"),
            resp.startsWith(QLatin1String("OI")) && resp.size() == 37, repr(resp));

    if (resp.size() == 37) {
        IfFields f = parseIfBody(resp.mid(2));
        r.check(QStringLiteral("15.16 OI body valid (freq + status fields)"),
                f.valid && isDigits(f.freq_hz, 11), repr(resp.mid(2)));
    } else {
        r.skip(QStringLiteral("15.16 OI body valid"), QStringLiteral("OI body wrong length"));
    }

    // ── BY: Busy indicator ────────────────────────────────────────────────────
    resp = c.query(QStringLiteral("BY"));
    r.check(QStringLiteral("15.17 BY; → BY + 2 chars (P1P2, each 0 or 1)"),
            resp.startsWith(QLatin1String("BY")) && resp.size() == 4 &&
            (resp[2] == '0' || resp[2] == '1') &&
            (resp[3] == '0' || resp[3] == '1'), repr(resp));

    // ── Stubs: RA, PA, RM, LK, TY ─────────────────────────────────────────────
    resp = c.query(QStringLiteral("RA"));
    r.check(QStringLiteral("15.18 RA; → RA0000 (attenuator off, no sub-RX)"),
            resp == QLatin1String("RA0000"), repr(resp));

    resp = c.query(QStringLiteral("PA"));
    r.check(QStringLiteral("15.19 PA; → PA00 (preamp off)"),
            resp == QLatin1String("PA00"), repr(resp));

    resp = c.query(QStringLiteral("RM0"));
    r.check(QStringLiteral("15.20 RM0; → RM + 5 digits (meter stub)"),
            resp.startsWith(QLatin1String("RM")) && isDigits(resp.mid(2), 5), repr(resp));

    resp = c.query(QStringLiteral("LK"));
    r.check(QStringLiteral("15.21 LK; → LK00 (key lock off)"),
            resp == QLatin1String("LK00"), repr(resp));

    resp = c.query(QStringLiteral("TY"));
    r.check(QStringLiteral("15.22 TY; → TY format (3-char body: 2 spaces + digit)"),
            resp.startsWith(QLatin1String("TY")) && resp.size() == 5 &&
            resp[2] == ' ' && resp[3] == ' ' &&
            (resp[4] == '0' || resp[4] == '1' || resp[4] == '2'), repr(resp));

    // ── UP / DN: VFO step ─────────────────────────────────────────────────────
    QString faBase = c.query(QStringLiteral("FA"));
    qint64 baseHz = 0;
    if (faBase.startsWith(QLatin1String("FA")) && isDigits(faBase.mid(2), 11))
        baseHz = faBase.mid(2).toLongLong();

    c.send(QStringLiteral("UP"));
    QThread::msleep(100);
    QString faUp = c.query(QStringLiteral("FA"));
    qint64 upHz = 0;
    if (faUp.startsWith(QLatin1String("FA")) && isDigits(faUp.mid(2), 11))
        upHz = faUp.mid(2).toLongLong();
    r.check(QStringLiteral("15.23 UP; → FA increases by one step"),
            baseHz > 0 && upHz > baseHz, repr(faUp));

    c.send(QStringLiteral("DN"));
    QThread::msleep(100);
    QString faDn = c.query(QStringLiteral("FA"));
    qint64 dnHz = 0;
    if (faDn.startsWith(QLatin1String("FA")) && isDigits(faDn.mid(2), 11))
        dnHz = faDn.mid(2).toLongLong();
    r.check(QStringLiteral("15.24 DN; → FA returns to base frequency"),
            dnHz == baseHz, repr(faDn));
}

} // namespace

// ═════════════════════════════════════════════════════════════════════════════
// main
// ═════════════════════════════════════════════════════════════════════════════

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("smartcat_ts2000_test"));

#if defined(Q_OS_UNIX)
    g_tty = isatty(STDOUT_FILENO);
#endif

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("AetherSDR TS-2000 dialect CAT integration test"));
    parser.addHelpOption();
    parser.addOption({ QStringLiteral("host"),    QStringLiteral("CAT host"), QStringLiteral("HOST"), QStringLiteral("localhost") });
    parser.addOption({ QStringLiteral("port"),    QStringLiteral("TS-2000 CAT port"), QStringLiteral("PORT"), QStringLiteral("5002") });
    parser.addOption({ QStringLiteral("timeout"), QStringLiteral("Read timeout (ms)"), QStringLiteral("MS"), QStringLiteral("3000") });
    parser.addOption({ QStringLiteral("ptt"),     QStringLiteral("Enable PTT tests (needs dummy load)") });
    parser.addOption({ QStringLiteral("cw"),      QStringLiteral("Enable CW keyer tests (needs antenna/dummy load)") });
    const QString defaultPty2 = defaultPtyPath(2);
    parser.addOption({ QStringLiteral("pty"),     QStringLiteral("PTY device path for PTY round-trip test (default: %1)").arg(defaultPty2),
                       QStringLiteral("PATH"),    defaultPty2 });
    parser.process(app);

    const QString host    = parser.value(QStringLiteral("host"));
    const quint16 port    = static_cast<quint16>(parser.value(QStringLiteral("port")).toUInt());
    const int     timeout = parser.value(QStringLiteral("timeout")).toInt();
    const bool    doPtt   = parser.isSet(QStringLiteral("ptt"));
    const bool    doCw    = parser.isSet(QStringLiteral("cw"));

    std::cout << '\n' << bold(QStringLiteral("AetherSDR TS-2000 CAT Test Suite")).toStdString() << '\n'
              << "Connecting to " << host.toStdString() << ':' << port << " ...\n";

    CatClient c(timeout);
    if (!c.connectToServer(host, port)) {
        std::cerr << red(QStringLiteral("Connection failed.")).toStdString() << '\n'
                  << yellow(QStringLiteral("Ensure AetherSDR is running with a TS-2000 port at the given port.")).toStdString() << '\n';
        return EXIT_FAILURE;
    }
    std::cout << green(QStringLiteral("Connected.")).toStdString() << '\n';
    if (doPtt)
        std::cout << yellow(QStringLiteral("⚠  PTT tests enabled — ensure a dummy load or antenna is connected")).toStdString() << '\n';
    if (doCw)
        std::cout << yellow(QStringLiteral("⚠  CW tests enabled — radio will transmit")).toStdString() << '\n';

    Runner r;

    qint64 origHz   = 14'225'000;
    QString origMode = QStringLiteral("2");
    {
        QString faResp = c.query(QStringLiteral("FA"));
        if (faResp.startsWith(QLatin1String("FA")) && isDigits(faResp.mid(2), 11))
            origHz = faResp.mid(2).toLongLong();
        QString mdResp = c.query(QStringLiteral("MD"));
        if (mdResp.startsWith(QLatin1String("MD")))
            origMode = mdResp.mid(2);
    }

    section1(c, r);
    section2(c, r, origHz);
    section3(c, r);
    section4(c, r, origMode);
    section5(c, r);
    section6(c, r);
    section7(c, r);
    section8(c, r);
    if (doPtt) { section9ptt(c, r); } else { section9skip(r); }
    section10(c, r);
    section11(c, r);
    section12(c, r);
    if (doCw) { section13cw(c, r); } else { section13skip(r); }
    section14pty(r, parser.value(QStringLiteral("pty")));
    section15(c, r);

    // Restore radio state
    c.send(QStringLiteral("AI0"));
    c.send(QStringLiteral("FT0"));
    c.send(QStringLiteral("RT0"));
    c.send(QStringLiteral("RC"));
    c.send(QStringLiteral("XT0"));
    c.send(QStringLiteral("RX"));
    c.send(QStringLiteral("FA") + hz11(origHz));
    c.send(QStringLiteral("MD") + origMode);

    c.close();
    return r.summary() ? EXIT_SUCCESS : EXIT_FAILURE;
}
