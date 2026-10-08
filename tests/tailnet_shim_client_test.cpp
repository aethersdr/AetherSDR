// Socket-free tests for the in-radio tailnet shim client's wire handling:
// status and provisioning replies parse, malformed replies are rejected,
// request bodies carry exactly what the shim's API accepts, and the helpers
// that turn operator text into a hostname and an allowlist stay in the
// shim's accepted alphabet (tools/flex-tailnet-shim/api.go).

#include "core/TailnetShimClient.h"

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
        "hostname":"kk7gwy-flex-8600","tailnet_ip":"100.78.16.123",
        "dns_name":"kk7gwy-flex-8600.tail1234.ts.net","allow":["tag:ops","me@example.com"],
        "sessions":1,"last_error":""})";
    const auto st = tailnetshim::parseStatus(running);
    ok &= expect(st.has_value(), "status parses");
    if (st) {
        ok &= expect(st->provisioned && st->state == QStringLiteral("running"), "state fields");
        ok &= expect(st->tailnetIp == QStringLiteral("100.78.16.123"), "tailnet ip");
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

    ok &= expect(tailnetshim::parseError(R"({"error":"admin token required"})")
                 == QStringLiteral("admin token required"), "error message parses");
    ok &= expect(tailnetshim::parseError("<html>").isEmpty(), "non-JSON error is empty");

    const QJsonObject body = QJsonDocument::fromJson(tailnetshim::provisionBody(
        QStringLiteral("  tskey-auth-k123  "), QStringLiteral("flex-8600"),
        {QStringLiteral("tag:ops")})).object();
    ok &= expect(body.keys() == QStringList({QStringLiteral("allow"), QStringLiteral("auth_key"),
                                             QStringLiteral("hostname")}),
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

    return ok ? 0 : 1;
}
