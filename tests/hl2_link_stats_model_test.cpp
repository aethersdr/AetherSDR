// The MODEL half of the HL2 link-stats seam — RadioModel driven by a real
// Hl2Backend against a fake HL2 on localhost.
//
// hl2_link_stats_test covers the backend side: that a transport snapshot is
// published on a cadence and that its numbers are real. Everything the OPERATOR
// actually sees is downstream of that, in RadioModel, and that half is where the
// interesting mistakes live — the seam can be perfect while every readout still
// lies. This pins the consumer contract:
//
//   • the readouts leave their structural zero (the reported symptom: a radio
//     streaming happily while the pane showed 0 packets, 0 bytes, "Off");
//   • a figure this transport cannot produce reads as ABSENT, and keeps reading
//     absent after the session ends — "no round trip to time" is a fact about
//     the wire, not about whether the wire is currently up;
//   • starting a scoring session forgets the PREVIOUS session's ping RTT, which
//     is the field the two reset paths drifted on;
//   • the heartbeat beats, and the status field is announced as Off on the
//     disconnect edge rather than freezing on the last quality the link had.

#include "models/RadioModel.h"
#include "TestSettingsProfile.h"

#include "core/backends/hl2/MetisProtocol.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QHostAddress>
#include <QJsonObject>
#include <QNetworkDatagram>
#include <QSignalSpy>
#include <QTimer>
#include <QUdpSocket>

#include <cstdint>
#include <cstdio>

using namespace AetherSDR;
namespace hl2 = AetherSDR::hl2;

