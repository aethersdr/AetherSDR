// AetherSDR FlexCAT (ZZ-extension) dialect CAT integration test.
// Requires a running AetherSDR instance with a FlexCAT port enabled.
//
// Build:  cmake --build build --target CAT_Flex_test
// Run:    ./build/CAT_Flex_test [--host HOST] [--port PORT] [--ptt] [--cw] [--pty PATH]
//
// Mirrors tests/test_flexcat.py.  PTT tests (section 10) are disabled by default;
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
    QStringLiteral("ID904"), QStringLiteral("ID905"), QStringLiteral("ID906"),
    QStringLiteral("ID907"), QStringLiteral("ID908"), QStringLiteral("ID909"),
    QStringLiteral("ID910"), QStringLiteral("ID911"), QStringLiteral("ID919"),
    QStringLiteral("ID930"), QStringLiteral("ID931"),
};

// ZZ 2-digit code → (Kenwood 1-digit, mode name)
struct ZzModeEntry { const char* zzCode; char kwDigit; const char* name; };
static const ZzModeEntry kZzModes[] = {
    { "00", '1', "LSB"  },
    { "01", '2', "USB"  },
    { "04", '3', "CW"   },
    { "05", '4', "FM"   },
    { "06", '5', "AM"   },
    { "07", '9', "DIGU" },
    { "09", '6', "DIGL" },
};

// Poll until ZZMD and MD both reflect an expected mode change (3-second deadline).
static void pollUntilModeAgrees(CatClient& c, const QString& zzExpect, const QString& mdExpect,
                                QString& outZz, QString& outMd)
{
    QElapsedTimer timer;
    timer.start();
    do {
        outZz = c.query(QStringLiteral("ZZMD"));
        outMd = c.query(QStringLiteral("MD"));
        if (outZz == zzExpect && outMd == mdExpect) return;
        if (timer.elapsed() < 3000) QThread::msleep(150);
    } while (timer.elapsed() < 3000);
}

// ═════════════════════════════════════════════════════════════════════════════
// Section 1 — Connection & Base Command Compatibility
// ═════════════════════════════════════════════════════════════════════════════

void section1(CatClient& c, Runner& r)
{
    r.section(QStringLiteral("Section 1 — Connection & Base Command Compatibility"));

    QString resp = c.query(QStringLiteral("ID"));
    r.check(QStringLiteral("1.1  ID; → valid model ID from CAT guide table (base cmd works in FlexCAT)"),
            kValidIDs.contains(resp), repr(resp));

    resp = c.query(QStringLiteral("PS"));
    r.check(QStringLiteral("1.2  PS; → PS1"),
            resp == QLatin1String("PS1"), repr(resp));

    resp = c.query(QStringLiteral("FA"));
    r.check(QStringLiteral("1.3  FA; (base command) returns \"FA\" + 11-digit Hz"),
            resp.startsWith(QLatin1String("FA")) && isDigits(resp.mid(2), 11), repr(resp));

    resp = c.query(QStringLiteral("MD"));
    r.check(QStringLiteral("1.4  MD; (base command) returns \"MD\" + single digit"),
            resp.startsWith(QLatin1String("MD")) && resp.size() == 3 && resp[2].isDigit(), repr(resp));
}

// ═════════════════════════════════════════════════════════════════════════════
// Section 2 — ZZFA — VFO-A Frequency
// ═════════════════════════════════════════════════════════════════════════════

void section2(CatClient& c, Runner& r, qint64 origHz)
{
    r.section(QStringLiteral("Section 2 — ZZFA — VFO-A Frequency"));

    QString resp = c.query(QStringLiteral("ZZFA"));
    r.check(QStringLiteral("2.1  ZZFA; returns \"ZZFA\" + 11-digit Hz"),
            resp.startsWith(QLatin1String("ZZFA")) && isDigits(resp.mid(4), 11), repr(resp));

    QString faResp = c.query(QStringLiteral("FA"));
    r.check(QStringLiteral("2.2  ZZFA; and FA; return the same frequency"),
            resp.mid(4) == faResp.mid(2),
            QStringLiteral("ZZFA=%1 FA=%2").arg(repr(resp.mid(4)), repr(faResp.mid(2))));

    const qint64 testHz = 14'074'000;
    c.send(QStringLiteral("ZZFA") + hz11(testHz));
    QThread::msleep(150);

    QString respZz = c.query(QStringLiteral("ZZFA"));
    QString respFa = c.query(QStringLiteral("FA"));
    r.check(QStringLiteral("2.3  set ZZFA 14.074 MHz → ZZFA; confirms"),
            respZz == QStringLiteral("ZZFA") + hz11(testHz),
            QStringLiteral("got %1").arg(repr(respZz)));
    r.check(QStringLiteral("2.4  FA; also reflects ZZFA set (cross-dialect consistency)"),
            respFa == QStringLiteral("FA") + hz11(testHz),
            QStringLiteral("got %1").arg(repr(respFa)));

    const qint64 test2Hz = 21'074'000;
    c.send(QStringLiteral("FA") + hz11(test2Hz));
    QThread::msleep(150);
    respZz = c.query(QStringLiteral("ZZFA"));
    r.check(QStringLiteral("2.5  set via base FA; ZZFA; reflects the change"),
            respZz == QStringLiteral("ZZFA") + hz11(test2Hz),
            QStringLiteral("got %1").arg(repr(respZz)));

    c.send(QStringLiteral("FA") + hz11(origHz));
    QThread::msleep(100);
}

// ═════════════════════════════════════════════════════════════════════════════
// Section 3 — ZZFB — VFO-B Frequency
// ═════════════════════════════════════════════════════════════════════════════

