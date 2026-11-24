// aetherd's LAN advertisement (docs/aetherd-remote-backend.md §1). Socket-free:
// the exact probe, the source-address and WireGuard-interface filters, the
// reply's contents and 1 KiB bound, and the 10-a-second rate bound. Then one
// responder on 127.0.0.1 and an ephemeral port: a probe is answered by unicast,
// anything else and an over-rate burst are not, and loopback stays ignored
// unless the test opts in.
#include "core/control/DiscoveryResponder.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkDatagram>
#include <QUdpSocket>
#include <cstdio>

using namespace AetherSDR::control;

namespace {
int failures = 0;
void check(bool condition, const char* message)
{
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
    failures += !condition;
}

const QString kFingerprint = QStringLiteral("c8").repeated(32);
const QString kUrl = QStringLiteral("wss://192.168.50.216:8443");

bool accepts(const char* address, const char* arrival = "eth0")
{
    return DiscoveryResponder::acceptsSource(QHostAddress(QString::fromLatin1(address)),
                                             QString::fromLatin1(arrival));
}

void probeFormat()
{
    check(DiscoveryResponder::isProbe("AETHERD-DISCOVER 1"), "the exact probe is a probe");
    check(!DiscoveryResponder::isProbe("AETHERD-DISCOVER 1\n"), "a trailing newline is not");
    check(!DiscoveryResponder::isProbe("AETHERD-DISCOVER 2"), "another version is not");
    check(!DiscoveryResponder::isProbe("aetherd-discover 1"), "case matters");
    check(!DiscoveryResponder::isProbe(""), "an empty datagram is not");
    check(!DiscoveryResponder::isProbe(QByteArray("AETHERD-DISCOVER 1").append('\0')),
          "a trailing NUL is not");
}

void sourceFilter()
{
    check(accepts("192.168.50.10") && accepts("10.1.2.3") && accepts("172.16.0.9"),
          "RFC 1918 sources are answered");
    check(accepts("169.254.7.7") && accepts("fe80::1"), "link-local sources are answered");
    check(accepts("fd00::5"), "an IPv6 ULA source is answered");
    check(accepts("::ffff:192.168.50.10"), "an IPv4-mapped private source is judged as IPv4");
    check(!accepts("8.8.8.8") && !accepts("2001:db8::1") && !accepts("172.32.0.1"),
          "public sources are ignored");
    check(!accepts("100.119.109.89") && !accepts("100.64.0.1") && !accepts("::ffff:100.100.1.1"),
          "100.64/10 (the tailnet's IPv4) is ignored");
    check(accepts("100.128.0.1") == false, "just past 100.64/10 is public, still ignored");
    check(!accepts("fd7a:115c:a1e0::1", "tailscale0"), "Tailscale's ULA on tailscale0 is ignored");
    check(!accepts("fd7a:115c:a1e0:ab12::5"), "Tailscale's IPv6 prefix is ignored on any interface");
    check(!accepts("192.168.50.10", "wg0"), "any source arriving on a wg* interface is ignored");
    check(!accepts("127.0.0.1") && !accepts("::1"), "loopback is ignored by default");
    check(DiscoveryResponder::acceptsSource(QHostAddress::LocalHost, {}, true),
          "loopback is answered only when a test opts in");
    check(!DiscoveryResponder::acceptsSource(QHostAddress(), {}, true), "a null source is ignored");
    check(DiscoveryResponder::isWireGuardInterfaceName("tailscale0")
              && DiscoveryResponder::isWireGuardInterfaceName("wg-home")
              && !DiscoveryResponder::isWireGuardInterfaceName("eth0")
              && !DiscoveryResponder::isWireGuardInterfaceName("end0"),
          "WireGuard names match the server's tailnet check");
}

void replyContents()
{
    const QByteArray bytes = DiscoveryResponder::buildReply(
        QStringLiteral("g2-alarm"), {QStringLiteral("ANAN-G2"), QStringLiteral("anan"), true},
        kUrl, kFingerprint);
    const QJsonObject reply = QJsonDocument::fromJson(bytes).object();
    check(reply.value("aetherd").toInt() == 1, "the reply is version 1");
    check(reply.value("name").toString() == "g2-alarm", "the reply names the host");
    const QJsonObject radio = reply.value("radio").toObject();
    check(radio.value("model").toString() == "ANAN-G2" && radio.value("family").toString() == "anan"
              && radio.value("connected").toBool(),
          "the reply names the held radio");
    check(reply.value("wss").toString() == kUrl, "the reply names the wss:// listener");
    check(reply.value("certificateSha256").toString() == kFingerprint, "the reply carries the fingerprint");
    check(reply.keys().size() == 5 && radio.keys().size() == 3,
          "the reply carries nothing beyond its documented fields");

    const QString huge = QStringLiteral("x").repeated(5000);
    const QByteArray bounded = DiscoveryResponder::buildReply(huge, {huge, huge, false}, kUrl, kFingerprint);
    const QJsonObject boundedReply = QJsonDocument::fromJson(bounded).object();
    check(!bounded.isEmpty() && bounded.size() <= DiscoveryResponder::kMaxReplyBytes,
          "oversized names are cut so the reply fits 1 KiB");
    check(boundedReply.value("name").toString().size() == DiscoveryResponder::kMaxFieldChars,
          "a name is cut at 64 characters");
    check(DiscoveryResponder::buildReply({}, {}, QStringLiteral("w").repeated(2000), kFingerprint).isEmpty(),
          "a reply that still cannot fit is not sent");
}

void rateBound()
{
    DiscoveryResponder responder({});
    int taken = 0;
    for (int i = 0; i < 25; ++i) { taken += responder.takeReplySlot(1000 + i); }
    check(taken == DiscoveryResponder::kMaxRepliesPerSecond, "at most 10 replies in one second");
    check(!responder.takeReplySlot(1999), "still refused inside the same second");
    check(responder.takeReplySlot(2000), "a slot frees one second after the first reply");
    int later = 0;
    for (int i = 0; i < 25; ++i) { later += responder.takeReplySlot(5000); }
    check(later == DiscoveryResponder::kMaxRepliesPerSecond, "the bound is per trailing second");
}

QList<QByteArray> collect(QUdpSocket& client, int waitMs)
{
    QList<QByteArray> replies;
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < waitMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        while (client.hasPendingDatagrams()) { replies.append(client.receiveDatagram().data()); }
    }
    return replies;
}

