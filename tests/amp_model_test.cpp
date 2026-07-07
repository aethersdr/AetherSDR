// AmpModel unit test — pins the power-amplifier (PGXL) state machine extracted
// from RadioModel (#4094): presence/operate derivation, telemetry forwarding,
// removal, reset, and the operate-command relay. Guards the behavior-neutral
// extraction (the 73 model tests didn't cover amp state directly).

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

static QMap<QString, QString> kv(std::initializer_list<std::pair<QString, QString>> l)
{
    QMap<QString, QString> m;
    for (const auto& p : l) m.insert(p.first, p.second);
    return m;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // ---- presence detection: ip/model captured on the first status only ----
    {
        AmpModel amp;
        QSignalSpy presence(&amp, &AmpModel::presenceChanged);
        amp.applyStatus("0x1000", "PowerGeniusXL", kv({{"ip", "192.168.1.50"}, {"state", "STANDBY"}}));
        CHECK(amp.present());
        CHECK(amp.handle() == "0x1000");
        CHECK(amp.ip() == "192.168.1.50");
        CHECK(amp.modelName() == "PowerGeniusXL");
        CHECK(!amp.operate());                 // STANDBY → off
        CHECK(presence.count() == 1);
        CHECK(presence.takeFirst().at(0).toBool() == true);
    }

    // ---- TGXL is NOT a power amp (never marks present) ----
    {
        AmpModel amp;
        amp.applyStatus("0x2000", "TunerGeniusXL", kv({{"state", "OPERATE"}}));
        CHECK(!amp.present());
        CHECK(amp.handle().isEmpty());
    }

    // ---- operate derivation: IDLE/OPERATE/TRANSMIT* = on, STANDBY = off; change-gated ----
    {
        AmpModel amp;
        amp.applyStatus("0x1000", "PowerGeniusXL", kv({{"state", "STANDBY"}}));
        QSignalSpy st(&amp, &AmpModel::stateChanged);
        amp.applyStatus("0x1000", "", kv({{"state", "IDLE"}}));       // later updates omit model
        CHECK(amp.operate() && st.count() == 1);
        amp.applyStatus("0x1000", "", kv({{"state", "OPERATE"}}));    // still on → no re-emit
        CHECK(amp.operate() && st.count() == 1);
        amp.applyStatus("0x1000", "", kv({{"state", "TRANSMIT_A"}})); // keyed → on
        CHECK(amp.operate() && st.count() == 1);
        amp.applyStatus("0x1000", "", kv({{"state", "STANDBY"}}));    // off
        CHECK(!amp.operate() && st.count() == 2);
    }

    // ---- telemetry emitted for a matching handle, ignored otherwise ----
    {
        AmpModel amp;
        amp.applyStatus("0x1000", "PowerGeniusXL", kv({{"state", "IDLE"}}));
        QSignalSpy tel(&amp, &AmpModel::telemetryUpdated);
        amp.applyStatus("0x1000", "", kv({{"temp", "42"}, {"id", "3.1"}}));
        CHECK(tel.count() == 1);
        amp.applyStatus("0x9999", "", kv({{"temp", "99"}}));          // foreign handle → ignored
        CHECK(tel.count() == 1);
    }

    // ---- removal clears presence for our handle only ----
    {
        AmpModel amp;
        amp.applyStatus("0x1000", "PowerGeniusXL", kv({{"state", "IDLE"}}));
        CHECK(amp.present());
        QSignalSpy presence(&amp, &AmpModel::presenceChanged);
        amp.handleRemoval("0x9999");           // not ours → no-op
        CHECK(amp.present() && presence.count() == 0);
        amp.handleRemoval("0x1000");
        CHECK(!amp.present() && amp.handle().isEmpty());
        CHECK(presence.count() == 1 && presence.takeFirst().at(0).toBool() == false);
    }

    // ---- setOperate relays the SmartSDR verb; no-op without a handle ----
    {
        AmpModel amp;
        QSignalSpy cmd(&amp, &AmpModel::commandReady);
        amp.setOperate(true);                  // no handle yet → nothing
        CHECK(cmd.count() == 0);
        amp.applyStatus("0x1000", "PowerGeniusXL", kv({{"state", "STANDBY"}}));
        amp.setOperate(true);
        CHECK(cmd.count() == 1);
        CHECK(cmd.takeFirst().at(0).toString() == "amplifier set 0x1000 operate=1");
        amp.setOperate(false);
        CHECK(cmd.count() == 1);
        CHECK(cmd.takeFirst().at(0).toString() == "amplifier set 0x1000 operate=0");
    }

    // ---- reset clears present/handle/operate ----
    {
        AmpModel amp;
        amp.applyStatus("0x1000", "PowerGeniusXL", kv({{"state", "IDLE"}}));
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
