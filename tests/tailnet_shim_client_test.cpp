// Socket-free tests for the in-radio tailnet shim client's wire handling:
// status and provisioning replies parse, malformed replies are rejected,
// request bodies carry exactly what the shim's API accepts, and the helpers
// that turn operator text into a hostname and an allowlist stay in the
// shim's accepted alphabet (tools/flex-tailnet-shim/api.go).

#include "core/TailnetAddress.h"
#include "core/TailnetShimClient.h"
#include "core/TailnetShimDownloader.h"

#include <QCryptographicHash>
#include <QTemporaryDir>

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

int main()
{
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

    return ok ? 0 : 1;
}
