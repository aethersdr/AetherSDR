// AmpModel unit test — the power-amplifier state machine (#4094). Exercises
// AmpModel::applyChanges(AmpDelta): presence latch, operate change-gating,
// telemetry/handle matching, removal, reset, and the operate-command relay.
// The wire→AmpDelta translation is covered separately by aetherd_amp_decode_test.

#include "models/AmpModel.h"

#include <QCoreApplication>
#include <QMap>
#include <QSignalSpy>
#include <QString>
#include <cstdio>

using namespace AetherSDR;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

// A "detected power amp" status delta (as FlexBackend would decode it).
static AmpDelta detected(const QString& handle, const QString& model,
                         const QString& ip, std::optional<bool> operate,
                         QMap<QString, QString> telem = {})
{
    AmpDelta d;
    d.handle = handle;
    d.detectedModel = model;
    if (!ip.isEmpty()) d.ip = ip;
    d.operate = operate;
    d.telemetry = std::move(telem);
    return d;
}

// A follow-up status for an already-detected amp (no model, same handle).
static AmpDelta update(const QString& handle, std::optional<bool> operate,
                       QMap<QString, QString> telem = {})
{
    AmpDelta d;
    d.handle = handle;
    d.operate = operate;
    d.telemetry = std::move(telem);
    return d;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    qRegisterMetaType<AmpDelta>();

    // ---- presence latch: model/ip captured once, on the first detect ----
    {
        AmpModel amp;
        QSignalSpy presence(&amp, &AmpModel::presenceChanged);
        amp.applyChanges(detected("0x1000", "PowerGeniusXL", "192.168.1.50", false));
        CHECK(amp.present());
        CHECK(amp.handle() == "0x1000");
        CHECK(amp.ip() == "192.168.1.50");
        CHECK(amp.modelName() == "PowerGeniusXL");
        CHECK(!amp.operate());
        CHECK(presence.count() == 1 && presence.takeFirst().at(0).toBool() == true);
        // A second detect does not re-latch ip/model or re-emit presence.
        amp.applyChanges(detected("0x1000", "PowerGeniusXL", "10.0.0.9", true));
        CHECK(amp.ip() == "192.168.1.50");            // unchanged
        CHECK(presence.count() == 0);
    }

    // ---- a delta with no detectedModel + unknown handle is a no-op (TGXL case) ----
    {
        AmpModel amp;
        QSignalSpy presence(&amp, &AmpModel::presenceChanged);
        QSignalSpy tel(&amp, &AmpModel::telemetryUpdated);
        amp.applyChanges(update("0x2000", true, {{"state", "OPERATE"}}));
        CHECK(!amp.present());
        CHECK(presence.count() == 0 && tel.count() == 0);
    }

    // ---- operate change-gating; absent operate leaves it as-is ----
    {
        AmpModel amp;
        amp.applyChanges(detected("0x1000", "PowerGeniusXL", "", false));
        QSignalSpy st(&amp, &AmpModel::stateChanged);
        amp.applyChanges(update("0x1000", true));     // off→on
        CHECK(amp.operate() && st.count() == 1);
        amp.applyChanges(update("0x1000", true));     // on→on: no re-emit
        CHECK(amp.operate() && st.count() == 1);
        amp.applyChanges(update("0x1000", std::nullopt, {{"temp", "40"}}));  // no state key
        CHECK(amp.operate() && st.count() == 1);      // operate unchanged
        amp.applyChanges(update("0x1000", false));    // on→off
        CHECK(!amp.operate() && st.count() == 2);
    }

    // ---- telemetry forwarded for a matching handle, ignored otherwise ----
    {
        AmpModel amp;
        amp.applyChanges(detected("0x1000", "PowerGeniusXL", "", true));
        QSignalSpy tel(&amp, &AmpModel::telemetryUpdated);
        amp.applyChanges(update("0x1000", std::nullopt, {{"temp", "42"}, {"id", "3.1"}}));
        CHECK(tel.count() == 1);
        amp.applyChanges(update("0x9999", std::nullopt, {{"temp", "99"}}));  // foreign handle
        CHECK(tel.count() == 1);
    }

    // ---- removal clears presence for our handle only ----
    {
        AmpModel amp;
        amp.applyChanges(detected("0x1000", "PowerGeniusXL", "", true));
        QSignalSpy presence(&amp, &AmpModel::presenceChanged);
        AmpDelta rmOther; rmOther.handle = "0x9999"; rmOther.removed = true;
        amp.applyChanges(rmOther);                    // not ours → no-op
        CHECK(amp.present() && presence.count() == 0);
        AmpDelta rm; rm.handle = "0x1000"; rm.removed = true;
        amp.applyChanges(rm);
        CHECK(!amp.present() && amp.handle().isEmpty());
        CHECK(presence.count() == 1 && presence.takeFirst().at(0).toBool() == false);
    }

    // ---- setOperate relays the SmartSDR verb; no-op without a handle ----
    {
        AmpModel amp;
        QSignalSpy cmd(&amp, &AmpModel::commandReady);
        amp.setOperate(true);                         // no handle yet
        CHECK(cmd.count() == 0);
        amp.applyChanges(detected("0x1000", "PowerGeniusXL", "", false));
        amp.setOperate(true);
        CHECK(cmd.count() == 1);
        CHECK(cmd.takeFirst().at(0).toString() == "amplifier set 0x1000 operate=1");
        amp.setOperate(false);
        CHECK(cmd.takeFirst().at(0).toString() == "amplifier set 0x1000 operate=0");
    }

    // ---- reset clears present/handle/operate ----
    {
        AmpModel amp;
        amp.applyChanges(detected("0x1000", "PowerGeniusXL", "", true));
        amp.reset();
        CHECK(!amp.present() && amp.handle().isEmpty() && !amp.operate());
    }

    if (g_failures == 0) {
        std::printf("amp_model_test: all checks passed\n");
        return 0;
    }
    std::printf("amp_model_test: %d failure(s)\n", g_failures);
    return 1;
}
