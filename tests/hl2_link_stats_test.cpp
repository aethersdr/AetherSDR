// HL2 transport telemetry — the IRadioBackend::linkStats seam.
//
// The network readouts (title-bar heartbeat, status-bar Network field, the
// Network Diagnostics pane) were written against the Flex stack and read their
// numbers off a RadioConnection and a PanadapterStream. The HL2 owns neither,
// so every one of them rendered a structural zero on a link that was working:
// the heartbeat never left its pre-connect amber and the diagnostics pane
// reported a connected radio pushing 0 kbps.
//
// This covers the replacement end to end against a fake HL2 on localhost: that
// the backend publishes a transport snapshot on a fixed cadence, that the
// counters in it are real, that "the radio has gone quiet" is distinguishable
// from "the link is fine", and — the one that is easy to get wrong — that a
// figure this transport CANNOT measure is reported as absent rather than as
// zero. A zero RTT formats as "< 1 ms", which is a link-quality claim nothing
// ever measured.

#include "core/backends/IRadioBackend.h"
#include "TestSettingsProfile.h"
#include "TestDspBuildWait.h"

#include "core/backends/hl2/Hl2Backend.h"
#include "core/backends/hl2/MetisProtocol.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QHostAddress>
#include <QNetworkDatagram>
#include <QSignalSpy>
#include <QTimer>
#include <QUdpSocket>

#include <cstdint>
#include <cstdio>
#include <vector>

using namespace AetherSDR;
using AetherSDR::hl2::Hl2Backend;
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
    // Isolated settings, for the same reason hl2_backend_test uses one: the
    // backend persists span state at connect, and a test must never write it
    // into the operator's real configuration.
    TestSettingsProfile settingsProfile(QStringLiteral("hl2-link-stats-test"));
    if (!settingsProfile.isValid()) {
        std::fprintf(stderr, "FAIL: could not create an isolated settings profile\n");
        return 1;
    }

    QCoreApplication app(argc, argv);

    // ---- fake HL2: a thinned EP6 stream, until told to go quiet ----
    //
    // One EP6 per EP2 would be the real 381 packets/s, and every one of them
    // runs the full WDSP demod inside the backend — for a stream whose CONTENT
    // nothing here looks at. Answering one request in eight keeps the counters,
    // the wakeup cadence and the sequence continuity all real at an eighth of
    // the sample-path cost. (hl2_backend_test caps its stream outright for the
    // same reason.) Wall time is dominated by WDSP channel setup either way;
    // the saving is CPU, which is what matters under the weekly TSan job.
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

    Hl2Backend backend;

    // Before a connection there is no transport to describe. A snapshot that
    // claimed to report while every counter was zero would be indistinguishable
    // from a dead link, which is the state this whole path exists to stop
    // misrendering.
    check(!backend.linkStats().reported,
          "linkStats() does not claim to report before connect");

    std::vector<IRadioBackend::LinkStats> ticks;
    QObject::connect(&backend, &IRadioBackend::linkStatsUpdated, &backend,
                     [&](const IRadioBackend::LinkStats& s) { ticks.push_back(s); });

    QSignalSpy connectedSpy(&backend, &IRadioBackend::connected);
    RadioConnectRequest req;
    req.host = QHostAddress(QHostAddress::LocalHost).toString();
    req.port = radioPort;
    backend.connectRadio(req);

    // Wait out the asynchronous DSP build, THEN time the publish cadence: the
    // 2.6 s below is the assertion (>= two 1 s ticks) and must not be spent
    // waiting for FFTW to finish planning.
    AetherSDR::test::awaitDspBuild("hl2_link_stats_test",
                                  [&] { return connectedSpy.count() >= 1; });
    spin(2600);
    check(connectedSpy.count() == 1, "backend connected on first EP6");

    check(ticks.size() >= 2, "linkStatsUpdated ticks on its fixed cadence while connected");
    if (ticks.empty()) {
        std::fprintf(stderr, "FAIL: no link-stats ticks; remaining checks skipped\n");
        return 1;
    }

    const IRadioBackend::LinkStats& live = ticks.back();
    check(live.reported, "a connected backend reports its transport");
    check(live.alive, "traffic arriving marks the link alive");
    check(live.rxPackets > 0, "EP6 packets are counted");
    check(live.rxBytes > 0, "received bytes are counted");
    // EP2 is paced off a wall clock and flows whether or not the radio answers,
    // so a connected session always has outbound traffic to report.
    check(live.txBytes > 0, "sent bytes are counted");
    check(!live.localEndpoint.isEmpty(), "the bound local endpoint is reported");
    check(live.rxPacketsLost == 0, "an ordered loopback stream loses nothing");

    // THE HONESTY INVARIANT. Protocol 1 is a one-way stream: EP2 goes out on a
    // wall clock, EP6 comes back free-running, and no frame in either direction
    // answers a specific frame in the other. There is no round trip to time, so
    // the field must read "not measured" (negative) and never 0 — every readout
    // formats a 0 as "< 1 ms" and would advertise a latency nobody measured.
    check(live.rttMs < 0, "RTT is reported as not-measured, not as zero");
    // The gap figures ARE measurable on this transport, and are what the
    // quality score has to run on instead.
    check(live.gapMs >= 0, "inter-arrival gap is measured");
    check(live.gapMaxMs >= live.gapMs, "the window maximum is not below its mean");
    check(live.jitterMs >= 0, "delivery spread is measured");

    // Counters are cumulative across ticks, never per-window.
    if (ticks.size() >= 2) {
        const IRadioBackend::LinkStats& earlier = ticks[ticks.size() - 2];
        check(live.rxBytes >= earlier.rxBytes, "rx bytes accumulate across ticks");
        check(live.rxPackets >= earlier.rxPackets, "rx packets accumulate across ticks");
    }

    // ---- the radio goes quiet ----
    //
    // This is the case the heartbeat exists for, and a cumulative counter alone
    // cannot express it: the totals simply stop moving, which is identical to
    // reading them twice quickly. `alive` is the per-tick difference, so a tick
    // over a silent second must say so while still reporting the link.
    //
    // Sampled inside the client's own silence window (2 s), so this observes the
    // quiet link rather than the disconnect that eventually follows it.
    radioAnswering = false;
    const std::size_t ticksBeforeSilence = ticks.size();
    spin(1500);
    bool sawSilentTick = false;
    bool silentTickStillReported = true;
    for (std::size_t i = ticksBeforeSilence; i < ticks.size(); ++i) {
        if (!ticks[i].alive) {
            sawSilentTick = true;
            silentTickStillReported = silentTickStillReported && ticks[i].reported;
        }
    }
    check(sawSilentTick, "a tick over a silent second reports the link as not alive");
    check(silentTickStillReported,
          "a silent link is still reported — quiet is not the same as unmeasured");

    // ---- disconnect stops the cadence ----
    backend.disconnectRadio();
    spin(300);
    const std::size_t ticksAtDisconnect = ticks.size();
    spin(1500);
    check(ticks.size() == ticksAtDisconnect,
          "no link-stats ticks after disconnect");
    check(!backend.linkStats().reported,
          "a disconnected backend stops claiming to report a transport");

    if (g_failures == 0)
        std::fprintf(stderr, "hl2_link_stats_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