int loopback()
{
    qint64 now = 0;
    DiscoveryResponder::Config config;
    config.name = QStringLiteral("loopback-engine");
    config.wssUrl = kUrl;
    config.certificateSha256 = kFingerprint;
    config.radio = [] { return DiscoveryResponder::Radio{QStringLiteral("HL2"), QStringLiteral("hl2"), false}; };
    config.clock = [&now] { return now; };
    config.bindAddress = QHostAddress::LocalHost;
    config.port = 0;
    config.acceptLoopback = true;
    DiscoveryResponder responder(config);
    QString error;
    if (!responder.start(&error)) {
        std::printf("[SKIP] cannot bind 127.0.0.1 UDP: %s\n", qPrintable(error));
        return 77;
    }
    check(responder.isListening() && responder.boundPort() != 0, "the responder binds an ephemeral port");
    const quint16 port = responder.boundPort();

    QUdpSocket client;
    if (!client.bind(QHostAddress::LocalHost, 0)) {
        std::printf("[SKIP] cannot bind a 127.0.0.1 client socket\n");
        return 77;
    }
    client.writeDatagram("AETHERD-DISCOVER 1", QHostAddress::LocalHost, port);
    QList<QByteArray> replies = collect(client, 500);
    check(replies.size() == 1, "one probe gets one reply");
    const QJsonObject reply = replies.isEmpty() ? QJsonObject{} : QJsonDocument::fromJson(replies.first()).object();
    check(reply.value("name").toString() == "loopback-engine"
              && reply.value("radio").toObject().value("family").toString() == "hl2"
              && !reply.value("radio").toObject().value("connected").toBool(true),
          "the reply reaches the prober's own port with the current radio");

    now += 2000;
    client.writeDatagram("AETHERD-DISCOVER 2", QHostAddress::LocalHost, port);
    client.writeDatagram("hello", QHostAddress::LocalHost, port);
    check(collect(client, 300).isEmpty(), "anything but the probe is ignored");

    now += 2000;
    for (int i = 0; i < 15; ++i) { client.writeDatagram("AETHERD-DISCOVER 1", QHostAddress::LocalHost, port); }
    check(collect(client, 500).size() == DiscoveryResponder::kMaxRepliesPerSecond,
          "a burst of 15 probes in one second gets 10 replies");

    responder.stop();
    check(!responder.isListening(), "stop releases the port");

    DiscoveryResponder::Config strict = config;
    strict.acceptLoopback = false;
    DiscoveryResponder production(strict);
    if (!production.start(&error)) {
        std::printf("[SKIP] cannot bind a second responder: %s\n", qPrintable(error));
        return 77;
    }
    client.writeDatagram("AETHERD-DISCOVER 1", QHostAddress::LocalHost, production.boundPort());
    check(collect(client, 300).isEmpty(), "with production filters, a loopback probe gets no reply");
    return 0;
}
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    probeFormat();
    sourceFilter();
    replyContents();
    rateBound();
    if (loopback() == 77) { return failures ? 1 : 77; }
    std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
