// WindowsFirewall's verdicts and Fix, and NetworkDiagnostics' failure logging.
// Both are pure enough to pin on any platform: assess() and afterFix() read a
// Status a test can build, and watch() reads a QNetworkReply a test can fake.

#include "core/NetworkDiagnostics.h"
#include "core/WindowsFirewall.h"

#include <QCoreApplication>
#include <QDir>
#include <QNetworkReply>
#include <QSslError>
#include <QStringList>
#include <QUrlQuery>

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

constexpr int kAll = 0x7fffffff;

Status windows(QList<int> profiles = {kProfilePrivate}, bool outboundBlocked = false)
{
    Status s;
    s.inspected = true;
    s.programPath = QStringLiteral("C:\\Program Files\\AetherSDR\\AetherSDR.exe");
    for (int bit : profiles) {
        Profile p;
        p.bit = bit;
        p.outboundBlockedByDefault = outboundBlocked;
        s.activeProfiles << p;
    }
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
    off.activeProfiles[0].enabled = false;
    check(assess(off).verdict == Verdict::Disabled, "firewall off for every active network");

    Status blocked = windows({kProfilePublic});
    blocked.rules << rule(false, kProtocolUdp, kProfilePublic) << rule(true, kProtocolAny, kAll);
    const Assessment b = assess(blocked);
    check(b.verdict == Verdict::Blocked && b.fixable && b.details.size() == 1,
          "an inbound block on an active profile beats an allow and is fixable");

    Status outBlock = windows();
    outBlock.rules << rule(false, kProtocolTcp, kAll, false) << rule(true, kProtocolAny, kAll);
    const Assessment ob = assess(outBlock);
    check(ob.verdict == Verdict::Blocked && !ob.fixable,
          "an outbound block rule is reported but not offered Fix (Fix only repairs inbound)");

    Status otherProfile = windows({kProfilePrivate});
    otherProfile.rules << rule(false, kProtocolUdp, kProfilePublic)
                       << rule(true, kProtocolTcp, kProfilePrivate)
                       << rule(true, kProtocolUdp, kProfilePrivate);
    check(assess(otherProfile).verdict == Verdict::Allowed,
          "a block rule on an inactive profile does not block");

    Status offProfileBlock = windows({kProfilePrivate, kProfilePublic});
    offProfileBlock.activeProfiles[1].enabled = false;
    offProfileBlock.rules << rule(false, kProtocolAny, kProfilePublic)
                          << rule(true, kProtocolAny, kProfilePrivate);
    check(assess(offProfileBlock).verdict == Verdict::Allowed,
          "a block rule on an active profile whose firewall is off does not block");

    Status disabledBlock = windows();
    disabledBlock.rules << rule(false, kProtocolAny, kAll, true, false);
    check(assess(disabledBlock).verdict == Verdict::NoAllowRule,
          "a disabled block rule does not block");

    // Review of #6289: two active profiles are judged one by one.
    Status twoProfiles = windows({kProfilePrivate, kProfilePublic});
    twoProfiles.rules << rule(true, kProtocolTcp, kProfilePrivate)
                      << rule(true, kProtocolUdp, kProfilePrivate);
    const Assessment tp = assess(twoProfiles);
    check(tp.verdict == Verdict::NoAllowRule && tp.fixable
              && tp.details.join(QLatin1Char(' ')).contains(QStringLiteral("public")),
          "Private-only allows leave an active Public network uncovered");

    // Review of #6289: an outbound allow must carry TCP and UDP, not any protocol number.
    Status icmpOnly = windows({kProfilePrivate}, true);
    icmpOnly.rules << rule(true, kProtocolAny, kAll) << rule(true, 1, kAll, false);
    check(assess(icmpOnly).verdict == Verdict::OutboundBlocked,
          "an ICMP-only outbound allow does not satisfy an outbound-blocked default");
    Status tcpOut = windows({kProfilePrivate}, true);
    tcpOut.rules << rule(true, kProtocolAny, kAll) << rule(true, kProtocolTcp, kAll, false);
    check(assess(tcpOut).verdict == Verdict::OutboundBlocked,
          "TCP-only outbound leaves the radio's UDP blocked");
    tcpOut.rules << rule(true, kProtocolUdp, kAll, false);
    check(assess(tcpOut).verdict == Verdict::Allowed,
          "outbound TCP and UDP allows satisfy an outbound-blocked default");

    Status portLimited = windows();
    Rule limited = rule(true, kProtocolAny, kAll);
    limited.restricted = true;
    portLimited.rules << limited;
    check(assess(portLimited).verdict == Verdict::NoAllowRule,
          "a port- or address-limited allow is not counted as coverage");
    Status limitedBlock = windows();
    Rule limitedB = rule(false, kProtocolUdp, kAll);
    limitedB.restricted = true;
    limitedBlock.rules << limitedB << rule(true, kProtocolAny, kAll);
    check(assess(limitedBlock).verdict == Verdict::Blocked,
          "a port-limited block still counts as blocking");

    const Assessment n = assess(windows());
    check(n.verdict == Verdict::NoAllowRule && n.fixable, "no rules yet: fixable pre-approval");

    Status tcpOnly = windows();
    tcpOnly.rules << rule(true, kProtocolTcp, kAll);
    check(assess(tcpOnly).verdict == Verdict::NoAllowRule, "TCP alone leaves UDP to the prompt");

    Status any = windows();
    any.rules << rule(true, kProtocolAny, kAll);
    check(assess(any).verdict == Verdict::Allowed, "an any-protocol allow covers TCP and UDP");

    // Review 2 of #6289: TCP is pre-allowed on domain and private networks
    // only; on a Public network only UDP is needed.
    Status publicUdp = windows({kProfilePublic});
    publicUdp.rules << rule(true, kProtocolUdp, kAll);
    check(assess(publicUdp).verdict == Verdict::Allowed,
          "on a Public network a UDP allow is enough; TCP stays with the prompt");
    Status privateUdp = windows({kProfilePrivate});
    privateUdp.rules << rule(true, kProtocolUdp, kAll);
    check(assess(privateUdp).verdict == Verdict::NoAllowRule,
          "on a Private network TCP is still needed");

    check(assess(windows({})).verdict == Verdict::NoNetwork,
          "no active network reads as no network, not as the firewall being off");

    Status suiteAndBlock = windows();
    suiteAndBlock.thirdPartyFirewalls << QStringLiteral("Bitdefender Firewall");
    suiteAndBlock.rules << rule(false, kProtocolAny, kAll);
    const Assessment sb = assess(suiteAndBlock);
    check(sb.verdict == Verdict::Blocked && sb.fixable
              && sb.details.join(QLatin1Char(' ')).contains(QStringLiteral("Bitdefender")),
          "a readable Defender block rule is reported and fixable even with a suite registered");
    Status suiteOnly = windows();
    suiteOnly.thirdPartyFirewalls << QStringLiteral("Bitdefender Firewall");
    check(assess(suiteOnly).verdict == Verdict::ThirdParty,
          "with nothing wrong in Defender's rules, the suite is what to look at");
    Status suiteDefenderOff = windows();
    suiteDefenderOff.activeProfiles[0].enabled = false;
    suiteDefenderOff.thirdPartyFirewalls << QStringLiteral("Bitdefender Firewall");
    check(assess(suiteDefenderOff).verdict == Verdict::ThirdParty,
          "a suite that turned Defender off is named rather than reported as no firewall");

    Status store = windows();
    store.packaged = true;
    store.rules << rule(false, kProtocolAny, kAll);
    const Assessment st = assess(store);
    check(st.verdict == Verdict::Blocked && !st.fixable,
          "a Store install is not offered Fix, which would pin rules to this version's path");
    Status storeNoRule = windows();
    storeNoRule.packaged = true;
    const Assessment sn = assess(storeNoRule);
    check(!sn.fixable && sn.summary.contains(QStringLiteral("Store install")),
          "a Store install's package-declared rules are not reported as missing and fixable");
}