static int g_failures = 0;
static void check(bool cond, const char* what)
{
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

static QByteArray fakeEp6(std::uint32_t seq)
{
    QByteArray p(static_cast<int>(hl2::kUsbPacketSize), 0);
    auto* b = reinterpret_cast<std::uint8_t*>(p.data());
    b[0] = 0xEF; b[1] = 0xFE; b[2] = 0x01; b[3] = 0x06;
    b[4] = static_cast<std::uint8_t>(seq >> 24); b[5] = static_cast<std::uint8_t>(seq >> 16);
    b[6] = static_cast<std::uint8_t>(seq >> 8);  b[7] = static_cast<std::uint8_t>(seq);
    b[8] = b[9] = b[10] = 0x7F;
    b[8 + hl2::kFrameSize] = b[9 + hl2::kFrameSize] = b[10 + hl2::kFrameSize] = 0x7F;
    return p;
}

static void spin(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

int main(int argc, char** argv)
{
    TestSettingsProfile settingsProfile(QStringLiteral("hl2-link-stats-model-test"));
    if (!settingsProfile.isValid()) {
        std::fprintf(stderr, "FAIL: could not create an isolated settings profile\n");
        return 1;
    }

    QCoreApplication app(argc, argv);

    // Same thinned fake radio hl2_link_stats_test uses, and for the same reason:
    // one EP6 per EP2 runs the full demod for content nothing here reads.
    constexpr int kAnswerEvery = 8;
    QUdpSocket radio;
    check(radio.bind(QHostAddress::LocalHost, 0), "fake radio binds");
    const quint16 radioPort = radio.localPort();
    std::uint32_t seq = 0;
    int requests = 0;
    bool radioAnswering = true;
    QObject::connect(&radio, &QUdpSocket::readyRead, &radio, [&] {
        while (radio.hasPendingDatagrams()) {
            const QNetworkDatagram dg = radio.receiveDatagram();
            if (radioAnswering && (++requests % kAnswerEvery) == 0)
                radio.writeDatagram(fakeEp6(seq++), dg.senderAddress(), dg.senderPort());
        }
    });

    RadioModel model;

    RadioInfo info;
    info.family  = QStringLiteral("hl2");
    info.serial  = QStringLiteral("00:1C:C0:00:00:01");
    info.address = QHostAddress(QHostAddress::LocalHost);
    info.port    = radioPort;

    QSignalSpy heartbeatSpy(&model, &RadioModel::pingReceived);
    QSignalSpy qualitySpy(&model, &RadioModel::networkQualityChanged);

    model.connectToRadio(info);
    check(model.panStream() == nullptr, "HL2 owns no PanadapterStream");

    // Long enough for the connect handshake plus several 1 s publish ticks.
    spin(3600);

    // ---- the reported symptom: the readouts are no longer structurally zero ----
    check(model.packetTotalCount() > 0,
          "packet total reaches the readout on a family with no PanadapterStream");
    check(model.rxBytes() > 0, "received bytes reach the readout");
    check(model.firstUdpPacketSeen(), "the first UDP packet is seen");
    check(model.networkQuality() != QStringLiteral("Off"),
          "a streaming HL2 is scored, not left Off for the session");
    check(!model.localUdpEndpoint().isEmpty()
              && model.localUdpEndpoint() != QStringLiteral("Not bound"),
          "the bound UDP endpoint is reported");
    check(model.localTcpEndpoint() == QStringLiteral("None (stream transport only)"),
          "no command plane is stated as such, not as a fault");

    // ---- the heartbeat beats ----
    check(heartbeatSpy.count() >= 2,
          "traffic arriving beats the heartbeat on a family that answers no ping");

    // ---- absent is not zero ----
    check(!model.hasLinkRtt(),
          "a stream-only transport reports no round trip to time");
    check(!model.hasStreamCategoryStats(),
          "a single-stream transport has no per-category breakdown");
    check(model.hasLinkTiming(),
          "delivery spacing IS measurable, and is reported once a window closes");

    // THE RESET INVARIANT. Both paths that start a scoring session go through
    // resetNetworkQualitySession(); when this was open-coded in each of them they
    // drifted, and the field the backend path forgot was m_lastPingRtt. On a
    // transport that reports rttMs < 0 nothing ever overwrites it, so a previous
    // Flex/WAN session's RTT would score THIS link — invisibly, because
    // hasLinkRtt() correctly hides it from every readout.
    check(model.lastPingRtt() == 0,
          "no RTT is carried into a session on a transport that measures none");
    check(model.maxPingRtt() == 0, "the session RTT maximum starts clean");

    const QJsonObject snapshot = model.troubleshootingSnapshot();
    const QJsonObject network = snapshot.value(QStringLiteral("radio"))
                                    .toObject()
                                    .value(QStringLiteral("network"))
                                    .toObject();
    // Guard the path itself: every check below reads a bool that defaults to
    // false on a missing key, so a wrong path would pass three of them silently.
    check(!network.isEmpty(), "the snapshot carries a radio.network block");
    check(network.value(QStringLiteral("source")).toString()
              == QStringLiteral("backend_link"),
          "the snapshot names the backend seam as the source");
    check(!network.value(QStringLiteral("rtt_measured")).toBool(),
          "the snapshot distinguishes an unmeasured RTT from a zero one");
    check(network.value(QStringLiteral("timing_measured")).toBool(),
          "the snapshot reports delivery timing as measured");
    check(!network.value(QStringLiteral("stream_categories_measured")).toBool(),
          "the snapshot reports the per-category split as inapplicable");

    // ---- the disconnect edge ----
    const int qualityBefore = qualitySpy.count();
    model.disconnectFromRadio();
    spin(600);

    // § 21.5: m_netState going to Off is not observable on its own — the status
    // field is written ONLY from this signal, and every other emitter of it hangs
    // off the transport being torn down. Without this the last quality the link
    // ever had stays on screen against a disconnected radio.
    check(qualitySpy.count() > qualityBefore,
          "the disconnect edge announces the network state rather than going quiet");
    check(model.networkQuality() == QStringLiteral("Off"),
          "a disconnected radio reads Off");

    // § 21.3, one state transition later. stopNetworkMonitor() drops the COUNTERS
    // — they belong to the session that ended — but whether this wire has a round
    // trip to time is a fact about the wire. Answering "yes" here puts every RTT
    // readout back on its Flex branch, where lastPingRtt()'s 0 renders as
    // "< 1 ms": the best latency the app can display, from a measurement that
    // never happened, on a radio that is not even connected.
    check(!model.hasLinkRtt(),
          "a disconnected stream-only transport still reports no round trip to time");
    check(!model.hasStreamCategoryStats(),
          "and still has no per-category breakdown");

    if (g_failures == 0)
        std::fprintf(stderr, "hl2_link_stats_model_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