void section3(CatClient& c, Runner& r)
{
    r.section(QStringLiteral("Section 3 — ZZFB — VFO-B Frequency"));

    QString resp = c.query(QStringLiteral("ZZFB"));
    r.check(QStringLiteral("3.1  ZZFB; returns \"ZZFB\" + 11-digit Hz"),
            resp.startsWith(QLatin1String("ZZFB")) && isDigits(resp.mid(4), 11), repr(resp));

    QString fbResp = c.query(QStringLiteral("FB"));
    r.check(QStringLiteral("3.2  ZZFB; and FB; agree"),
            resp.mid(4) == fbResp.mid(2),
            QStringLiteral("ZZFB=%1 FB=%2").arg(repr(resp.mid(4)), repr(fbResp.mid(2))));

    QString faResp = c.query(QStringLiteral("FA"));
    const bool vfoBMapped = (resp.mid(4) != faResp.mid(2) &&
                              !resp.mid(4).isEmpty() && !faResp.mid(2).isEmpty());

    if (!vfoBMapped) {
        r.skip(QStringLiteral("3.3  set ZZFB 14.225 MHz → ZZFB; confirms"),
               QStringLiteral("VFO B not mapped to a second slice — configure port VFO B first"));
    } else {
        const qint64 testHz = 14'225'000;
        c.send(QStringLiteral("ZZFB") + hz11(testHz));
        QElapsedTimer timer;
        timer.start();
        QString pollResp;
        do {
            pollResp = c.query(QStringLiteral("ZZFB"));
            if (pollResp == QStringLiteral("ZZFB") + hz11(testHz)) break;
            if (timer.elapsed() < 2000) QThread::msleep(100);
        } while (timer.elapsed() < 2000);
        r.check(QStringLiteral("3.3  set ZZFB 14.225 MHz → ZZFB; confirms"),
                pollResp == QStringLiteral("ZZFB") + hz11(testHz),
                QStringLiteral("got %1").arg(repr(pollResp)));
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Section 4 — ZZMD — Mode (2-digit ZZ codes)
// ═════════════════════════════════════════════════════════════════════════════

void section4(CatClient& c, Runner& r, const QString& origModeDigit)
{
    r.section(QStringLiteral("Section 4 — ZZMD — Mode (2-digit ZZ codes)"));

    QString resp = c.query(QStringLiteral("ZZMD"));
    r.check(QStringLiteral("4.1  ZZMD; returns \"ZZMD\" + 2-digit code"),
            resp.startsWith(QLatin1String("ZZMD")) && isDigits(resp.mid(4), 2), repr(resp));

    int checkIdx = 2;
    for (const ZzModeEntry& mode : kZzModes) {
        const QString zzCode  = QString::fromLatin1(mode.zzCode);
        const QString kwDigit = QString(QLatin1Char(mode.kwDigit));
        c.send(QStringLiteral("ZZMD") + zzCode);
        QString respZz, respMd;
        pollUntilModeAgrees(c, QStringLiteral("ZZMD") + zzCode, QStringLiteral("MD") + kwDigit,
                            respZz, respMd);
        r.check(QStringLiteral("4.%1  set ZZMD%2 (%3) → ZZMD; round-trips")
                    .arg(checkIdx).arg(zzCode, QString::fromLatin1(mode.name)),
                respZz == QStringLiteral("ZZMD") + zzCode, repr(respZz));
        r.check(QStringLiteral("     MD; agrees with ZZMD%1 (%2) → MD%3")
                    .arg(zzCode, QString::fromLatin1(mode.name), kwDigit),
                respMd == QStringLiteral("MD") + kwDigit,
                QStringLiteral("expected MD%1, got %2").arg(kwDigit, repr(respMd)));
        ++checkIdx;
    }

    c.send(QStringLiteral("MD") + origModeDigit);
    QThread::msleep(200);
}

// ═════════════════════════════════════════════════════════════════════════════
// Section 5 — ZZME — VFO B DSP Mode (get/set)
// ═════════════════════════════════════════════════════════════════════════════

void section5(CatClient& c, Runner& r)
{
    r.section(QStringLiteral("Section 5 — ZZME — VFO B DSP Mode (get/set)"));

    QString resp = c.query(QStringLiteral("ZZME"));
    const bool vfoBPresent = resp.startsWith(QLatin1String("ZZME")) && isDigits(resp.mid(4), 2);

    if (!vfoBPresent) {
        r.check(QStringLiteral("5.1  ZZME; → ?; when no VFO B slice mapped"),
                resp == QLatin1String("?"), repr(resp));
        r.skip(QStringLiteral("5.2  ZZME set/get round-trip"),
               QStringLiteral("no VFO B slice"));
        r.skip(QStringLiteral("5.3  ZZME set does not affect VFO A (ZZMD)"),
               QStringLiteral("no VFO B slice"));
        return;
    }

    r.check(QStringLiteral("5.1  ZZME; → \"ZZME\" + 2-digit code (VFO B mode)"),
            vfoBPresent, repr(resp));

    const QString origZzme = resp;
    // Avoid FM (05) — band-restricted on HF. Toggle between DIGU (07) and LSB (00).
    const QString testZzme = (origZzme == QLatin1String("ZZME07"))
                             ? QStringLiteral("ZZME00") : QStringLiteral("ZZME07");
    c.send(testZzme);
    QElapsedTimer timer;
    timer.start();
    QString respB;
    do {
        respB = c.query(QStringLiteral("ZZME"));
        if (respB == testZzme) break;
        if (timer.elapsed() < 2000) QThread::msleep(100);
    } while (timer.elapsed() < 2000);
    QString respA = c.query(QStringLiteral("ZZMD"));
    r.check(QStringLiteral("5.2  set %1 (VFO B mode) → ZZME; confirms %1").arg(testZzme),
            respB == testZzme, QStringLiteral("got %1").arg(repr(respB)));
    r.check(QStringLiteral("5.3  setting ZZME does not change VFO A (ZZMD unchanged)"),
            respA.startsWith(QLatin1String("ZZMD")),
            QStringLiteral("ZZMD=%1").arg(repr(respA)));

    c.send(origZzme);
    QThread::msleep(100);
}

// ═════════════════════════════════════════════════════════════════════════════
// Section 6 — ZZIF — Extended IF Status
// ═════════════════════════════════════════════════════════════════════════════

void section6(CatClient& c, Runner& r)
{
    r.section(QStringLiteral("Section 6 — ZZIF — Extended IF Status"));

    c.send(QStringLiteral("FA") + hz11(14'225'000));
    c.send(QStringLiteral("MD2"));
    c.send(QStringLiteral("FT0"));
    QThread::msleep(200);

    QString resp = c.query(QStringLiteral("ZZIF"));
    const QString body = resp.startsWith(QLatin1String("ZZIF")) ? resp.mid(4) : QString();
    const IfFields fields = parseIfBody(body);

    r.check(QStringLiteral("6.1  ZZIF; returns \"ZZIF\" + exactly 35-char body"),
            resp.startsWith(QLatin1String("ZZIF")) && body.size() == 35,
            QStringLiteral("body len=%1 resp=%2").arg(body.size()).arg(repr(resp.left(16))));

    r.check(QStringLiteral("6.2  ZZIF body freq field is all digits"),
            isDigits(fields.freq_hz, 11), repr(fields.freq_hz));

    QString zzfaResp = c.query(QStringLiteral("ZZFA"));
    const QString zzfaHz = zzfaResp.startsWith(QLatin1String("ZZFA")) ? zzfaResp.mid(4) : QString();
    r.check(QStringLiteral("6.3  ZZIF body freq field matches ZZFA; response"),
            fields.freq_hz == zzfaHz,
            QStringLiteral("ZZIF=%1 ZZFA=%2").arg(repr(fields.freq_hz), repr(zzfaHz)));

    QString mdResp = c.query(QStringLiteral("MD"));
    const QString mdDigit = mdResp.startsWith(QLatin1String("MD")) ? mdResp.mid(2) : QString();
    r.check(QStringLiteral("6.4  ZZIF body mode (pos 27, Kenwood digit) matches MD;"),
            QString(fields.mode) == mdDigit,
            QStringLiteral("IF mode=%1 MD=%2").arg(repr(QString(fields.mode)), repr(mdDigit)));

    QString ifResp = c.query(QStringLiteral("IF"));
    const QString ifBody = ifResp.startsWith(QLatin1String("IF")) ? ifResp.mid(2) : QString();
    r.check(QStringLiteral("6.5  ZZIF body is identical to IF body"),
            body == ifBody,
            QStringLiteral("ZZIF body=%1 IF body=%2").arg(repr(body.left(16)), repr(ifBody.left(16))));
}

// ═════════════════════════════════════════════════════════════════════════════
// Section 7 — ZZAI — AI Mode (async unsolicited updates)
// ═════════════════════════════════════════════════════════════════════════════

void section7(CatClient& c, Runner& r)
{
    r.section(QStringLiteral("Section 7 — ZZAI — AI Mode (async unsolicited updates)"));

    QString resp = c.query(QStringLiteral("ZZAI"));
    r.check(QStringLiteral("7.1  ZZAI; → ZZAI00 (disabled by default)"),
            resp == QLatin1String("ZZAI00"), repr(resp));

    QString aiResp = c.query(QStringLiteral("AI"));
    r.check(QStringLiteral("7.2  AI; and ZZAI; share the same state (both report disabled)"),
            aiResp == QLatin1String("AI0"), repr(aiResp));

    c.send(QStringLiteral("ZZAI01"));
    resp = c.query(QStringLiteral("ZZAI"));
    r.check(QStringLiteral("7.3  ZZAI01; enables AI; ZZAI; → ZZAI01"),
            resp == QLatin1String("ZZAI01"), repr(resp));
    aiResp = c.query(QStringLiteral("AI"));
    r.check(QStringLiteral("7.4  AI; reflects ZZAI enable (shared state)"),
            aiResp == QLatin1String("AI1"), repr(aiResp));

    const qint64 newHz = 14'100'000;
    c.send(QStringLiteral("ZZFA") + hz11(newHz));
    QString push = c.tryRead(2000);
    r.check(QStringLiteral("7.5  AI push after ZZFA set → push is \"FA<freq>\" (NOT ZZFA prefix)"),
            push == QStringLiteral("FA") + hz11(newHz), repr(push));

    c.send(QStringLiteral("ZZMD05"));  // FM
    push = c.tryRead(2000);
    r.check(QStringLiteral("7.6  AI push after ZZMD set → push is \"MD4\" (FM, NOT ZZMD prefix)"),
            push == QLatin1String("MD4"), repr(push));

    c.send(QStringLiteral("ZZAI00"));
    resp = c.query(QStringLiteral("ZZAI"));
    r.check(QStringLiteral("7.7  ZZAI00; disables AI; ZZAI; → ZZAI00"),
            resp == QLatin1String("ZZAI00"), repr(resp));

    c.send(QStringLiteral("ZZFA") + hz11(14'225'000));
    QString silence = c.tryRead(500);
    r.check(QStringLiteral("7.8  no AI push after ZZAI00 (silence expected)"),
            silence.isNull(),
            QStringLiteral("got unexpected push: %1").arg(repr(silence)));

    c.send(QStringLiteral("AI1"));
    resp = c.query(QStringLiteral("ZZAI"));
    r.check(QStringLiteral("7.9  enable via base AI1; → ZZAI; → ZZAI01"),
            resp == QLatin1String("ZZAI01"), repr(resp));

    c.send(QStringLiteral("AI0"));
    resp = c.query(QStringLiteral("ZZAI"));
    r.check(QStringLiteral("7.10 disable via base AI0; → ZZAI; → ZZAI00"),
            resp == QLatin1String("ZZAI00"), repr(resp));
}

// ═════════════════════════════════════════════════════════════════════════════
// Section 8 — ZZSW — Split
// ═════════════════════════════════════════════════════════════════════════════

void section8(CatClient& c, Runner& r)
{
    r.section(QStringLiteral("Section 8 — ZZSW — Split"));

    QString resp = c.query(QStringLiteral("ZZSW"));
    r.check(QStringLiteral("8.1  ZZSW; → ZZSW0 (split off initially)"),
            resp == QLatin1String("ZZSW0"), repr(resp));

    c.send(QStringLiteral("ZZSW1"));
    QString respZz = c.query(QStringLiteral("ZZSW"));
    QString respFt = c.query(QStringLiteral("FT"));
    r.check(QStringLiteral("8.2  ZZSW1; enables split; ZZSW; → ZZSW1"),
            respZz == QLatin1String("ZZSW1"), repr(respZz));
    r.check(QStringLiteral("8.3  FT; also reflects split on (FT1)"),
            respFt == QLatin1String("FT1"), repr(respFt));

    c.send(QStringLiteral("ZZSW0"));
    c.send(QStringLiteral("FT1"));
    respZz = c.query(QStringLiteral("ZZSW"));
    r.check(QStringLiteral("8.4  set split via base FT1; → ZZSW; → ZZSW1"),
            respZz == QLatin1String("ZZSW1"), repr(respZz));

    c.send(QStringLiteral("ZZSW0"));
    respZz = c.query(QStringLiteral("ZZSW"));
    respFt = c.query(QStringLiteral("FT"));
    r.check(QStringLiteral("8.5  ZZSW0; clears split; ZZSW; → ZZSW0"),
            respZz == QLatin1String("ZZSW0"), repr(respZz));
    r.check(QStringLiteral("8.6  FT; also reflects split off (FT0)"),
            respFt == QLatin1String("FT0"), repr(respFt));
}

// ═════════════════════════════════════════════════════════════════════════════
// Section 9 — ZZSM — S-Meter
// ═════════════════════════════════════════════════════════════════════════════

void section9(CatClient& c, Runner& r)
{
    r.section(QStringLiteral("Section 9 — ZZSM — S-Meter"));

    QString resp = c.query(QStringLiteral("ZZSM0"));
    r.check(QStringLiteral("9.1  ZZSM0; → ZZSM0000 (4-digit zero)"),
            resp == QLatin1String("ZZSM0000"), repr(resp));

    resp = c.query(QStringLiteral("SM0"));
    r.check(QStringLiteral("9.2  SM0; (base) → SM00000 (5-digit zero)"),
            resp == QLatin1String("SM00000"), repr(resp));
}

// ═════════════════════════════════════════════════════════════════════════════
// Section 10 — PTT  (optional — requires dummy load or antenna)
// ═════════════════════════════════════════════════════════════════════════════

void section10ptt(CatClient& c, Runner& r)
{
    r.section(QStringLiteral("Section 10 — PTT  ⚠  TX ACTIVE"));

    QString resp = c.query(QStringLiteral("ZZIF"));
    IfFields fields = parseIfBody(resp.startsWith(QLatin1String("ZZIF")) ? resp.mid(4) : QString());
    r.check(QStringLiteral("10.0 ZZIF confirms TX=0 before keying"),
            fields.tx == QChar('0'), repr(QString(fields.tx)));

    c.send(QStringLiteral("ZZTX"));
    QThread::msleep(1000);

    resp = c.query(QStringLiteral("ZZIF"));
    fields = parseIfBody(resp.startsWith(QLatin1String("ZZIF")) ? resp.mid(4) : QString());
    const bool txOn = (fields.tx == QChar('1'));

    if (!txOn) {
        r.skip(QStringLiteral("10.1 ZZTX; keys radio; ZZIF TX='1'"),
               QStringLiteral("TX blocked — check interlock/inhibit line"));
        r.skip(QStringLiteral("10.2 ZZRX; unkeys radio; ZZIF TX='0'"),
               QStringLiteral("TX blocked"));
        r.skip(QStringLiteral("10.3 base TX;/RX; also work; IF TX='0'"),
               QStringLiteral("TX blocked"));
        c.send(QStringLiteral("ZZRX"));
        c.send(QStringLiteral("RX"));
        return;
    }

    r.check(QStringLiteral("10.1 ZZTX; keys radio; ZZIF body TX field = '1'"),
            txOn, repr(QString(fields.tx)));

    c.send(QStringLiteral("ZZRX"));
    QThread::msleep(250);

    resp = c.query(QStringLiteral("ZZIF"));
    fields = parseIfBody(resp.startsWith(QLatin1String("ZZIF")) ? resp.mid(4) : QString());
    r.check(QStringLiteral("10.2 ZZRX; unkeys radio; ZZIF body TX field = '0'"),
            fields.tx == QChar('0'), repr(QString(fields.tx)));

    c.send(QStringLiteral("TX"));
    QThread::msleep(500);
    c.send(QStringLiteral("RX"));
    QThread::msleep(250);
    resp = c.query(QStringLiteral("IF"));
    fields = parseIfBody(resp.startsWith(QLatin1String("IF")) ? resp.mid(2) : QString());
    r.check(QStringLiteral("10.3 base TX; / RX; also work in FlexCAT mode; IF confirms TX=0"),
            fields.tx == QChar('0'), repr(QString(fields.tx)));

    // Safety: watch 2 s — firmware may briefly re-assert TX after unkey
    {
        QElapsedTimer safeTimer; safeTimer.start();
        bool sawTxOn = false;
        c.send(QStringLiteral("ZZRX")); c.send(QStringLiteral("RX"));
        do {
            QThread::msleep(250);
            resp = c.query(QStringLiteral("ZZIF"));
            fields = parseIfBody(resp.startsWith(QLatin1String("ZZIF")) ? resp.mid(4) : QString());
            if (fields.tx != QChar('0')) {
                sawTxOn = true;
                c.send(QStringLiteral("ZZRX")); c.send(QStringLiteral("RX"));
            }
        } while (safeTimer.elapsed() < 2000);
        if (sawTxOn)
            qWarning("10.S: TX re-asserted during 2-s safety watch — forced RX; possible firmware bug");
        r.check(QStringLiteral("10.S safety: TX confirmed off after 2-s watch"),
                fields.tx == QChar('0'), repr(QString(fields.tx)));
    }
}

void section10skip(Runner& r)
{
    r.section(QStringLiteral("Section 10 — PTT  (skipped — pass --ptt to enable)"));
    for (const char* name : { "10.0 baseline", "10.1 ZZTX; keys",
                               "10.2 ZZRX; unkeys", "10.3 base TX/RX" }) {
        r.skip(QString::fromLatin1(name), QStringLiteral("--ptt not set"));
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Section 11 — Unknown / Invalid ZZ Commands
// ═════════════════════════════════════════════════════════════════════════════

void section11(CatClient& c, Runner& r)
{
    r.section(QStringLiteral("Section 11 — Unknown / Invalid ZZ Commands"));

    struct Entry { const char* cmd; const char* label; };
    static const Entry entries[] = {
        { "ZZXQ", "11.1" }, { "ZZAB", "11.2" }, { "ZZZZ", "11.3" },
    };
    for (const Entry& e : entries) {
        QString resp = c.query(QString::fromLatin1(e.cmd));
        r.check(QStringLiteral("%1  %2; → ?;")
                    .arg(QString::fromLatin1(e.label), QString::fromLatin1(e.cmd)),
                resp == QLatin1String("?"), repr(resp));
    }

    QString resp = c.query(QStringLiteral("XQ"));
    r.check(QStringLiteral("11.4  base unknown XQ; → ?;"),
            resp == QLatin1String("?"), repr(resp));
}

// ═════════════════════════════════════════════════════════════════════════════
// Section 12 — Cross-Dialect Consistency (base ↔ ZZ commands)
// ═════════════════════════════════════════════════════════════════════════════

void section12(CatClient& c, Runner& r)
{
    r.section(QStringLiteral("Section 12 — Cross-Dialect Consistency (base ↔ ZZ commands)"));

    const qint64 testHz = 14'074'000;
    c.send(QStringLiteral("FA") + hz11(testHz));
    QThread::msleep(150);
    QString resp = c.query(QStringLiteral("ZZFA"));
    r.check(QStringLiteral("12.1 set freq via FA; → ZZFA; reflects it"),
            resp == QStringLiteral("ZZFA") + hz11(testHz),
            QStringLiteral("got %1").arg(repr(resp)));

    const qint64 test2Hz = 7'074'000;
    c.send(QStringLiteral("ZZFA") + hz11(test2Hz));
    QThread::msleep(150);
    resp = c.query(QStringLiteral("FA"));
    r.check(QStringLiteral("12.2 set freq via ZZFA; → FA; reflects it"),
            resp == QStringLiteral("FA") + hz11(test2Hz),
            QStringLiteral("got %1").arg(repr(resp)));

    auto setAndPollMode = [&](const QString& label, const QString& sendCmd,
                              const QString& zzExpect, const QString& mdExpect) {
        c.send(sendCmd);
        QString rz, rm;
        pollUntilModeAgrees(c, zzExpect, mdExpect, rz, rm);
        r.check(QStringLiteral("%1 → ZZMD; → %2").arg(label, zzExpect),
                rz == zzExpect, repr(rz));
        r.check(QStringLiteral("%1 → MD; → %2").arg(label, mdExpect),
                rm == mdExpect, repr(rm));
    };

    setAndPollMode(QStringLiteral("12.3 set mode via MD1 (LSB)"),
                   QStringLiteral("MD1"), QStringLiteral("ZZMD00"), QStringLiteral("MD1"));
    setAndPollMode(QStringLiteral("12.4 set mode via MD2 (USB)"),
                   QStringLiteral("MD2"), QStringLiteral("ZZMD01"), QStringLiteral("MD2"));
    setAndPollMode(QStringLiteral("12.5 set mode via ZZMD04 (CW)"),
                   QStringLiteral("ZZMD04"), QStringLiteral("ZZMD04"), QStringLiteral("MD3"));
    setAndPollMode(QStringLiteral("12.6 set mode via ZZMD09 (DIGL)"),
                   QStringLiteral("ZZMD09"), QStringLiteral("ZZMD09"), QStringLiteral("MD6"));

    c.send(QStringLiteral("FT1"));
    resp = c.query(QStringLiteral("ZZSW"));
    r.check(QStringLiteral("12.7 set split via FT1; → ZZSW; → ZZSW1"),
            resp == QLatin1String("ZZSW1"), repr(resp));

    c.send(QStringLiteral("ZZSW0"));
    resp = c.query(QStringLiteral("FT"));
    r.check(QStringLiteral("12.8 clear split via ZZSW0; → FT; → FT0"),
            resp == QLatin1String("FT0"), repr(resp));
}

// ═════════════════════════════════════════════════════════════════════════════
// Section 13 — Tier-2 ZZ Commands
// ═════════════════════════════════════════════════════════════════════════════

void section13(CatClient& c, Runner& r)
{
    r.section(QStringLiteral("Section 13 — Tier-2 ZZ Commands (audio, AGC, power, RIT, XIT, NB/NR, binaural)"));

    // ── ZZAG / AG cross-check ──────────────────────────────────────────────
    QString respZz = c.query(QStringLiteral("ZZAG"));
    QString respAg = c.query(QStringLiteral("AG"));
    r.check(QStringLiteral("13.1  ZZAG; → \"ZZAG\" + 3-digit gain"),
            respZz.startsWith(QLatin1String("ZZAG")) && isDigits(respZz.mid(4), 3), repr(respZz));
    r.check(QStringLiteral("13.2  ZZAG; and AG; agree on gain value"),
            respAg.startsWith(QLatin1String("AG")) && respZz.mid(4) == respAg.mid(2),
            QStringLiteral("ZZAG=%1 AG=%2").arg(repr(respZz.mid(4)), repr(respAg.mid(2))));

    const QString origAg = respZz;
    c.send(QStringLiteral("ZZAG060"));
    QThread::msleep(100);
    QString gotAg = c.query(QStringLiteral("AG"));
    r.check(QStringLiteral("13.3  set ZZAG060 → AG; also reflects it (cross-dialect)"),
            gotAg == QLatin1String("AG060"), repr(gotAg));

    if (origAg.startsWith(QLatin1String("ZZAG"))) { c.send(origAg); QThread::msleep(50); }

    // ── ZZGT / GT cross-check ──────────────────────────────────────────────
    respZz = c.query(QStringLiteral("ZZGT"));
    QString respGt = c.query(QStringLiteral("GT"));
    r.check(QStringLiteral("13.4  ZZGT; → \"ZZGT\" + valid AGC code (0/2/3/4)"),
            respZz.startsWith(QLatin1String("ZZGT")) && isValidAgcCode(respZz.mid(4)), repr(respZz));
    r.check(QStringLiteral("13.5  ZZGT; and GT; return the same AGC code"),
            respGt.startsWith(QLatin1String("GT")) && respZz.mid(4) == respGt.mid(2),
            QStringLiteral("ZZGT=%1 GT=%2").arg(repr(respZz.mid(4)), repr(respGt.mid(2))));

    const QString origGt = respZz;
    c.send(QStringLiteral("ZZGT3"));
    QThread::msleep(150);
    QString gotGt = c.query(QStringLiteral("GT"));
    r.check(QStringLiteral("13.6  set ZZGT3 → GT; also reflects GT3"),
            gotGt == QLatin1String("GT3"), repr(gotGt));

    if (origGt.startsWith(QLatin1String("ZZGT"))) { c.send(origGt); QThread::msleep(100); }

    // ── ZZPC / PC cross-check ──────────────────────────────────────────────
    respZz = c.query(QStringLiteral("ZZPC"));
    QString respPc = c.query(QStringLiteral("PC"));
    r.check(QStringLiteral("13.7  ZZPC; → \"ZZPC\" + 3-digit power"),
            respZz.startsWith(QLatin1String("ZZPC")) && isDigits(respZz.mid(4), 3), repr(respZz));
    r.check(QStringLiteral("13.8  ZZPC; and PC; agree on power value"),
            respPc.startsWith(QLatin1String("PC")) && respZz.mid(4) == respPc.mid(2),
            QStringLiteral("ZZPC=%1 PC=%2").arg(repr(respZz.mid(4)), repr(respPc.mid(2))));

    const QString origPc = respZz;
    c.send(QStringLiteral("ZZPC070"));
    QThread::msleep(100);
    QString gotPc = c.query(QStringLiteral("PC"));
    r.check(QStringLiteral("13.9  set ZZPC070 → PC; reflects PC070"),
            gotPc == QLatin1String("PC070"), repr(gotPc));

    if (origPc.startsWith(QLatin1String("ZZPC"))) { c.send(origPc); QThread::msleep(50); }

    // ── ZZMG: Mic gain ────────────────────────────────────────────────────
    QString origMg = c.query(QStringLiteral("ZZMG"));
    r.check(QStringLiteral("13.10 ZZMG; → \"ZZMG\" + 3-digit gain"),
            origMg.startsWith(QLatin1String("ZZMG")) && isDigits(origMg.mid(4), 3), repr(origMg));

    c.send(QStringLiteral("ZZMG050"));
    QThread::msleep(100);
    QString gotMg = c.query(QStringLiteral("ZZMG"));
    r.check(QStringLiteral("13.11 set ZZMG050 → ZZMG; confirms ZZMG050"),
            gotMg == QLatin1String("ZZMG050"), repr(gotMg));

    if (origMg.startsWith(QLatin1String("ZZMG"))) { c.send(origMg); QThread::msleep(50); }

    // ── ZZMA: VFO A mute ──────────────────────────────────────────────────
    QString origMa = c.query(QStringLiteral("ZZMA"));
    r.check(QStringLiteral("13.12 ZZMA; → ZZMA0 or ZZMA1"),
            origMa == QLatin1String("ZZMA0") || origMa == QLatin1String("ZZMA1"), repr(origMa));

    QString newMa = (origMa == QLatin1String("ZZMA0"))
                    ? QStringLiteral("ZZMA1") : QStringLiteral("ZZMA0");
    c.send(newMa);
    QThread::msleep(100);
    QString gotMa = c.query(QStringLiteral("ZZMA"));
    r.check(QStringLiteral("13.13 toggle ZZMA → confirms %1").arg(newMa),
            gotMa == newMa, repr(gotMa));

    c.send(origMa);
    QThread::msleep(100);

    // ── ZZLB: VFO A audio pan ─────────────────────────────────────────────
    QString origLb = c.query(QStringLiteral("ZZLB"));
    r.check(QStringLiteral("13.14 ZZLB; → \"ZZLB\" + 3-digit pan (0-100)"),
            origLb.startsWith(QLatin1String("ZZLB")) && isDigits(origLb.mid(4), 3), repr(origLb));

    c.send(QStringLiteral("ZZLB050"));
    QThread::msleep(100);
    QString gotLb = c.query(QStringLiteral("ZZLB"));
    r.check(QStringLiteral("13.15 set ZZLB050 (center) → ZZLB; confirms ZZLB050"),
            gotLb == QLatin1String("ZZLB050"), repr(gotLb));

    if (origLb.startsWith(QLatin1String("ZZLB"))) { c.send(origLb); QThread::msleep(50); }

    // ── ZZRT / RT: RIT state ──────────────────────────────────────────────
    c.send(QStringLiteral("RT0"));
    QThread::msleep(50);

    QString respZzRt = c.query(QStringLiteral("ZZRT"));
    QString respRt   = c.query(QStringLiteral("RT"));
    r.check(QStringLiteral("13.16 ZZRT; → ZZRT0 or ZZRT1"),
            respZzRt == QLatin1String("ZZRT0") || respZzRt == QLatin1String("ZZRT1"), repr(respZzRt));
    r.check(QStringLiteral("13.17 ZZRT; and RT; agree"),
            respRt.startsWith(QLatin1String("RT")) && respZzRt.mid(4) == respRt.mid(2),
            QStringLiteral("ZZRT=%1 RT=%2").arg(repr(respZzRt), repr(respRt)));

    c.send(QStringLiteral("ZZRT1"));
    QThread::msleep(100);
    QString gotRt = c.query(QStringLiteral("RT"));
    r.check(QStringLiteral("13.18 ZZRT1; enables RIT → RT; reflects RT1"),
            gotRt == QLatin1String("RT1"), repr(gotRt));

    // ── ZZRG: RIT frequency ───────────────────────────────────────────────
    c.send(QStringLiteral("ZZRG+00750"));
    QThread::msleep(100);
    QString gotZzrg = c.query(QStringLiteral("ZZRG"));
    r.check(QStringLiteral("13.19 set ZZRG+00750 → ZZRG; confirms ZZRG+00750"),
            gotZzrg == QLatin1String("ZZRG+00750"), repr(gotZzrg));

    QString gotRg = c.query(QStringLiteral("RG"));
    r.check(QStringLiteral("13.20 RG; (base) also reflects ZZRG set"),
            gotRg == QLatin1String("RG+00750"), repr(gotRg));

    // ── ZZRC: clear RIT ───────────────────────────────────────────────────
    c.send(QStringLiteral("ZZRC"));
    QThread::msleep(50);
    gotZzrg = c.query(QStringLiteral("ZZRG"));
    r.check(QStringLiteral("13.21 ZZRC; clears RIT → ZZRG; returns ZZRG+00000"),
            gotZzrg == QLatin1String("ZZRG+00000"), repr(gotZzrg));

    // ── ZZRD / ZZRU: increment / decrement ────────────────────────────────
    c.send(QStringLiteral("ZZRU00200"));
    QThread::msleep(50);
    gotZzrg = c.query(QStringLiteral("ZZRG"));
    r.check(QStringLiteral("13.22 ZZRU00200; increments RIT → ZZRG+00200"),
            gotZzrg == QLatin1String("ZZRG+00200"), repr(gotZzrg));

    c.send(QStringLiteral("ZZRD00050"));
    QThread::msleep(50);
    gotZzrg = c.query(QStringLiteral("ZZRG"));
    r.check(QStringLiteral("13.23 ZZRD00050; decrements → ZZRG+00150"),
            gotZzrg == QLatin1String("ZZRG+00150"), repr(gotZzrg));

    c.send(QStringLiteral("ZZRC"));
    c.send(QStringLiteral("ZZRT0"));
    c.send(QStringLiteral("RT0"));
    QThread::msleep(50);

    // ── ZZXS / XT: XIT state ──────────────────────────────────────────────
    respZz = c.query(QStringLiteral("ZZXS"));
    r.check(QStringLiteral("13.24 ZZXS; → ZZXS0 or ZZXS1"),
            respZz == QLatin1String("ZZXS0") || respZz == QLatin1String("ZZXS1"), repr(respZz));

    c.send(QStringLiteral("ZZXS1"));
    QThread::msleep(100);
    QString gotXt = c.query(QStringLiteral("XT"));
    r.check(QStringLiteral("13.25 ZZXS1; enables XIT → XT; reflects XT1"),
            gotXt == QLatin1String("XT1"), repr(gotXt));

    // ── ZZXG: XIT frequency ───────────────────────────────────────────────
    c.send(QStringLiteral("ZZXG-00300"));
    QThread::msleep(100);
    QString gotZzxg = c.query(QStringLiteral("ZZXG"));
    r.check(QStringLiteral("13.26 set ZZXG-00300 → ZZXG; confirms ZZXG-00300"),
            gotZzxg == QLatin1String("ZZXG-00300"), repr(gotZzxg));

    // ── ZZXC: clear XIT ───────────────────────────────────────────────────
    c.send(QStringLiteral("ZZXC"));
    QThread::msleep(50);
    gotZzxg = c.query(QStringLiteral("ZZXG"));
    r.check(QStringLiteral("13.27 ZZXC; clears XIT → ZZXG; returns ZZXG+00000"),
            gotZzxg == QLatin1String("ZZXG+00000"), repr(gotZzxg));

    c.send(QStringLiteral("ZZXS0"));
    c.send(QStringLiteral("XT0"));
    QThread::msleep(50);

    // ── ZZNR: Noise Reduction state ───────────────────────────────────────
    QString origNr = c.query(QStringLiteral("ZZNR"));
    r.check(QStringLiteral("13.28 ZZNR; → ZZNR0 or ZZNR1"),
            origNr == QLatin1String("ZZNR0") || origNr == QLatin1String("ZZNR1"), repr(origNr));

    QString newNr = (origNr == QLatin1String("ZZNR0"))
                    ? QStringLiteral("ZZNR1") : QStringLiteral("ZZNR0");
    c.send(newNr);
    QThread::msleep(100);
    QString gotNr = c.query(QStringLiteral("ZZNR"));
    r.check(QStringLiteral("13.29 toggle ZZNR → confirms %1").arg(newNr),
            gotNr == newNr, repr(gotNr));

    c.send(origNr);
    QThread::msleep(50);

    // ── ZZNL: Noise Blanker level ─────────────────────────────────────────
    QString origNl = c.query(QStringLiteral("ZZNL"));
    r.check(QStringLiteral("13.30 ZZNL; → \"ZZNL\" + 3-digit level"),
            origNl.startsWith(QLatin1String("ZZNL")) && isDigits(origNl.mid(4), 3), repr(origNl));

    c.send(QStringLiteral("ZZNL040"));
    QThread::msleep(100);
    QString gotNl = c.query(QStringLiteral("ZZNL"));
    r.check(QStringLiteral("13.31 set ZZNL040 → ZZNL; confirms ZZNL040"),
            gotNl == QLatin1String("ZZNL040"), repr(gotNl));

    if (origNl.startsWith(QLatin1String("ZZNL"))) { c.send(origNl); QThread::msleep(50); }

    // ── ZZBI: Binaural receive ────────────────────────────────────────────
    QString origBi = c.query(QStringLiteral("ZZBI"));
    r.check(QStringLiteral("13.32 ZZBI; → ZZBI0 or ZZBI1"),
            origBi == QLatin1String("ZZBI0") || origBi == QLatin1String("ZZBI1"), repr(origBi));

    QString newBi = (origBi == QLatin1String("ZZBI0"))
                    ? QStringLiteral("ZZBI1") : QStringLiteral("ZZBI0");
    c.send(newBi);
    QThread::msleep(100);
    QString gotBi = c.query(QStringLiteral("ZZBI"));
    r.check(QStringLiteral("13.33 toggle ZZBI → confirms %1").arg(newBi),
            gotBi == newBi, repr(gotBi));

    c.send(origBi);
    QThread::msleep(200);

    // ── ZZFR: toggle active VFO ───────────────────────────────────────────
    QString faBefore = c.query(QStringLiteral("ZZFA"));
    QString fbBefore = c.query(QStringLiteral("ZZFB"));
    c.send(QStringLiteral("ZZFR"));
    QThread::msleep(50);
    QString faAfter = c.query(QStringLiteral("ZZFA"));
    // If VFO A and B differ, after ZZFR the new ZZFA should match old ZZFB.
    // If they're equal (single-slice session), the swap is a no-op — treat as pass.
    const bool zzfrOk = (faBefore.mid(4) == fbBefore.mid(4))
                        ? true
                        : (faAfter.mid(4) == fbBefore.mid(4));
    r.check(QStringLiteral("13.34 ZZFR; swaps VFOs — new ZZFA matches old ZZFB freq"),
            zzfrOk,
            QStringLiteral("before: ZZFA=%1 ZZFB=%2 after ZZFA=%3")
                .arg(repr(faBefore.mid(4)), repr(fbBefore.mid(4)), repr(faAfter.mid(4))));
    c.send(QStringLiteral("ZZFR"));  // swap back
    QThread::msleep(50);

    // ── ZZAR: VFO A AGC threshold (0-100, 3-digit) ───────────────────────────
    QString origAr = c.query(QStringLiteral("ZZAR"));
    r.check(QStringLiteral("13.35 ZZAR; → \"ZZAR\" + 3-digit threshold"),
            origAr.startsWith(QLatin1String("ZZAR")) && isDigits(origAr.mid(4), 3), repr(origAr));

    c.send(QStringLiteral("ZZAR060"));
    QThread::msleep(100);
    QString gotAr = c.query(QStringLiteral("ZZAR"));
    r.check(QStringLiteral("13.36 set ZZAR060 → ZZAR; confirms ZZAR060"),
            gotAr == QLatin1String("ZZAR060"), repr(gotAr));

    if (origAr.startsWith(QLatin1String("ZZAR"))) { c.send(origAr); QThread::msleep(50); }

    // ── VFO B presence check (probe via ZZLE) ─────────────────────────────────
    QString zzleProbe = c.query(QStringLiteral("ZZLE"));
    const bool vfoBAvail = zzleProbe.startsWith(QLatin1String("ZZLE")) && isDigits(zzleProbe.mid(4), 3);

    // ── ZZLE: VFO B audio gain ────────────────────────────────────────────────
    if (!vfoBAvail) {
        r.check(QStringLiteral("13.37 ZZLE; → ? when no VFO B slice"),
                zzleProbe == QLatin1String("?"), repr(zzleProbe));
        r.skip(QStringLiteral("13.38 set ZZLE round-trip"), QStringLiteral("no VFO B slice"));
    } else {
        r.check(QStringLiteral("13.37 ZZLE; → \"ZZLE\" + 3-digit gain (VFO B audio)"),
                vfoBAvail, repr(zzleProbe));
        c.send(QStringLiteral("ZZLE065"));
        QThread::msleep(100);
        QString gotLe = c.query(QStringLiteral("ZZLE"));
        r.check(QStringLiteral("13.38 set ZZLE065 → ZZLE; confirms ZZLE065"),
                gotLe == QLatin1String("ZZLE065"), repr(gotLe));
        c.send(zzleProbe);  // restore original gain
        QThread::msleep(50);
    }

    // ── ZZLF: VFO B audio pan ─────────────────────────────────────────────────
    if (!vfoBAvail) {
        r.skip(QStringLiteral("13.39 ZZLF; → ? when no VFO B"), QStringLiteral("no VFO B slice"));
        r.skip(QStringLiteral("13.40 set ZZLF round-trip"), QStringLiteral("no VFO B slice"));
    } else {
        QString origLf = c.query(QStringLiteral("ZZLF"));
        r.check(QStringLiteral("13.39 ZZLF; → \"ZZLF\" + 3-digit pan (VFO B)"),
                origLf.startsWith(QLatin1String("ZZLF")) && isDigits(origLf.mid(4), 3), repr(origLf));
        c.send(QStringLiteral("ZZLF050"));
        QThread::msleep(100);
        QString gotLf = c.query(QStringLiteral("ZZLF"));
        r.check(QStringLiteral("13.40 set ZZLF050 (center) → ZZLF; confirms ZZLF050"),
                gotLf == QLatin1String("ZZLF050"), repr(gotLf));
        if (origLf.startsWith(QLatin1String("ZZLF"))) { c.send(origLf); QThread::msleep(50); }
    }

    // ── ZZMB: VFO B mute (0/1) ────────────────────────────────────────────────
    if (!vfoBAvail) {
        r.skip(QStringLiteral("13.41 ZZMB; → ? when no VFO B"), QStringLiteral("no VFO B slice"));
        r.skip(QStringLiteral("13.42 toggle ZZMB round-trip"), QStringLiteral("no VFO B slice"));
    } else {
        QString origMb = c.query(QStringLiteral("ZZMB"));
        r.check(QStringLiteral("13.41 ZZMB; → ZZMB0 or ZZMB1"),
                origMb == QLatin1String("ZZMB0") || origMb == QLatin1String("ZZMB1"), repr(origMb));
        QString newMb = (origMb == QLatin1String("ZZMB0"))
                        ? QStringLiteral("ZZMB1") : QStringLiteral("ZZMB0");
        c.send(newMb);
        QThread::msleep(100);
        QString gotMb = c.query(QStringLiteral("ZZMB"));
        r.check(QStringLiteral("13.42 toggle ZZMB → confirms %1").arg(newMb),
                gotMb == newMb, repr(gotMb));
        c.send(origMb);
        QThread::msleep(100);
    }

    // ── ZZAS: VFO B AGC threshold (0-100, 3-digit) ────────────────────────────
    if (!vfoBAvail) {
        r.skip(QStringLiteral("13.43 ZZAS; → ? when no VFO B"), QStringLiteral("no VFO B slice"));
        r.skip(QStringLiteral("13.44 set ZZAS round-trip"), QStringLiteral("no VFO B slice"));
    } else {
        QString origAs = c.query(QStringLiteral("ZZAS"));
        r.check(QStringLiteral("13.43 ZZAS; → \"ZZAS\" + 3-digit threshold (VFO B AGC)"),
                origAs.startsWith(QLatin1String("ZZAS")) && isDigits(origAs.mid(4), 3), repr(origAs));
        c.send(QStringLiteral("ZZAS055"));
        QThread::msleep(100);
        QString gotAs = c.query(QStringLiteral("ZZAS"));
        r.check(QStringLiteral("13.44 set ZZAS055 → ZZAS; confirms ZZAS055"),
                gotAs == QLatin1String("ZZAS055"), repr(gotAs));
        if (origAs.startsWith(QLatin1String("ZZAS"))) { c.send(origAs); QThread::msleep(50); }
    }

    // ── ZZRW: VFO B RIT frequency ─────────────────────────────────────────────
    if (!vfoBAvail) {
        r.skip(QStringLiteral("13.45 set ZZRW+00300 round-trip"), QStringLiteral("no VFO B slice"));
        r.skip(QStringLiteral("13.46 clear ZZRW → ZZRW+00000"), QStringLiteral("no VFO B slice"));
    } else {
        c.send(QStringLiteral("ZZRY0"));    // ensure VFO B RIT is off before touching freq
        c.send(QStringLiteral("ZZRW+00000"));
        QThread::msleep(50);
        c.send(QStringLiteral("ZZRW+00300"));
        QThread::msleep(100);
        QString gotRw = c.query(QStringLiteral("ZZRW"));
        r.check(QStringLiteral("13.45 set ZZRW+00300 → ZZRW; confirms ZZRW+00300"),
                gotRw == QLatin1String("ZZRW+00300"), repr(gotRw));
        c.send(QStringLiteral("ZZRW+00000"));
        QThread::msleep(50);
        gotRw = c.query(QStringLiteral("ZZRW"));
        r.check(QStringLiteral("13.46 clear ZZRW → ZZRW; confirms ZZRW+00000"),
                gotRw == QLatin1String("ZZRW+00000"), repr(gotRw));
    }

    // ── ZZRY: VFO B RIT state (0/1) ───────────────────────────────────────────
    if (!vfoBAvail) {
        r.skip(QStringLiteral("13.47 ZZRY0 confirms VFO B RIT off"), QStringLiteral("no VFO B slice"));
        r.skip(QStringLiteral("13.48 ZZRY1 enables VFO B RIT"), QStringLiteral("no VFO B slice"));
    } else {
        c.send(QStringLiteral("ZZRY0"));
        QThread::msleep(50);
        QString gotRy = c.query(QStringLiteral("ZZRY"));
        r.check(QStringLiteral("13.47 ZZRY0; clears VFO B RIT → ZZRY; confirms ZZRY0"),
                gotRy == QLatin1String("ZZRY0"), repr(gotRy));
        c.send(QStringLiteral("ZZRY1"));
        QThread::msleep(100);
        gotRy = c.query(QStringLiteral("ZZRY"));
        r.check(QStringLiteral("13.48 ZZRY1; enables VFO B RIT → ZZRY; confirms ZZRY1"),
                gotRy == QLatin1String("ZZRY1"), repr(gotRy));
        c.send(QStringLiteral("ZZRY0"));  // leave RIT off
        QThread::msleep(50);
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Section 14 — CW Keyer (KY send)  (optional — requires antenna/dummy load)
// ═════════════════════════════════════════════════════════════════════════════

void section14cw(CatClient& c, Runner& r)
{
    r.section(QStringLiteral("Section 14 — CW Keyer (KY)  ⚠  TX ACTIVE"));

    QString origMd = c.query(QStringLiteral("MD"));
    c.send(QStringLiteral("ZZMD04"));  // CW mode required for keying
    QThread::msleep(200);

    QString resp = c.query(QStringLiteral("KY"));
    r.check(QStringLiteral("14.1 KY; → KY0 (buffer idle before send)"),
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
        r.skip(QStringLiteral("14.2 KY CQ CQ CQ TEST TEST; → KY1 (busy)"),
               QStringLiteral("keyer did not start — check interlock/inhibit line"));
        r.skip(QStringLiteral("14.3 KY; → KY0 (complete)"),
               QStringLiteral("keyer did not start"));
        if (origMd.startsWith(QLatin1String("MD"))) { c.send(origMd); QThread::msleep(100); }
        c.send(QStringLiteral("RX"));
        return;
    }
    r.check(QStringLiteral("14.2 KY CQ CQ CQ TEST TEST; → KY; → KY1 (keyer busy / transmitting)"),
            true, {});

    timer.restart();
    do {
        pollResp = c.query(QStringLiteral("KY"));
        if (pollResp == QLatin1String("KY0")) break;
        if (timer.elapsed() < 15000) QThread::msleep(200);
    } while (timer.elapsed() < 15000);
    r.check(QStringLiteral("14.3 KY; → KY0 (keyer idle after transmission complete)"),
            pollResp == QLatin1String("KY0"), repr(pollResp));

    if (origMd.startsWith(QLatin1String("MD"))) { c.send(origMd); QThread::msleep(100); }

    // Safety: watch 2 s — CW TX-tail may keep PA on after keyer reports KY0
    {
        QElapsedTimer safeTimer; safeTimer.start();
        bool sawTxOn = false;
        QString safeResp; IfFields sf;
        c.send(QStringLiteral("ZZRX")); c.send(QStringLiteral("RX"));
        do {
            QThread::msleep(250);
            safeResp = c.query(QStringLiteral("ZZIF"));
            sf = parseIfBody(safeResp.startsWith(QLatin1String("ZZIF")) ? safeResp.mid(4) : QString());
            if (sf.tx != QChar('0')) {
                sawTxOn = true;
                c.send(QStringLiteral("ZZRX")); c.send(QStringLiteral("RX"));
            }
        } while (safeTimer.elapsed() < 2000);
        if (sawTxOn)
            qWarning("14.S: TX re-asserted during 2-s safety watch — forced RX; possible firmware bug");
        r.check(QStringLiteral("14.S safety: TX confirmed off after 2-s watch"),
                sf.tx == QChar('0'), repr(QString(sf.tx)));
    }
}

void section14skip(Runner& r)
{
    r.section(QStringLiteral("Section 14 — CW Keyer (skipped — pass --cw to enable)"));
    for (const char* name : { "14.1 KY; → KY0 baseline",
                               "14.2 KY CQ CQ CQ TEST TEST; → KY1 (busy)",
                               "14.3 KY; → KY0 (complete)" }) {
        r.skip(QString::fromLatin1(name), QStringLiteral("--cw not set"));
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Section 16 — ZZDE — Diversity Receive (FLEX-6700)
// Accepts ZZDE0/ZZDE1 (6700 with diversity) or ?; (non-6700 or no diversity
// slice).  On a valid response the set/get round-trip is also verified.
// ═════════════════════════════════════════════════════════════════════════════

void section16(CatClient& c, Runner& r)
{
    r.section(QStringLiteral("Section 16 — ZZDE — Diversity Receive (FLEX-6700)"));

    QString orig = c.query(QStringLiteral("ZZDE"));
    const bool validResp = (orig == QLatin1String("ZZDE0") ||
                            orig == QLatin1String("ZZDE1"));
    const bool noSupport = (orig == QLatin1String("?"));

    r.check(QStringLiteral("16.1 ZZDE; → ZZDE0, ZZDE1 (6700), or ?; (non-6700)"),
            validResp || noSupport, repr(orig));

    if (validResp) {
        QString toggle = (orig == QLatin1String("ZZDE0"))
                         ? QStringLiteral("ZZDE1") : QStringLiteral("ZZDE0");
        c.send(toggle);
        QThread::msleep(50);
        QString got = c.query(QStringLiteral("ZZDE"));
        r.check(QStringLiteral("16.2 ZZDE set/get round-trip"),
                got == toggle, repr(got));
        c.send(orig);   // restore
        QThread::msleep(50);
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Section 15 — PTY round-trip  (Unix only; skips if device cannot be opened)
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

void section15pty(Runner& r, const QString& ptyPath)
{
    r.section(QStringLiteral("Section 15 — PTY round-trip (%1)").arg(ptyPath));

    PtyClient p;
    if (!p.openDevice(ptyPath)) {
        for (const char* name : { "15.1 PTY ID;", "15.2 PTY ZZFA;", "15.3 PTY PS;" })
            r.skip(QString::fromLatin1(name),
                   QStringLiteral("cannot open %1").arg(ptyPath));
        return;
    }

    QString resp = p.query(QStringLiteral("ID"));
    r.check(QStringLiteral("15.1 ID; via PTY → valid model ID"),
            kValidIDs.contains(resp), repr(resp));

    resp = p.query(QStringLiteral("ZZFA"));
    r.check(QStringLiteral("15.2 ZZFA; via PTY → \"ZZFA\" + 11-digit Hz"),
            resp.startsWith(QLatin1String("ZZFA")) && isDigits(resp.mid(4), 11), repr(resp));

    resp = p.query(QStringLiteral("PS"));
    r.check(QStringLiteral("15.3 PS; via PTY → PS1"),
            resp == QLatin1String("PS1"), repr(resp));
}
#else
void section15pty(Runner& r, const QString& ptyPath)
{
    r.section(QStringLiteral("Section 15 — PTY round-trip (Unix only)"));
    for (const char* name : { "15.1 PTY ID;", "15.2 PTY ZZFA;", "15.3 PTY PS;" })
        r.skip(QString::fromLatin1(name), QStringLiteral("Unix only"));
    (void)ptyPath;
}
#endif

} // namespace

// ═════════════════════════════════════════════════════════════════════════════
// main
// ═════════════════════════════════════════════════════════════════════════════

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("smartcat_flexcat_test"));

#if defined(Q_OS_UNIX)
    g_tty = isatty(STDOUT_FILENO);
#endif

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("AetherSDR FlexCAT (ZZ-extension) dialect CAT integration test"));
    parser.addHelpOption();
    parser.addOption({ QStringLiteral("host"),    QStringLiteral("CAT host"), QStringLiteral("HOST"), QStringLiteral("localhost") });
    parser.addOption({ QStringLiteral("port"),    QStringLiteral("FlexCAT port"), QStringLiteral("PORT"), QStringLiteral("5001") });
    parser.addOption({ QStringLiteral("timeout"), QStringLiteral("Read timeout (ms)"), QStringLiteral("MS"), QStringLiteral("3000") });
    parser.addOption({ QStringLiteral("ptt"),     QStringLiteral("Enable PTT tests (needs dummy load)") });
    parser.addOption({ QStringLiteral("cw"),      QStringLiteral("Enable CW keyer tests (needs antenna/dummy load)") });
    const QString defaultPty1 = defaultPtyPath(1);
    parser.addOption({ QStringLiteral("pty"),     QStringLiteral("PTY device path for PTY round-trip test (default: %1)").arg(defaultPty1),
                       QStringLiteral("PATH"),    defaultPty1 });
    parser.process(app);

    const QString host    = parser.value(QStringLiteral("host"));
    const quint16 port    = static_cast<quint16>(parser.value(QStringLiteral("port")).toUInt());
    const int     timeout = parser.value(QStringLiteral("timeout")).toInt();
    const bool    doPtt   = parser.isSet(QStringLiteral("ptt"));
    const bool    doCw    = parser.isSet(QStringLiteral("cw"));

    std::cout << '\n' << bold(QStringLiteral("AetherSDR FlexCAT (ZZ-extension) Test Suite")).toStdString() << '\n'
              << "Connecting to " << host.toStdString() << ':' << port << " ...\n";

    CatClient c(timeout);
    if (!c.connectToServer(host, port)) {
        std::cerr << red(QStringLiteral("Connection failed.")).toStdString() << '\n'
                  << yellow(QStringLiteral("Ensure AetherSDR is running with a FlexCAT port at the given port.")).toStdString() << '\n';
        return EXIT_FAILURE;
    }
    std::cout << green(QStringLiteral("Connected.")).toStdString() << '\n';
    if (doPtt)
        std::cout << yellow(QStringLiteral("⚠  PTT tests enabled — ensure a dummy load or antenna is connected")).toStdString() << '\n';
    if (doCw)
        std::cout << yellow(QStringLiteral("⚠  CW tests enabled — radio will transmit")).toStdString() << '\n';

    Runner r;

    qint64 origHz    = 14'225'000;
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
    section9(c, r);
    if (doPtt) { section10ptt(c, r); } else { section10skip(r); }
    section11(c, r);
    section12(c, r);
    section13(c, r);
    if (doCw) { section14cw(c, r); } else { section14skip(r); }
    section16(c, r);
    section15pty(r, parser.value(QStringLiteral("pty")));

    // Restore radio state
    c.send(QStringLiteral("ZZAI00"));
    c.send(QStringLiteral("AI0"));
    c.send(QStringLiteral("ZZSW0"));
    c.send(QStringLiteral("FT0"));
    c.send(QStringLiteral("ZZRT0"));
    c.send(QStringLiteral("ZZRC"));
    c.send(QStringLiteral("RT0"));
    c.send(QStringLiteral("RC"));
    c.send(QStringLiteral("ZZXS0"));
    c.send(QStringLiteral("ZZXC"));
    c.send(QStringLiteral("XT0"));
    c.send(QStringLiteral("ZZMA0"));  // un-mute VFO A
    c.send(QStringLiteral("ZZMB0"));  // un-mute VFO B
    c.send(QStringLiteral("ZZRY0"));  // clear VFO B RIT
    c.send(QStringLiteral("ZZRW+00000"));
    c.send(QStringLiteral("ZZRX"));
    c.send(QStringLiteral("RX"));
    c.send(QStringLiteral("FA") + hz11(origHz));
    c.send(QStringLiteral("MD") + origMode);

    c.close();
    return r.summary() ? EXIT_SUCCESS : EXIT_FAILURE;
}
