// TunerModel unit test — the TGXL state machine (#4092). Exercises
// TunerModel::applyChanges(TunerDelta): present-only application, change-gating,
// the tuning/antenna edge signals, and the single stateChanged. The wire→delta
// translation is covered separately by aetherd_tuner_decode_test.

#include "models/TunerModel.h"
#include "core/TgxlConnection.h"

#include <QCoreApplication>
#include <QSignalSpy>
#include <cstdio>

using namespace AetherSDR;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    qRegisterMetaType<TunerDelta>();

    // ---- full apply + change-gated stateChanged ----
    {
        TunerModel t;
        QSignalSpy st(&t, &TunerModel::stateChanged);
        TunerDelta d;
        d.model = "TunerGeniusXL"; d.serialNum = "TG123";
        d.operate = true; d.bypass = false;
        d.relayC1 = 20; d.relayC2 = 5; d.relayL = 12;
        d.antennaA = 1; d.oneByThree = true; d.ip = "10.0.0.5";
        t.applyChanges(d);
        CHECK(t.modelName() == "TunerGeniusXL" && t.serialNum() == "TG123");
        CHECK(t.isOperate() && !t.isBypass());
        CHECK(t.relayC1() == 20 && t.relayC2() == 5 && t.relayL() == 12);
        CHECK(t.antennaA() == 1 && t.hasAntennaSwitch() && t.tgxlIp() == "10.0.0.5");
        CHECK(st.count() == 1);
        t.applyChanges(d);                // identical → change-gated, no re-emit
        CHECK(st.count() == 1);
    }

    // ---- absent fields are left untouched ----
    {
        TunerModel t;
        TunerDelta a; a.operate = true; a.relayC1 = 5;
        t.applyChanges(a);
        QSignalSpy st(&t, &TunerModel::stateChanged);
        TunerDelta b; b.operate = true;   // operate unchanged; relayC1 not present
        t.applyChanges(b);
        CHECK(t.relayC1() == 5 && st.count() == 0);
    }

    // ---- tuning + antenna edges emit their signals before stateChanged ----
    {
        TunerModel t;
        QSignalSpy tun(&t, &TunerModel::tuningChanged);
        QSignalSpy ant(&t, &TunerModel::antennaAChanged);
        TunerDelta d; d.tuning = true; d.antennaA = 2;
        t.applyChanges(d);
        CHECK(t.isTuning() && tun.count() == 1 && tun.takeFirst().at(0).toBool() == true);
        CHECK(t.antennaA() == 2 && ant.count() == 1 && ant.takeFirst().at(0).toInt() == 2);
        TunerDelta off; off.tuning = false;
        t.applyChanges(off);
        CHECK(!t.isTuning() && tun.count() == 1 && tun.takeFirst().at(0).toBool() == false);
    }

    // ---- encode: neutral relay intents; no-op without a handle (#4092) ----
    // The model emits vendor-neutral intents (no SmartSDR strings); FlexBackend
    // translates them — see aetherd_amp_tuner_encode_test for the wire form.
    {
        TunerModel t;
        QSignalSpy op(&t, &TunerModel::operateRequested);
        QSignalSpy by(&t, &TunerModel::bypassRequested);
        QSignalSpy at(&t, &TunerModel::autotuneRequested);

        // No handle yet (no direct conn) → every relay verb is a no-op.
        t.setOperate(true); t.setBypass(true); t.autoTune();
        CHECK(op.count() == 0 && by.count() == 0 && at.count() == 0);

        t.setHandle("0x2000");
        QSignalSpy st(&t, &TunerModel::stateChanged);

        t.setOperate(true);
        CHECK(op.count() == 1 && op.takeFirst().at(0).toBool() == true);
        CHECK(t.isOperate() && st.count() == 1);          // optimistic state update
        t.setBypass(true);
        CHECK(by.count() == 1 && by.takeFirst().at(0).toBool() == true);
        CHECK(t.isBypass() && st.count() == 2);
        t.autoTune();                                      // no direct conn → relay intent
        CHECK(at.count() == 1);
    }

    // ---- direct-channel kv apply: relays land from the status poll too -----
    // Regression for the frozen C1/L/C2 bars: the relay positions arrive on the
    // 1/sec status poll reply, which the model used to parse for meters only.
    {
        TunerModel t;
        TgxlConnection conn;
        t.setDirectConnection(&conn);
        QSignalSpy st(&t, &TunerModel::stateChanged);

        emit conn.statusUpdated({{"relayC1", "17"}, {"relayL", "4"}, {"relayC2", "9"}});
        CHECK(t.relayC1() == 17 && t.relayL() == 4 && t.relayC2() == 9);
        CHECK(st.count() == 1);
        emit conn.statusUpdated({{"relayC1", "17"}, {"relayL", "4"}, {"relayC2", "9"}});
        CHECK(st.count() == 1);                       // change-gated, no re-emit
        emit conn.stateUpdated({{"relayC1", "18"}});   // the push path still works
        CHECK(t.relayC1() == 18 && st.count() == 2);
    }

    // ---- direct-channel antenna-switch detection via the info reply --------
    {
        TunerModel t;
        TgxlConnection conn;
        t.setDirectConnection(&conn);
        CHECK(!t.hasAntennaSwitch());
        emit conn.infoUpdated({{"3way", "1"}});
        CHECK(t.hasAntennaSwitch());

        // A tuner without the switch omits the key; the relayed Flex key is
        // an independent source of the same fact.
        TunerModel u;
        TunerDelta d; d.oneByThree = true;
        u.applyChanges(d);
        CHECK(u.hasAntennaSwitch());
    }

    // ---- direct-channel operate reaches the tuner without a Flex handle ----
    // Regression for the dead Standby button: setOperate used to require a
    // handle, which a direct-only TGXL (#2250) never has.
    {
        TunerModel t;
        TgxlConnection conn;
        t.setDirectConnection(&conn);
        QSignalSpy op(&t, &TunerModel::operateRequested);
        QSignalSpy st(&t, &TunerModel::stateChanged);

        // No handle and no live socket → still a no-op, and no relay intent.
        t.setOperate(true);
        CHECK(op.count() == 0 && !t.isOperate() && st.count() == 0);
        CHECK(!t.hasRadioRelay());

        // With a handle the relay intent is emitted as before.
        t.setHandle("0x2000");
        CHECK(t.hasRadioRelay());
        t.setOperate(true);
        CHECK(op.count() == 1 && t.isOperate());

        // The tuner's own report wins over the optimistic update.
        emit conn.statusUpdated({{"state", "0"}});
        CHECK(!t.isOperate());
    }

    // ---- operate state is learned from the tuner at startup ---------------
    // Regression for the button coming up STANDBY on a tuner that is online:
    // m_operate defaults to false, so the first status reply has to establish
    // the true state before the operator touches anything (#4552).
    {
        TunerModel t;
        TgxlConnection conn;
        t.setDirectConnection(&conn);
        QSignalSpy st(&t, &TunerModel::stateChanged);
        CHECK(!t.isOperate());                       // default before any status

        emit conn.statusUpdated({{"state", "1"}, {"relayC1", "3"}});
        CHECK(t.isOperate() && st.count() == 1);     // no click needed
        emit conn.statusUpdated({{"state", "1"}});
        CHECK(st.count() == 1);                      // change-gated
        emit conn.statusUpdated({{"state", "0"}});   // 0 = standby
        CHECK(!t.isOperate() && st.count() == 2);
    }

    // ---- "state" is the operate flag ONLY in the status reply -------------
    // The unsolicited push is itself the "state" object; a key of that name in
    // the push or in the info reply is not the operate flag and must not move
    // the button.
    {
        TunerModel t;
        TgxlConnection conn;
        t.setDirectConnection(&conn);
        emit conn.statusUpdated({{"state", "1"}});
        CHECK(t.isOperate());

        emit conn.stateUpdated({{"state", "0"}});    // push — ignored
        CHECK(t.isOperate());
        emit conn.infoUpdated({{"state", "0"}});     // info reply — ignored
        CHECK(t.isOperate());
        emit conn.statusUpdated({{"state", "0"}});   // status reply — applied
        CHECK(!t.isOperate());

        // A status carrying no "state" leaves the flag alone.
        emit conn.statusUpdated({{"state", "1"}});
        emit conn.statusUpdated({{"fwd", "40.0"}});
        CHECK(t.isOperate());
    }

    if (g_failures == 0) {
        std::printf("tuner_model_test: all checks passed\n");
        return 0;
    }
    std::printf("tuner_model_test: %d failure(s)\n", g_failures);
    return 1;
}
