// The RadioModel half of the HL2 link-stats seam, socket-free. The model is
// built with the production HL2 backend wiring (rebuildBackendForTest), and
// the backend's own seam signals are emitted directly: linkStatsUpdated for a
// publish tick, disconnected() for the disconnect edge.

#include "models/RadioModel.h"

#include "core/AppSettings.h"

#include "TestSettingsProfile.h"

#include <QCoreApplication>
#include <QJsonObject>
#include <QSignalSpy>

#include <cstdio>

using namespace AetherSDR;

static int g_failures = 0;
static void check(bool ok, const char* what)
{
    std::fprintf(stderr, "[%s] %s\n", ok ? " OK " : "FAIL", what);
    if (!ok)
        ++g_failures;
}

static IRadioBackend::LinkStats streamingTick(quint64 rxPackets, int rttMs = -1)
{
    IRadioBackend::LinkStats s;
    s.reported = true;
    s.alive = true;
    s.rxPackets = rxPackets;
    s.rxBytes = static_cast<qint64>(rxPackets) * 1032;
    s.txBytes = 4096;
    s.rttMs = rttMs;
    s.gapMs = 3;
    s.gapMaxMs = 9;
    s.jitterMs = 6;
    s.localEndpoint = QStringLiteral("192.0.2.7:50000");
    return s;
}

static QJsonObject networkBlock(const RadioModel& model)
{
    return model.troubleshootingSnapshot()
        .value(QStringLiteral("radio")).toObject()
        .value(QStringLiteral("network")).toObject();
}

// A transport that has never measured an RTT: readouts, predicates and the
// diagnostics block on both sides of the disconnect edge.
static void streamOnlyTransport()
{
    RadioModel model;
    if (!model.rebuildBackendForTest(QStringLiteral("hl2")) || !model.backend()) {
        check(false, "the hl2 backend is built into this binary");
        return;
    }
    IRadioBackend* backend = model.backend();
    QSignalSpy beats(&model, &RadioModel::pingReceived);
    QSignalSpy quality(&model, &RadioModel::networkQualityChanged);

    check(model.panStream() == nullptr, "HL2 owns no PanadapterStream");
    check(model.packetTotalCount() == 0 && model.networkQuality() == QStringLiteral("Off"),
          "before any snapshot the readouts are zero and quality is Off");

    IRadioBackend::LinkStats unreported = streamingTick(10);
    unreported.reported = false;
    emit backend->linkStatsUpdated(unreported);
    check(model.packetTotalCount() == 0 && beats.isEmpty()
              && model.networkQuality() == QStringLiteral("Off"),
          "a snapshot that does not report is ignored");

    emit backend->linkStatsUpdated(streamingTick(500));
    check(model.packetTotalCount() == 500, "packet total comes from the backend seam");
    check(model.rxBytes() == 500 * 1032 && model.txBytes() == 4096,
          "byte totals come from the backend seam");
    check(model.firstUdpPacketSeen(), "the first UDP packet is seen");
    check(model.networkQuality() != QStringLiteral("Off"), "a streaming HL2 is scored, not Off");
    check(model.localUdpEndpoint() == QStringLiteral("192.0.2.7:50000"),
          "the bound UDP endpoint is the backend's");
    check(model.localTcpEndpoint() == QStringLiteral("None (stream transport only)"),
          "no command plane is stated as such, not as a fault");
    check(beats.count() == 1, "an alive tick beats the heartbeat");
    IRadioBackend::LinkStats quiet = streamingTick(500);
    quiet.alive = false;
    emit backend->linkStatsUpdated(quiet);
    check(beats.count() == 1, "a silent tick does not beat the heartbeat");

    check(!model.hasLinkRtt(), "a stream-only transport has no RTT to show");
    check(model.hasLinkTiming(), "delivery timing is measured");
    check(!model.hasStreamCategoryStats(), "a single stream has no per-category split");
    const QJsonObject live = networkBlock(model);
    check(!live.isEmpty(), "the diagnostics snapshot carries radio.network");
    check(live.value(QStringLiteral("source")).toString() == QStringLiteral("backend_link"),
          "radio.network names the backend seam as its source");
    check(!live.value(QStringLiteral("rtt_measured")).toBool(true),
          "radio.network says the RTT is unmeasured");
    check(live.value(QStringLiteral("timing_measured")).toBool(false),
          "radio.network says timing is measured");
    check(!live.value(QStringLiteral("stream_categories_measured")).toBool(true),
          "radio.network says the category split is inapplicable");

    const int announcedBefore = quality.count();
    emit backend->disconnected();
    check(quality.count() > announcedBefore, "the disconnect edge announces the quality");
    check(model.networkQuality() == QStringLiteral("Off"), "a disconnected radio reads Off");
    check(model.packetTotalCount() == 0 && model.rxBytes() == 0 && model.txBytes() == 0
              && !model.firstUdpPacketSeen(),
          "the session's counters reset to zero on disconnect");
    check(model.localUdpEndpoint() == QStringLiteral("Not bound"),
          "the UDP endpoint is no longer reported");
    check(!model.hasLinkRtt(), "after disconnect the transport still has no RTT to show");
    check(model.hasLinkTiming(), "and its timing stays measurable");
    check(!model.hasStreamCategoryStats(), "and it still has no category split");
    check(!networkBlock(model).value(QStringLiteral("rtt_measured")).toBool(true),
          "radio.network still says the RTT is unmeasured after disconnect");
}

// Each scoring session starts with no RTT, even when the previous session's
// transport measured one.
static void perSessionRttReset()
{
    RadioModel model;
    if (!model.rebuildBackendForTest(QStringLiteral("hl2")) || !model.backend())
        return;
    IRadioBackend* backend = model.backend();

    emit backend->linkStatsUpdated(streamingTick(100, 50));
    check(model.lastPingRtt() == 50, "a measured RTT reaches the model");
    emit backend->disconnected();

    emit backend->linkStatsUpdated(streamingTick(100));
    check(model.lastPingRtt() == 0, "a new session does not inherit the previous RTT");
    check(model.maxPingRtt() == 0, "the session RTT maximum starts clean");
}

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("hl2-link-stats-model-seam-test"));
    if (!profile.isValid()) {
        std::fprintf(stderr, "FAIL: could not create an isolated settings profile\n");
        return 1;
    }
    QCoreApplication app(argc, argv);
    AppSettings::instance().load();

    streamOnlyTransport();
    perSessionRttReset();

    std::fprintf(stderr, "hl2_link_stats_model_seam_test: %s\n",
                 g_failures == 0 ? "all checks passed" : "FAILED");
    return g_failures == 0 ? 0 : 1;
}
