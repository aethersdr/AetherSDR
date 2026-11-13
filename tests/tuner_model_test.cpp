// TunerModel unit test — the TGXL state machine (#4092). Exercises
// TunerModel::applyChanges(TunerDelta): present-only application, change-gating,
// the tuning/antenna edge signals, and the single stateChanged. The wire→delta
// translation is covered separately by aetherd_tuner_decode_test.

#include "models/TunerModel.h"
#include "core/TgxlConnection.h"

#include <QCoreApplication>
#include <QLoggingCategory>
#include <QSignalSpy>
#include <QStringList>
#include <cstdio>

namespace AetherSDR {
struct PeripheralConnectionTestAccess {
    static void makeReady(TgxlConnection& connection)
    {
        connection.m_connected = true; // inert transport: no connection or peer
    }

    static void captureCommands(TgxlConnection& connection, QStringList& lines)
    {
        connection.m_commandWriter = [&lines](const QByteArray& line) {
            lines.append(QString::fromUtf8(line));
        };
    }
};
}

using namespace AetherSDR;

static int g_failures = 0;
static QStringList g_warnings;

static void captureWarning(QtMsgType type, const QMessageLogContext& context, const QString& message)
{
    if (type == QtWarningMsg && QString::fromLatin1(context.category) == QStringLiteral("aether.tuner")) {
        g_warnings.append(message);
    }
}

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
        QSignalSpy direct(&t, &TunerModel::directAutotuneStarted);

        // No handle yet (no direct conn) → every relay verb is a no-op.
        t.setOperate(true); t.setBypass(true); t.autoTune();
        CHECK(op.count() == 0 && by.count() == 0 && at.count() == 0);
        CHECK(direct.count() == 0);   // neither path fires without a handle or link

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
        CHECK(direct.count() == 0);   // relayed start never reports a direct tune
    }

    // A ready direct transport emits the start edge once; abort never re-arms.
    {
        TgxlConnection direct;
        PeripheralConnectionTestAccess::makeReady(direct);
        TunerModel t;
        t.setDirectConnection(&direct);
        QSignalSpy started(&t, &TunerModel::directAutotuneStarted);
        QSignalSpy relayed(&t, &TunerModel::autotuneRequested);
        t.autoTune();
        CHECK(started.count() == 1 && relayed.count() == 0);
        TunerDelta tuning;
        tuning.tuning = true;
        t.applyChanges(tuning);
        t.abortTune();
        CHECK(started.count() == 1 && relayed.count() == 0);
    }

    // A present tuner with neither a ready direct link nor a handle refuses.
    {
        TunerModel t;
        TgxlConnection direct;
        t.setDirectConnection(&direct);
        QSignalSpy refused(&t, &TunerModel::relayedCommandRefused);
        QSignalSpy op(&t, &TunerModel::operateRequested);
        QSignalSpy by(&t, &TunerModel::bypassRequested);
        QSignalSpy st(&t, &TunerModel::stateChanged);

        // No tuner at all: still the silent no-op, nothing to refuse.
        t.setOperate(true); t.setBypass(true);
        CHECK(refused.count() == 0 && op.count() == 0 && by.count() == 0);

        CHECK(QMetaObject::invokeMethod(&direct, "connected", Qt::DirectConnection));
        CHECK(t.isPresent() && t.handle().isEmpty());

        t.setOperate(true);
        CHECK(refused.count() == 1
              && refused.takeFirst().at(0).toString() == QLatin1String("operate"));
        t.setBypass(true);
        CHECK(refused.count() == 1
              && refused.takeFirst().at(0).toString() == QLatin1String("bypass"));
        CHECK(op.count() == 0 && by.count() == 0);          // nothing relayed
        CHECK(!t.isOperate() && !t.isBypass() && st.count() == 0);  // no lying state

        // Once a Flex relays it (handle present) the verbs go through and
        // nothing is refused.
        t.setHandle("0x3000");
        t.setOperate(true);
        CHECK(op.count() == 1 && refused.count() == 0);
    }

    // One press needing both verbs is one refusal when no transport is ready.
    {
        TunerModel t;
        TgxlConnection direct;
        t.setDirectConnection(&direct);
        CHECK(QMetaObject::invokeMethod(&direct, "connected", Qt::DirectConnection));
        QSignalSpy refused(&t, &TunerModel::relayedCommandRefused);
        QStringList wire;
        QObject::connect(&t, &TunerModel::operateRequested, &t,
                         [&wire](bool on) { wire << QStringLiteral("operate=%1").arg(on ? 1 : 0); });
        QObject::connect(&t, &TunerModel::bypassRequested, &t,
                         [&wire](bool on) { wire << QStringLiteral("bypass=%1").arg(on ? 1 : 0); });

        t.setOperateAndBypass(false, false, /*operateFirst=*/true);
        CHECK(refused.count() == 1
              && refused.takeFirst().at(0).toString() == QLatin1String("operate"));
        t.setOperateAndBypass(true, false, /*operateFirst=*/false);
        CHECK(refused.count() == 1
              && refused.takeFirst().at(0).toString() == QLatin1String("bypass"));
        CHECK(wire.isEmpty() && !t.isOperate() && !t.isBypass());

        t.setHandle("0x3000");
        t.setOperateAndBypass(true, true, /*operateFirst=*/true);
        CHECK(wire == QStringList({QStringLiteral("operate=1"), QStringLiteral("bypass=1")}));
        wire.clear();
        t.setOperateAndBypass(false, false, /*operateFirst=*/false);
        CHECK(wire == QStringList({QStringLiteral("bypass=0"), QStringLiteral("operate=0")}));
        CHECK(refused.count() == 0);
    }

    // Direct commands work without a radio handle; only device status changes
    // their displayed state. Vendor TGXL API, pages 3-4.
    {
        QStringList wire;
        TgxlConnection direct;
        PeripheralConnectionTestAccess::makeReady(direct);
        PeripheralConnectionTestAccess::captureCommands(direct, wire);
        TunerModel t;
        t.setDirectConnection(&direct);
        CHECK(QMetaObject::invokeMethod(&direct, "connected", Qt::DirectConnection));
        QSignalSpy refused(&t, &TunerModel::relayedCommandRefused);
        QSignalSpy op(&t, &TunerModel::operateRequested);
        QSignalSpy by(&t, &TunerModel::bypassRequested);
        QSignalSpy st(&t, &TunerModel::stateChanged);

        t.setOperateAndBypass(true, false, /*operateFirst=*/false);
        CHECK(wire == QStringList({QStringLiteral("C1|bypass set=0\n"),
                                   QStringLiteral("C2|operate set=1\n")}));
        CHECK(refused.count() == 0 && op.count() == 0 && by.count() == 0);
        CHECK(!t.isOperate() && !t.isBypass() && st.count() == 0);
        CHECK(QMetaObject::invokeMethod(&direct, "processLine", Qt::DirectConnection,
              Q_ARG(QString, QStringLiteral("S0|state state=1 bypass=0"))));
        CHECK(t.isOperate() && !t.isBypass());

        // Direct-only mode changes await device readback.
        wire.clear();
        st.clear();
        t.setBypass(true);
        CHECK(wire == QStringList({QStringLiteral("C3|bypass set=1\n")}));
        CHECK(t.isOperate() && !t.isBypass() && st.count() == 0);
        CHECK(QMetaObject::invokeMethod(&direct, "processLine", Qt::DirectConnection,
              Q_ARG(QString, QStringLiteral("S1|status state=1 bypass=1"))));
        CHECK(t.isOperate() && t.isBypass());

        wire.clear();
        t.setOperateAndBypass(false, false, /*operateFirst=*/true);
        CHECK(wire == QStringList({QStringLiteral("C4|operate set=0\n"),
                                   QStringLiteral("C5|bypass set=0\n")}));
        CHECK(t.isOperate() && t.isBypass());
        CHECK(QMetaObject::invokeMethod(&direct, "processLine", Qt::DirectConnection,
              Q_ARG(QString, QStringLiteral("S0|state state=0 bypass=0"))));
        CHECK(!t.isOperate() && !t.isBypass());
        CHECK(op.count() == 0 && by.count() == 0 && refused.count() == 0);

        // A radio handle keeps mode commands on the proven relay path, even
        // while the direct link is ready for status and other commands.
        t.setHandle(QStringLiteral("0x2000"));
        wire.clear();
        st.clear();
        t.setOperateAndBypass(true, true, /*operateFirst=*/true);
        CHECK(wire.isEmpty() && op.count() == 1 && by.count() == 1);
        CHECK(t.isOperate() && t.isBypass() && st.count() == 2);

        CHECK(QMetaObject::invokeMethod(&direct, "onDisconnected", Qt::DirectConnection));
        wire.clear();
        t.setOperateAndBypass(false, false, /*operateFirst=*/false);
        CHECK(wire.isEmpty() && op.count() == 2 && by.count() == 2);
        CHECK(!t.isOperate() && !t.isBypass()); // existing relayed optimistic state
    }

    // Unknown tuning must disarm abort: autotune on an idle device starts TX.
    {
        QStringList wire;
        TgxlConnection direct;
        PeripheralConnectionTestAccess::makeReady(direct);
        PeripheralConnectionTestAccess::captureCommands(direct, wire);
        TunerModel t;
        t.setDirectConnection(&direct);
        for (const QString& prefix : {QStringLiteral("S0|state "),
                                      QStringLiteral("S1|status "),
                                      QStringLiteral("R2|0|")}) {
            for (const QString& value : {QString(), QStringLiteral("bad"), QStringLiteral("-1")}) {
                CHECK(QMetaObject::invokeMethod(&direct, "processLine", Qt::DirectConnection,
                      Q_ARG(QString, QStringLiteral("S0|state tuning=1"))));
                CHECK(t.isTuning());
                CHECK(QMetaObject::invokeMethod(&direct, "processLine", Qt::DirectConnection,
                      Q_ARG(QString, QStringLiteral("S1|status relayC1=42"))));
                CHECK(t.isTuning()); // absent is different from present but invalid
                wire.clear();
                CHECK(QMetaObject::invokeMethod(&direct, "processLine", Qt::DirectConnection,
                      Q_ARG(QString, prefix + QStringLiteral("tuning=") + value)));
                CHECK(!t.isTuning());
                t.abortTune();
                CHECK(wire.isEmpty());
            }
        }
    }

    // Refused replies must warn, not publish their bodies as successful data.
    {
        TgxlConnection direct;
        QSignalSpy status(&direct, &TgxlConnection::statusUpdated);
        QSignalSpy names(&direct, &TgxlConnection::antennaNamesChanged);
        QLoggingCategory::setFilterRules(QStringLiteral("aether.tuner.warning=true"));
        const QtMessageHandler previous = qInstallMessageHandler(captureWarning);
        for (const QString& reply : {QStringLiteral("R7|1|unsupported command"),
                                     QStringLiteral("R8|50000001|state=0 bypass=1"),
                                     QStringLiteral("R9|2|"),
                                     QStringLiteral("R10|3|antenna name1=Rejected")}) {
            g_warnings.clear();
            CHECK(QMetaObject::invokeMethod(&direct, "processLine", Qt::DirectConnection,
                  Q_ARG(QString, reply)));
            CHECK(g_warnings.size() == 1);
            if (!g_warnings.isEmpty()) {
                CHECK(g_warnings.first().contains(QStringLiteral("command refused")));
                CHECK(g_warnings.first().contains(reply));
            }
            CHECK(status.count() == 0 && names.count() == 0);
        }
        g_warnings.clear();
        CHECK(QMetaObject::invokeMethod(&direct, "processLine", Qt::DirectConnection,
              Q_ARG(QString, QStringLiteral("R11|0|state=1 bypass=0"))));
        CHECK(QMetaObject::invokeMethod(&direct, "processLine", Qt::DirectConnection,
              Q_ARG(QString, QStringLiteral("R12|0|"))));
        CHECK(g_warnings.isEmpty() && status.count() == 1);
        if (!status.isEmpty()) {
            const QMap<QString, QString> fields = status.first().first().value<QMap<QString, QString>>();
            CHECK(fields.value(QStringLiteral("state")) == QStringLiteral("1"));
        }
        qInstallMessageHandler(previous);
    }

    // Preserve the zero-based relay numbering captured in #469.
    {
        QStringList wire;
        TgxlConnection direct;
        PeripheralConnectionTestAccess::makeReady(direct);
        PeripheralConnectionTestAccess::captureCommands(direct, wire);
        TunerModel t;
        t.setDirectConnection(&direct);
        QSignalSpy st(&t, &TunerModel::stateChanged);

        t.adjustRelay(0, 1);
        t.adjustRelay(1, 1);
        t.adjustRelay(2, 1);
        t.adjustRelay(0, -1);
        CHECK(wire == QStringList({QStringLiteral("C1|tune relay=0 move=1\n"),
                                   QStringLiteral("C2|tune relay=1 move=1\n"),
                                   QStringLiteral("C3|tune relay=2 move=1\n"),
                                   QStringLiteral("C4|tune relay=0 move=-1\n")}));
        CHECK(st.count() == 0);
    }

    // Polls and unsolicited pushes share the same present-only, change-gated
    // relay/mode application. The first frame is captured from TGXL 1.2.17.
    {
        TgxlConnection direct;
        TunerModel t;
        t.setDirectConnection(&direct);
        QSignalSpy st(&t, &TunerModel::stateChanged);
        QSignalSpy tuning(&t, &TunerModel::tuningChanged);
        QSignalSpy ant(&t, &TunerModel::antennaAChanged);
        const QString status = QStringLiteral(
            "S230|status fwd=21.42 peak=21.42 max=62.43 swr=-60.0000 "
            "pttA=0 bandA=6 modeA=1 flexA=FLEX-8600 freqA=14161.500 "
            "bypassA=0 bypassRxA=0 antA=0 "
            "pttB=0 bandB=0 modeB=0 flexB=FLEX-8600 freqB=0.000 "
            "bypassB=0 bypassRxB=0 antB=0 "
            "state=1 active=1 tuning=0 bypass=0 ag=0 relayC1=44 relayL=12 relayC2=8");
        CHECK(QMetaObject::invokeMethod(&direct, "processLine", Qt::DirectConnection,
                                       Q_ARG(QString, status)));
        CHECK(t.relayC1() == 44 && t.relayL() == 12 && t.relayC2() == 8);
        CHECK(t.isOperate() && !t.isBypass() && !t.isTuning());
        CHECK(t.antennaA() == 0 && st.count() == 1 && ant.count() == 1);
        CHECK(QMetaObject::invokeMethod(&direct, "processLine", Qt::DirectConnection,
                                       Q_ARG(QString, status)));
        CHECK(st.count() == 1 && ant.count() == 1);

        CHECK(QMetaObject::invokeMethod(&direct, "processLine", Qt::DirectConnection,
              Q_ARG(QString, QStringLiteral(
                  "S0|state state=0 bypass=1 tuning=1 relayC1=255 relayL=0 relayC2=32"))));
        CHECK(!t.isOperate() && t.isBypass() && t.isTuning());
        CHECK(t.relayC1() == 255 && t.relayL() == 0 && t.relayC2() == 32);
        CHECK(st.count() == 2 && tuning.count() == 1 && t.antennaA() == 0);

        // Alternate R-frame replies and partial updates must also work.
        CHECK(QMetaObject::invokeMethod(&direct, "processLine", Qt::DirectConnection,
              Q_ARG(QString, QStringLiteral("R231|0|relayC1=0 bypass=0 tuning=0"))));
        CHECK(t.relayC1() == 0 && t.relayL() == 0 && t.relayC2() == 32);
        CHECK(!t.isOperate() && !t.isBypass() && !t.isTuning());
        CHECK(st.count() == 3 && tuning.count() == 2);

        CHECK(QMetaObject::invokeMethod(&direct, "processLine", Qt::DirectConnection,
              Q_ARG(QString, QStringLiteral("S232|status fwd=21.42 swr=-60.0000"))));
        CHECK(st.count() == 3 && t.relayC2() == 32 && !t.isOperate());

        CHECK(QMetaObject::invokeMethod(&direct, "processLine", Qt::DirectConnection,
              Q_ARG(QString, QStringLiteral(
                  "S233|status state=2 bypass=bad tuning=-1 "
                  "relayC1=-1 relayL=256 relayC2=bad antA=3"))));
        CHECK(st.count() == 3 && tuning.count() == 2 && ant.count() == 1);
        CHECK(!t.isOperate() && !t.isBypass() && !t.isTuning());
        CHECK(t.relayC1() == 0 && t.relayL() == 0 && t.relayC2() == 32);
        CHECK(t.antennaA() == 0);
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
