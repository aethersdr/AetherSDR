// Socket-free tests for the in-radio tailnet shim client's wire handling:
// status and provisioning replies parse, malformed replies are rejected,
// request bodies carry exactly what the shim's API accepts, and the helpers
// that turn operator text into a hostname and an allowlist stay in the
// shim's accepted alphabet (tools/flex-tailnet-shim/api.go).

#include "core/AudioCompressionPolicy.h"
#include "core/TailnetAddress.h"
#include "core/TailnetLinkTelemetry.h"
#include "core/TailnetShimClient.h"
#include "core/TailnetShimDownloader.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QTemporaryDir>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <iostream>

namespace {

bool expect(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
    }
    return condition;
}

}  // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    using namespace AetherSDR;
    bool ok = true;

    const QByteArray running = R"({"version":"0.2.0","provisioned":true,"state":"running",
        "hostname":"kk7gwy-flex-8600","tailnet_ip":"100.100.1.2",
        "dns_name":"kk7gwy-flex-8600.tail1234.ts.net","allow":["tag:ops","me@example.com"],
        "sessions":1,"last_error":""})";
    const auto st = tailnetshim::parseStatus(running);
    ok &= expect(st.has_value(), "status parses");
    if (st) {
        ok &= expect(st->provisioned && st->state == QStringLiteral("running"), "state fields");
        ok &= expect(st->tailnetIp == QStringLiteral("100.100.1.2"), "tailnet ip");
        ok &= expect(st->allow == QStringList({QStringLiteral("tag:ops"),
                                               QStringLiteral("me@example.com")}), "allowlist");
        ok &= expect(st->sessions == 1, "session count");
    }
    ok &= expect(!tailnetshim::parseStatus("not json").has_value(), "garbage rejected");
    ok &= expect(!tailnetshim::parseStatus(R"({"error":"x"})").has_value(),
                 "an error object is not a status");

    const QByteArray provisioned = R"({"admin_token":"abc123","status":)" + running + "}";
    const auto pr = tailnetshim::parseProvisionReply(provisioned);
    ok &= expect(pr.has_value() && pr->first == QStringLiteral("abc123")
                 && pr->second.state == QStringLiteral("running"), "provision reply parses");
    ok &= expect(!tailnetshim::parseProvisionReply(running).has_value(),
                 "provision reply without a token is rejected");

    const QByteArray withDevices = R"({"version":"0.3.0","provisioned":true,"state":"running",
        "routes":["192.168.50.120"],"share_discovered":false,
        "discovered_devices":[{"kind":"Antenna Genius","name":"Antenna_Genius","ip":"192.168.50.103","port":9007},
                              {"kind":"Tuner Genius XL","name":"KK7GWY_-_TGXL","ip":"192.168.50.101","port":9010}],
        "advertised_routes":["192.168.50.120/32"],"approved_routes":["192.168.50.120/32"]})";
    const auto sd = tailnetshim::parseStatus(withDevices);
    ok &= expect(sd && sd->discovered.size() == 2 && sd->discovered[1].kind == QStringLiteral("Tuner Genius XL")
                 && sd->discovered[0].ip == QStringLiteral("192.168.50.103"), "discovered devices parse");
    ok &= expect(sd && !sd->shareDiscovered && sd->routes == QStringList({QStringLiteral("192.168.50.120")})
                 && sd->advertisedRoutes.size() == 1
                 && sd->approvedRoutes == QStringList({QStringLiteral("192.168.50.120/32")}),
                 "sharing settings and route approval parse");
    ok &= expect(st && st->shareDiscovered, "share_discovered defaults to on when absent");
    const QJsonObject sharing = QJsonDocument::fromJson(tailnetshim::sharingBody(
        {QStringLiteral("192.168.50.120")}, false)).object();
    ok &= expect(sharing.keys() == QStringList({QStringLiteral("routes"), QStringLiteral("share_discovered")})
                 && !sharing.value(QStringLiteral("share_discovered")).toBool(true),
                 "sharing body has exactly the API's fields");

    ok &= expect(tailnetshim::parseError(R"({"error":"admin token required"})")
                 == QStringLiteral("admin token required"), "error message parses");

    ok &= expect(tailnetshim::parseError("<html>").isEmpty(), "non-JSON error is empty");

    const QJsonObject body = QJsonDocument::fromJson(tailnetshim::provisionBody(
        QStringLiteral("  tskey-auth-k123  "), QStringLiteral("flex-8600"),
        {QStringLiteral("tag:ops")})).object();
    ok &= expect(body.keys() == QStringList({QStringLiteral("allow"), QStringLiteral("auth_key"),
                                             QStringLiteral("hostname"), QStringLiteral("routes"),
                                             QStringLiteral("share_discovered")}),
                 "provision body has exactly the API's fields");
    ok &= expect(body.value(QStringLiteral("auth_key")).toString()
                 == QStringLiteral("tskey-auth-k123"), "auth key trimmed");

    ok &= expect(tailnetshim::suggestedHostname(QStringLiteral("KK7GWY FLEX-8600"))
                 == QStringLiteral("kk7gwy-flex-8600"), "hostname from nickname");
    ok &= expect(tailnetshim::suggestedHostname(QStringLiteral("  --  "))
                 == QStringLiteral("flex-radio"), "empty nickname falls back");
    ok &= expect(tailnetshim::suggestedHostname(QString(80, QLatin1Char('a'))).size() == 63,
                 "hostname capped at 63");

    ok &= expect(tailnetshim::splitAllowList(QStringLiteral(" a@b.com,tag:ops  tag:ops\nc@d.org,"))
                 == QStringList({QStringLiteral("a@b.com"), QStringLiteral("tag:ops"),
                                 QStringLiteral("c@d.org")}),
                 "allowlist split, trimmed and de-duplicated");

    // Tailnet address classification (RFC #6271 D4: Opus by default there).
    ok &= expect(isTailnetAddress(QHostAddress(QStringLiteral("100.64.0.10"))), "100.64.x is tailnet");
    ok &= expect(isTailnetAddress(QHostAddress(QStringLiteral("100.127.255.254"))), "top of 100.64/10");
    ok &= expect(!isTailnetAddress(QHostAddress(QStringLiteral("100.128.0.1"))), "100.128 is not");
    ok &= expect(!isTailnetAddress(QHostAddress(QStringLiteral("192.168.50.100"))), "LAN is not");
    ok &= expect(isTailnetAddress(QHostAddress(QStringLiteral("fd7a:115c:a1e0::1234"))), "tailnet IPv6");
    ok &= expect(!isTailnetAddress(QHostAddress(QStringLiteral("fd00::1"))), "other ULA is not");
    ok &= expect(isTailnetAddress(QHostAddress(QStringLiteral("::ffff:100.64.1.2"))), "v4-mapped tailnet");
    ok &= expect(!isTailnetAddress(QHostAddress()), "null address is not");

    // Pinned-image verification (RFC #6271 D2): exact bytes only.
    {
        QTemporaryDir dir;
        const QByteArray image("an image payload");
        const QByteArray sha = QCryptographicHash::hash(image, QCryptographicHash::Sha256).toHex();
        const QString path = dir.filePath(QStringLiteral("image.tar.gz"));
        QFile f(path);
        ok &= expect(f.open(QIODevice::WriteOnly) && f.write(image) == image.size(), "fixture written");
        f.close();
        ok &= expect(TailnetShimDownloader::verifyFile(path, sha, image.size()).isEmpty(),
                     "exact bytes verify");
        ok &= expect(TailnetShimDownloader::verifyFile(path, sha.toUpper(), image.size()).isEmpty(),
                     "hex case doesn't matter");
        ok &= expect(!TailnetShimDownloader::verifyFile(path, sha, image.size() + 1).isEmpty(),
                     "wrong size refused");
        QByteArray other = image;
        other[0] = 'A';
        const QByteArray otherSha = QCryptographicHash::hash(other, QCryptographicHash::Sha256).toHex();
        ok &= expect(!TailnetShimDownloader::verifyFile(path, otherSha, image.size()).isEmpty(),
                     "same size, different bytes refused");
        ok &= expect(!TailnetShimDownloader::verifyFile(dir.filePath(QStringLiteral("missing")), sha,
                                                        image.size()).isEmpty(),
                     "missing file refused");
    }

    // Link telemetry (/v1/session): the shape tools/flex-tailnet-shim
    // telemetry.go encodes, summed over this client's sessions and streams.
    {
        const QByteArray report = R"json({"version":"0.4.0","path":"relay","endpoint":"",
            "relay":"sea","rtt_ms":41.5,"rtt_via":"DERP(sea)","rtt_age_s":1.2,
            "path_changes":2,"path_since_s":95,"to_client_kbps":812.4,
            "from_client_kbps":31.0,"last_handshake_s":40,"shim_cpu_pct":3.5,
            "shim_rss_kb":24576,"mtu_clamp":1200,"sessions":[
              {"id":1,"peer":"laptop","to_client_failures":2,"streams":[
                {"stream_id":"0x40000000","packets":1000,"gaps":3,"breaks":1},
                {"stream_id":"0x42000000","packets":500,"gaps":0,"breaks":0}]},
              {"id":2,"peer":"laptop","to_client_failures":1,"streams":[
                {"stream_id":"0x04000008","packets":200,"gaps":2,"breaks":2}]}]})json";
        const auto r = tailnetshim::parseSessionReport(report);
        ok &= expect(r.has_value(), "session report parses");
        if (r) {
            ok &= expect(r->version == QLatin1String("0.4.0") && r->path == QLatin1String("relay")
                             && r->relay == QLatin1String("sea"), "path fields");
            ok &= expect(r->rttMs == 41.5 && r->rttVia == QLatin1String("DERP(sea)"), "rtt");
            ok &= expect(r->pathChanges == 2 && r->mtuClamp == 1200 && r->shimRssKb == 24576,
                         "scalars");
            ok &= expect(r->sessions == 2 && r->sendFailures == 3, "sessions summed");
            ok &= expect(r->radioPackets == 1700 && r->radioBreaks == 3 && r->radioGaps == 5,
                         "streams summed across sessions");
        }
        const auto none = tailnetshim::parseSessionReport(
            R"({"version":"0.4.0","rtt_ms":-1,"sessions":[]})");
        ok &= expect(none && none->rttMs < 0 && none->sessions == 0,
                     "no RTT yet stays -1, no sessions is valid");
        ok &= expect(!tailnetshim::parseSessionReport("not json"), "garbage refused");
        ok &= expect(!tailnetshim::parseSessionReport(R"({"sessions":[]})"), "no version refused");
        ok &= expect(!tailnetshim::parseSessionReport(R"({"version":"0.4.0"})"),
                     "no sessions array refused");

        // Breaks the tunnel added: the client's rate less the radio's.
        ok &= expect(qFuzzyCompare(tailnetshim::tunnelBreakPercent(1000, 5, 1000, 1), 0.4),
                     "0.5% seen, 0.1% before the tunnel: 0.4% added");
        ok &= expect(tailnetshim::tunnelBreakPercent(1000, 1, 1000, 3) == 0.0,
                     "radio worse than client (window skew) reads 0, not negative");
        ok &= expect(tailnetshim::tunnelBreakPercent(0, 0, 1000, 0) == 0.0,
                     "no client packets in the window: 0");
        ok &= expect(qFuzzyCompare(tailnetshim::tunnelBreakPercent(200, 2, 0, 0), 1.0),
                     "no radio count: everything the client saw");

        // Polling follows the radio's address: tailnet only. Starting the
        // poller issues one request, but nothing here runs an event loop, so
        // it never leaves the process and is aborted when the address
        // clears. Keep it that way: a processEvents() above this would make
        // this a network test.
        TailnetLinkTelemetry poller;
        poller.setRadioAddress(QHostAddress(QStringLiteral("192.168.1.20")));
        ok &= expect(!poller.isPolling() && !poller.current(), "LAN radio: idle");
        poller.setRadioAddress(QHostAddress(QStringLiteral("100.101.102.103")));
        ok &= expect(poller.isPolling(), "tailnet radio: polling");
        poller.setRadioAddress(QHostAddress());
        ok &= expect(!poller.isPolling() && !poller.current(), "disconnect: idle and cleared");
    }

    // A finished download reaches the cache only if the bytes on disk verify
    // (a full disk can leave fewer than the network delivered).
    {
        QTemporaryDir dir;
        const QByteArray image("the pinned image bytes");
        const QByteArray sha = QCryptographicHash::hash(image, QCryptographicHash::Sha256).toHex();
        const QString part = dir.filePath(QStringLiteral("image.part"));
        const QString target = dir.filePath(QStringLiteral("image.tar.gz"));
        auto writeFile = [](const QString& path, const QByteArray& bytes) {
            QFile f(path);
            return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size();
        };
        ok &= expect(writeFile(part, image.left(5)), "truncated part written");
        ok &= expect(!TailnetShimDownloader::promoteVerified(part, target, sha, image.size()).isEmpty(),
                     "a short write on disk is refused");
        ok &= expect(!QFile::exists(part) && !QFile::exists(target),
                     "the refused part is deleted and nothing is cached");
        ok &= expect(writeFile(part, image), "complete part written");
        ok &= expect(TailnetShimDownloader::promoteVerified(part, target, sha, image.size()).isEmpty(),
                     "verified bytes are promoted");
        ok &= expect(QFile::exists(target) && !QFile::exists(part), "promoted to the cache path");
    }

    // RFC #6271 D4: which compression a session asks for.
    {
        const QString none = QStringLiteral("none");
        const QString opus = QStringLiteral("opus");
        ok &= expect(audioCompressionFor({}, false, false) == none, "never chosen, LAN: uncompressed");
        ok &= expect(audioCompressionFor({}, true, false) == none, "never chosen, SmartLink: uncompressed");
        ok &= expect(audioCompressionFor({}, false, true) == opus, "never chosen, tailnet: Opus");
        ok &= expect(audioCompressionFor(QStringLiteral("None"), false, true) == none,
                     "an explicit Uncompressed wins over a tailnet");
        ok &= expect(audioCompressionFor(QStringLiteral("Opus"), false, false) == opus,
                     "an explicit Opus wins on the LAN");
        ok &= expect(audioCompressionFor(QStringLiteral("Auto"), false, false) == none, "Auto, LAN");
        ok &= expect(audioCompressionFor(QStringLiteral("Auto"), true, false) == opus, "Auto, SmartLink");
        ok &= expect(audioCompressionFor(QStringLiteral("Auto"), false, true) == opus, "Auto, tailnet");
    }

    // Strings from the container are capped before they reach the window.
    {
        const QString huge(10000, QLatin1Char('x'));
        QJsonObject o{{QStringLiteral("version"), QStringLiteral("0.4.0")},
                      {QStringLiteral("state"), QStringLiteral("running")},
                      {QStringLiteral("hostname"), huge},
                      {QStringLiteral("last_error"), huge}};
        QJsonArray allow;
        for (int i = 0; i < 500; ++i) {
            allow.append(QStringLiteral("tag:t%1").arg(i));
        }
        o.insert(QStringLiteral("allow"), allow);
        const auto capped = tailnetshim::parseStatus(QJsonDocument(o).toJson());
        ok &= expect(capped && capped->hostname.size() <= 256 && capped->lastError.size() <= 512
                         && capped->allow.size() <= 64,
                     "oversized status strings and lists are capped");
    }

    return ok ? 0 : 1;
}
