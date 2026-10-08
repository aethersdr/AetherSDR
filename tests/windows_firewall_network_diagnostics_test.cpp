// WindowsFirewall's verdicts and Fix command, and NetworkDiagnostics' failure
// logging. Both are pure enough to pin on any platform: assess() reads a
// Status a test can build, and watch() reads a QNetworkReply a test can fake.

#include "core/NetworkDiagnostics.h"
#include "core/WindowsFirewall.h"

#include <QCoreApplication>
#include <QNetworkReply>
#include <QSslError>
#include <QStringList>
#include <QTimer>

#include <cstdio>

using namespace AetherSDR;
using namespace AetherSDR::WindowsFirewall;

namespace {

int g_failed = 0;

void check(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", what);
    if (!ok) ++g_failed;
}

QStringList g_lines;
QtMessageHandler g_previous = nullptr;

void capture(QtMsgType type, const QMessageLogContext& ctx, const QString& msg)
{
    if (ctx.category && QByteArray(ctx.category) == "aether.network") {
        g_lines << msg;
    }
    if (g_previous) g_previous(type, ctx, msg);
}

class FakeReply : public QNetworkReply {
public:
    FakeReply(const QUrl& url, NetworkError error, const QString& text, int http = 0)
    {
        setUrl(url);
        setError(error, text);
        if (http) setAttribute(QNetworkRequest::HttpStatusCodeAttribute, http);
        setOpenMode(QIODevice::ReadOnly);
    }
    void finish() { setFinished(true); emit finished(); }
    void raiseSsl(const QList<QSslError>& errors) { emit sslErrors(errors); }
    void abort() override {}
protected:
    qint64 readData(char*, qint64) override { return -1; }
};

Status windows(int profiles = kProfilePrivate)
{
    Status s;
    s.inspected = true;
    s.programPath = QStringLiteral("C:\\Program Files\\AetherSDR\\AetherSDR.exe");
    s.currentProfiles = profiles;
    s.enabledOnCurrent = true;
    return s;
}

Rule rule(bool allow, int protocol, int profiles, bool inbound = true, bool enabled = true)
{
    Rule r;
    r.name = QStringLiteral("aethersdr.exe");
    r.allow = allow;
    r.protocol = protocol;
    r.profiles = profiles;
    r.inbound = inbound;
    r.enabled = enabled;
    return r;
}

void testVerdicts()
{
    check(assess(Status{}).verdict == Verdict::NotInspected, "not inspected off Windows");

    Status third = windows();
    third.thirdPartyFirewalls << QStringLiteral("Norton Smart Firewall");
    const Assessment t = assess(third);
    check(t.verdict == Verdict::ThirdParty && !t.fixable
              && t.summary.contains(QStringLiteral("Norton Smart Firewall")),
          "a third-party firewall is named and not offered Fix");

    Status off = windows();
    off.enabledOnCurrent = false;
    check(assess(off).verdict == Verdict::Disabled, "firewall off for the active network");

    Status blocked = windows(kProfilePublic);
    blocked.rules << rule(false, 17, kProfilePublic) << rule(true, 256, 0x7fffffff);
    const Assessment b = assess(blocked);
    check(b.verdict == Verdict::Blocked && b.fixable && b.details.size() == 1,
          "a block rule on the active profile beats an allow rule and is fixable");

    Status otherProfile = windows(kProfilePrivate);
    otherProfile.rules << rule(false, 17, kProfilePublic)
                       << rule(true, 6, kProfilePrivate) << rule(true, 17, kProfilePrivate);
    check(assess(otherProfile).verdict == Verdict::Allowed,
          "a block rule on an inactive profile does not block");

    Status disabledBlock = windows();
    disabledBlock.rules << rule(false, 256, 0x7fffffff, true, false);
    check(assess(disabledBlock).verdict == Verdict::NoAllowRule,
          "a disabled block rule does not block");

    Status outbound = windows();
    outbound.outboundBlockedByDefault = true;
    outbound.rules << rule(true, 256, 0x7fffffff);
    check(assess(outbound).verdict == Verdict::OutboundBlocked,
          "outbound-blocked default with no outbound allow is reported");
    outbound.rules << rule(true, 256, 0x7fffffff, false);
    check(assess(outbound).verdict == Verdict::Allowed,
          "an outbound allow rule satisfies an outbound-blocked default");

    Status none = windows();
    const Assessment n = assess(none);
    check(n.verdict == Verdict::NoAllowRule && n.fixable, "no rules yet: fixable pre-approval");

    Status tcpOnly = windows();
    tcpOnly.rules << rule(true, 6, 0x7fffffff);
    check(assess(tcpOnly).verdict == Verdict::NoAllowRule, "TCP alone leaves UDP to the prompt");

    Status any = windows();
    any.rules << rule(true, 256, 0x7fffffff);
    check(assess(any).verdict == Verdict::Allowed, "an any-protocol allow covers TCP and UDP");
}

void testFixCommand()
{
    const QString cmd = fixCommandLine(QStringLiteral("C:/Users/Op/AetherSDR/AetherSDR.exe"));
    const QString expected = QStringLiteral(
        "/D /C \"netsh advfirewall firewall delete rule name=all "
        "program=\"C:\\Users\\Op\\AetherSDR\\AetherSDR.exe\" >NUL 2>&1 & "
        "netsh advfirewall firewall add rule name=\"AetherSDR (TCP-In)\" dir=in action=allow "
        "program=\"C:\\Users\\Op\\AetherSDR\\AetherSDR.exe\" enable=yes profile=any protocol=TCP & "
        "netsh advfirewall firewall add rule name=\"AetherSDR (UDP-In)\" dir=in action=allow "
        "program=\"C:\\Users\\Op\\AetherSDR\\AetherSDR.exe\" enable=yes profile=any protocol=UDP\"");
#ifdef Q_OS_WIN
    check(cmd == expected, "Fix deletes every program rule, then adds TCP and UDP allows");
#else
    // toNativeSeparators keeps '/' off Windows; the command shape is the same.
    QString posix = expected;
    posix.replace(QStringLiteral("C:\\Users\\Op\\AetherSDR\\AetherSDR.exe"),
                 QStringLiteral("C:/Users/Op/AetherSDR/AetherSDR.exe"));
    check(cmd == posix, "Fix deletes every program rule, then adds TCP and UDP allows");
#endif
}

void testFailureLogging()
{
    g_previous = qInstallMessageHandler(capture);

    auto* a = new FakeReply(QUrl(QStringLiteral("https://xmldata.qrz.com/xml/current/?username=K1ABC;password=hunter2")),
                            QNetworkReply::SslHandshakeFailedError,
                            QStringLiteral("TLS handshake failed"));
    NetworkDiagnostics::watch(a, "QRZ");
    a->raiseSsl({QSslError(QSslError::UnableToGetLocalIssuerCertificate)});
    a->finish();
    check(g_lines.size() == 2 && g_lines.at(0).startsWith(QStringLiteral("TLS backend")),
          "the first failure logs the TLS backend once, then the failure");
    const QString line = g_lines.value(1);
    check(line.contains(QStringLiteral("QRZ: https://xmldata.qrz.com failed"))
              && line.contains(QStringLiteral("TLS handshake failed"))
              && line.contains(QStringLiteral("TLS errors:")),
          "the failure names the service, host, error text and TLS errors");
    check(!line.contains(QStringLiteral("hunter2")) && !line.contains(QStringLiteral("/xml/")),
          "the path and query (QRZ credentials) are never logged");

    auto* again = new FakeReply(QUrl(QStringLiteral("https://xmldata.qrz.com/other")),
                                QNetworkReply::SslHandshakeFailedError, QStringLiteral("x"));
    NetworkDiagnostics::watch(again, "QRZ");
    again->finish();
    check(g_lines.size() == 2, "the same host and error is logged once per session");

    auto* tile = new FakeReply(QUrl(QStringLiteral("https://tile.openstreetmap.org/1/0/0.png")),
                               QNetworkReply::ConnectionRefusedError,
                               QStringLiteral("Connection refused"), 0);
    NetworkDiagnostics::watch(tile, "map tiles");
    tile->finish();
    check(g_lines.size() == 3 && !g_lines.last().startsWith(QStringLiteral("TLS backend")),
          "a new host logs its failure without repeating the backend line");

    auto* http = new FakeReply(QUrl(QStringLiteral("https://api.github.com/x")),
                               QNetworkReply::ContentNotFoundError, QStringLiteral("Not Found"), 404);
    NetworkDiagnostics::watch(http, "update check");
    http->finish();
    check(g_lines.size() == 4 && g_lines.last().contains(QStringLiteral("HTTP 404")),
          "the HTTP status rides along when there is one");

    auto* cancelled = new FakeReply(QUrl(QStringLiteral("https://cancel.example")),
                                    QNetworkReply::OperationCanceledError, QStringLiteral("cancel"));
    NetworkDiagnostics::watch(cancelled, "map tiles");
    cancelled->finish();
    auto* ok = new FakeReply(QUrl(QStringLiteral("https://ok.example")),
                             QNetworkReply::NoError, QString());
    NetworkDiagnostics::watch(ok, "map tiles");
    ok->finish();
    check(g_lines.size() == 4, "cancelled and successful replies log nothing");

    qInstallMessageHandler(g_previous);
    for (auto* r : {a, again, tile, http, cancelled, ok}) delete r;
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    testVerdicts();
    testFixCommand();
    testFailureLogging();
    std::printf("%s\n", g_failed ? "FAILED" : "all checks passed");
    return g_failed ? 1 : 0;
}
