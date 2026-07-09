// TunerModel unit test — the TGXL state machine (#4092). Exercises
// TunerModel::applyChanges(TunerDelta): present-only application, change-gating,
// the tuning/antenna edge signals, and the single stateChanged. The wire→delta
// translation is covered separately by aetherd_tuner_decode_test.

#include "models/TunerModel.h"

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

    if (g_failures == 0) {
        std::printf("tuner_model_test: all checks passed\n");
        return 0;
    }
    std::printf("tuner_model_test: %d failure(s)\n", g_failures);
    return 1;
}