void testFixKeepsOutbound()
{
    // Review of #6289: an outbound-blocking PC whose administrator allowed
    // AetherSDR out, plus a dismissed prompt's inbound block.
    Status before = windows({kProfilePrivate}, true);
    before.rules << rule(true, kProtocolTcp, kAll, false) << rule(true, kProtocolUdp, kAll, false)
                 << rule(false, kProtocolAny, kAll);
    const Assessment pre = assess(before);
    check(pre.verdict == Verdict::Blocked && pre.fixable, "before Fix: inbound block, fixable");

    const Status after = afterFix(before);
    int outbound = 0;
    for (const Rule& r : after.rules) {
        outbound += r.inbound ? 0 : 1;
    }
    check(outbound == 2, "Fix keeps the administrator's outbound allows");
    check(assess(after).verdict == Verdict::Allowed,
          "after Fix the outbound-blocking PC is allowed in and still allowed out");

    Status publicAfter = afterFix(windows({kProfilePublic}));
    check(assess(publicAfter).verdict == Verdict::Allowed,
          "after Fix a Public network is covered by the UDP allow alone");

    const QString program = QStringLiteral("C:/Users/Op Name/A&B (x86)/AetherSDR.exe");
    const QString cmd = fixCommandLine(program);
    check(cmd.contains(QStringLiteral("delete rule name=all dir=in program=")),
          "Fix deletes inbound rules only");
    check(!cmd.contains(QStringLiteral("dir=out")) && cmd.count(QStringLiteral("delete rule")) == 1,
          "Fix never deletes outbound rules");
    check(cmd.contains(QStringLiteral(
              "name=\"AetherSDR (UDP-In)\" dir=in action=allow program=\"%1\" enable=yes "
              "profile=any protocol=UDP").arg(QDir::toNativeSeparators(program)))
              && cmd.contains(QStringLiteral(
              "name=\"AetherSDR (TCP-In)\" dir=in action=allow program=\"%1\" enable=yes "
              "profile=domain,private protocol=TCP").arg(QDir::toNativeSeparators(program))),
          "Fix adds UDP on all networks and TCP on domain and private only, quoting the path");
    check(cmd.count(QStringLiteral(" && ")) == 1 && cmd.count(QStringLiteral(">NUL 2>&1 & ")) == 1,
          "a failed add fails the command; only the no-match delete is allowed to fail");

    const QString qualified = fixCommandLine(program, QStringLiteral("C:\\Windows\\System32"));
    check(qualified.count(QStringLiteral("\"C:\\Windows\\System32\\netsh.exe\" advfirewall")) == 3
              && !qualified.contains(QStringLiteral(" netsh advfirewall"))
              && !qualified.startsWith(QStringLiteral("/D /C \"netsh")),
          "with the system directory, every netsh is called by its full path");

    QList<Rule> planned = fixRules();
    check(planned.size() == 2 && planned[0].protocol == kProtocolUdp
              && (planned[0].profiles & 0x7) == 0x7 && planned[1].protocol == kProtocolTcp
              && planned[1].profiles == (kProfileDomain | kProfilePrivate),
          "the planned rules are UDP on every profile and TCP on domain and private");
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
    check(g_lines.size() == 2, "the same service, host and error is logged once per session");

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

    // Review of #6289: Qt's HTTP error text quotes the full request URL, and
    // QRZ's query carries the credentials, delimiters and spaces included.
    QUrl qrz(QStringLiteral("https://xmldata.qrz.com/xml/current/"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("username"), QStringLiteral("K1ABC"));
    query.addQueryItem(QStringLiteral("password"), QStringLiteral("alpha;SECRET TAIL&x=y"));
    qrz.setQuery(query);
    for (const QString& spelling : {qrz.toString(), qrz.toString(QUrl::FullyEncoded)}) {
        const QString qtText = QStringLiteral("Error transferring %1 - server replied: Forbidden")
                                   .arg(spelling);
        auto* forbidden = new FakeReply(qrz, QNetworkReply::ContentAccessDenied, qtText, 403);
        NetworkDiagnostics::watch(forbidden, spelling == qrz.toString() ? "QRZ login" : "QRZ lookup");
        forbidden->finish();
        const QString leak = g_lines.last();
        check(leak.contains(QStringLiteral(
                  "Error transferring https://xmldata.qrz.com - server replied: Forbidden"))
                  && leak.contains(QStringLiteral("HTTP 403")),
              "Qt's quoted request URL is cut to scheme://host");
        check(!leak.contains(QStringLiteral("K1ABC")) && !leak.contains(QStringLiteral("alpha"))
                  && !leak.contains(QStringLiteral("SECRET")) && !leak.contains(QStringLiteral("TAIL"))
                  && !leak.contains(QStringLiteral("/xml/")),
              "no credential, query or path survives in the HTTP error line");
        if (spelling == qrz.toString()) {
            const QString shown = NetworkDiagnostics::safeErrorString(forbidden);
            check(shown == QStringLiteral(
                      "Error transferring https://xmldata.qrz.com - server replied: Forbidden"),
                  "safeErrorString, which QRZ now logs and shows, carries no credential");
        }
        delete forbidden;
    }

    const QString foreign = QStringLiteral(
        "Error transferring https://user:hunter2@mirror.example/dl?token=abc123 - redirected");
    auto* other = new FakeReply(QUrl(QStringLiteral("https://cdn.example/start")),
                                QNetworkReply::ProtocolFailure, foreign);
    NetworkDiagnostics::watch(other, "DeepFist model download");
    other->finish();
    const QString otherLine = g_lines.last();
    check(otherLine.contains(QStringLiteral("https://mirror.example - redirected"))
              && !otherLine.contains(QStringLiteral("hunter2"))
              && !otherLine.contains(QStringLiteral("abc123")),
          "any other URL in the error text loses its userinfo, path and query");

    qInstallMessageHandler(g_previous);
    for (auto* r : {a, again, tile, http, cancelled, ok, other}) delete r;
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    testVerdicts();
    testFixKeepsOutbound();
    testFailureLogging();
    std::printf("%s\n", g_failed ? "FAILED" : "all checks passed");
    return g_failed ? 1 : 0;
}
