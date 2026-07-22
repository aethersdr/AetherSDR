// aetherd 2.4 (#4094) — FlexBackend::decodeAmplifierStatus. Pins the SmartSDR
// "amplifier <handle> …" wire → typed AmpDelta translation that moved out of
// RadioModel: power-amp detection (non-TGXL model), TGXL discrimination, ip
// capture, operate derivation from "state", telemetry passthrough, removal.

#include "core/backends/flex/FlexBackend.h"
#include "core/backends/AmpDelta.h"

#include <QCoreApplication>
#include <QSignalSpy>
#include <QString>
#include <QVariant>
#include <cstdio>

using namespace AetherSDR;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

static AmpDelta decode(FlexBackend& b, const QString& handle, const QString& model,
                       const QMap<QString, QString>& kvs, bool removed)
{
    QSignalSpy spy(&b, &IRadioBackend::amplifierChanged);
    b.decodeAmplifierStatus(handle, model, kvs, removed);
    if (spy.count() != 1) return {};
    return spy.takeFirst().at(0).value<AmpDelta>();
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    qRegisterMetaType<AmpDelta>();
    FlexBackend b;

    // ---- power amp (PGXL): detected model + ip + operate-from-state ----
    {
        const AmpDelta d = decode(b, "0x1000", "PowerGeniusXL",
            {{"ip", "192.168.1.50"}, {"state", "IDLE"}, {"temp", "35"}}, false);
        CHECK(d.handle == "0x1000");
        CHECK(!d.removed);
        CHECK(d.detectedModel.has_value() && *d.detectedModel == "PowerGeniusXL");
        CHECK(d.ip.has_value() && *d.ip == "192.168.1.50");
        CHECK(d.operate.has_value() && *d.operate == true);   // IDLE → on
        CHECK(d.telemetry.value("temp") == "35");             // full kvs forwarded
    }

    // ---- TGXL is NOT a power amp: no detectedModel (routes to TunerModel) ----
    {
        const AmpDelta d = decode(b, "0x2000", "TunerGeniusXL", {{"state", "OPERATE"}}, false);
        CHECK(!d.detectedModel.has_value());
        CHECK(d.operate.has_value() && *d.operate == true);   // still decoded, model gates it
        CHECK(d.handle == "0x2000");
    }

    // ---- operate derivation: OPERATE/TRANSMIT* → on, STANDBY → off, absent → nullopt ----
    {
        CHECK(*decode(b, "0x1", "PowerGeniusXL", {{"state", "OPERATE"}}, false).operate == true);
        CHECK(*decode(b, "0x1", "PowerGeniusXL", {{"state", "TRANSMIT_A"}}, false).operate == true);
        CHECK(*decode(b, "0x1", "PowerGeniusXL", {{"state", "STANDBY"}}, false).operate == false);
        CHECK(!decode(b, "0x1", "PowerGeniusXL", {{"ip", "x"}}, false).operate.has_value());   // no state key
        CHECK(!decode(b, "0x1", "PowerGeniusXL", {{"state", ""}}, false).operate.has_value()); // bare state= → skipped
    }

    // ---- ip only latched when present ----
    {
        const AmpDelta d = decode(b, "0x1", "PowerGeniusXL", {{"state", "IDLE"}}, false);
        CHECK(!d.ip.has_value());
    }

    // ---- removal: removed flag set, nothing else decoded ----
    {
        const AmpDelta d = decode(b, "0x1000", QString(), {}, true);
        CHECK(d.removed && d.handle == "0x1000");
        CHECK(!d.detectedModel.has_value() && !d.operate.has_value());
    }

    // ---- #4203: a known-tuner handle mis-routed into the amp decode must NOT be
    // cached as m_ampHandle. Observe via the encode path: with no amp handle
    // cached, amp.operate fails closed (extensionError), never targeting the TGXL.
    {
        FlexBackend g;
        // Establish the tuner handle (#4198 encode-path cache).
        g.decodeTunerStatus("0x2000", {{"model", "TunerGeniusXL"}});
        // Model-less TGXL status that falls through to decodeAmplifierStatus (the
        // pre-existing routing edge: model empty AND handle == known tuner handle).
        (void)decode(g, "0x2000", QString(), {{"state", "OPERATE"}}, false);
        // Guard held: m_ampHandle stayed empty, so amp.operate reports no handle.
        QSignalSpy okSpy(&g, &IRadioBackend::extensionResult);
        QSignalSpy errSpy(&g, &IRadioBackend::extensionError);
        g.invokeExtension("flex", "amp.operate", /*requestId=*/1, QVariantMap{{"on", true}});
        CHECK(errSpy.count() == 1 && okSpy.count() == 0);   // fails closed, never targets TGXL

        // Positive control: a genuine PGXL handle (≠ tuner handle) still caches and
        // amp.operate dispatches against it.
        (void)decode(g, "0x9000", "PowerGeniusXL", {{"state", "IDLE"}}, false);
        QSignalSpy okSpy2(&g, &IRadioBackend::extensionResult);
        QSignalSpy errSpy2(&g, &IRadioBackend::extensionError);
        g.invokeExtension("flex", "amp.operate", /*requestId=*/2, QVariantMap{{"on", true}});
        CHECK(okSpy2.count() == 1 && errSpy2.count() == 0);
    }

    if (g_failures == 0) {
        std::printf("aetherd_amp_decode_test: all checks passed\n");
        return 0;
    }
    std::printf("aetherd_amp_decode_test: %d failure(s)\n", g_failures);
    return 1;
}
