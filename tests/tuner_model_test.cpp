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
        QSignalSpy antennaEdge(&t, &TunerModel::antennaAChanged);
        QSignalSpy tuningEdge(&t, &TunerModel::tuningChanged);
        bool operateAtPresence = false;
        bool bypassAtPresence = false;
        bool edgesDeferredAtPresence = false;
        QString ipAtPresence;
        QObject::connect(&t, &TunerModel::presenceChanged, &t,
                         [&t, &antennaEdge, &tuningEdge, &operateAtPresence,
                          &bypassAtPresence, &edgesDeferredAtPresence,
                          &ipAtPresence](bool present) {
            if (present) {
                operateAtPresence = t.isOperate();
                bypassAtPresence = t.isBypass();
                edgesDeferredAtPresence = antennaEdge.count() == 0
                    && tuningEdge.count() == 0;
                ipAtPresence = t.tgxlIp();
            }
        });
        TunerDelta d;
        d.handle = "0x2000";
        d.model = "TunerGeniusXL"; d.serialNum = "TG123";
        d.operate = true; d.bypass = false; d.tuning = true;
        d.relayC1 = 20; d.relayC2 = 5; d.relayL = 12;
        d.antennaA = 1; d.oneByThree = true; d.ip = "10.0.0.5";
        t.applyChanges(d);
        CHECK(t.modelName() == "TunerGeniusXL" && t.serialNum() == "TG123");
        CHECK(t.isOperate() && !t.isBypass());
        CHECK(t.relayC1() == 20 && t.relayC2() == 5 && t.relayL() == 12);
        CHECK(t.antennaA() == 1 && t.hasAntennaSwitch() && t.tgxlIp() == "10.0.0.5");
        CHECK(t.isPresent() && operateAtPresence && !bypassAtPresence
              && edgesDeferredAtPresence);
        CHECK(ipAtPresence == "10.0.0.5");
        CHECK(antennaEdge.count() == 1 && tuningEdge.count() == 1);
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

    // ---- direct presence with no radio handle survives a handle-less delta ----
    {
        TunerModel t;
        TgxlConnection direct;
        t.setDirectConnection(&direct);
        QSignalSpy presence(&t, &TunerModel::presenceChanged);
        CHECK(QMetaObject::invokeMethod(&direct, "connected", Qt::DirectConnection));
        CHECK(t.isPresent() && t.handle().isEmpty() && presence.count() == 1);

        TunerDelta directState;
        directState.operate = true;
        t.applyChanges(directState);
        CHECK(t.isPresent() && t.handle().isEmpty() && t.isOperate());
        CHECK(presence.count() == 1);
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

    // ---- per-port PTT and the pttChanged edge ----
    {
        TunerModel t;
        QSignalSpy st(&t, &TunerModel::stateChanged);
        QSignalSpy ptt(&t, &TunerModel::pttChanged);

        TunerDelta d;
        d.pttA = true;
        d.pttB = false;
        t.applyChanges(d);

        CHECK(t.pttA() && !t.pttB());
        CHECK(st.count() == 1);          // one stateChanged for the whole delta
        // pttA moved false→true; pttB was already false, so it is not an edge.
        CHECK(ptt.count() == 1);
        {
            const QList<QVariant> args = ptt.takeFirst();
            CHECK(args.at(0).toBool() == true && args.at(1).toBool() == false);
        }

        // Re-applying the same values changes nothing and announces nothing.
        t.applyChanges(d);
        CHECK(st.count() == 1 && ptt.count() == 0);

        // Unkeying is an edge in its own right — the lamp has to go out.
        // std::optional<bool>(false) is engaged; a guard written as
        // `if (*d.pttA)` rather than `if (d.pttA)` would leave the lamp lit.
        TunerDelta down;
        down.pttA = false;
        t.applyChanges(down);
        CHECK(!t.pttA() && st.count() == 2 && ptt.count() == 1);
        {
            const QList<QVariant> args = ptt.takeFirst();
            CHECK(args.at(0).toBool() == false && args.at(1).toBool() == false);
        }

        // A delta carrying neither PTT field announces nothing.
        TunerDelta other;
        other.relayL = 7;
        t.applyChanges(other);
        CHECK(ptt.count() == 0);
    }

    // ---- abortTune sends autotune, and only while a tune is running ----
    // `autotune` is a toggle in the firmware: it starts a cycle when idle and
    // aborts the running one when tuning=1 (captured off the wire between the
    // 4O3A TunerGeniusDesk app and the tuner). The guard is therefore the
    // whole safety property — unguarded, an abort press on an idle tuner
    // would START a tune and key the transmitter.
    {
        TunerModel t;
        t.setHandle("0x2000");
        QSignalSpy at(&t, &TunerModel::autotuneRequested);
        QSignalSpy by(&t, &TunerModel::bypassRequested);

        t.abortTune();                       // not tuning → nothing sent
        CHECK(at.count() == 0);

        TunerDelta start; start.tuning = true;
        t.applyChanges(start);
        CHECK(t.isTuning());

        t.abortTune();
        CHECK(at.count() == 1);

        // An abort leaves the tuner in operate: bypass is never touched, so
        // there is no state to put back afterwards.
        CHECK(by.count() == 0);
        CHECK(!t.isBypass());

        // Once the tuner reports it has stopped, a further press is inert
        // rather than starting a fresh tune.
        TunerDelta done; done.tuning = false;
        t.applyChanges(done);
        t.abortTune();
        CHECK(at.count() == 1);
    }

    // ---- a tune that is never seen to end must not latch ----
    // The radio relays tuning=1, then drops mid-tune. The tuner finishes on
    // its own and sits idle, but nothing tells this client so. Left latched,
    // the flag passes abortTune()'s guard — and abortTune() sends `autotune`,
    // which on an idle tuner STARTS one and keys the transmitter through the
    // TGXL's interlock cable. Losing the handle is how the relayed side says
    // the tuner is gone, so that is where the flag has to go.
    {
        TunerModel t;
        t.setHandle("0x2000");
        QSignalSpy at(&t, &TunerModel::autotuneRequested);
        QSignalSpy tuning(&t, &TunerModel::tuningChanged);

        TunerDelta start; start.tuning = true;
        t.applyChanges(start);
        CHECK(t.isTuning());
        CHECK(tuning.count() == 1);

        t.setHandle({});                     // radio gone mid-tune
        CHECK(!t.isTuning());
        CHECK(tuning.count() == 2 && tuning.takeLast().at(0).toBool() == false);

        t.abortTune();                       // the press that used to key TX
        CHECK(at.count() == 0);
    }

    if (g_failures == 0) {
        std::printf("tuner_model_test: all checks passed\n");
        return 0;
    }
    std::printf("tuner_model_test: %d failure(s)\n", g_failures);
    return 1;
}
