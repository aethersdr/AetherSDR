// Socket-free: the Flex status ingress fence and its wire order, with real
// transports that are never opened, plus RadioModel's production binding.
#include "TestSettingsProfile.h"
#include "core/backends/flex/CommandParser.h"
#include "core/backends/flex/FlexBackend.h"
#include "core/backends/flex/FlexStatusIngress.h"
#include "core/backends/flex/RadioConnection.h"
#include "core/backends/flex/WanConnection.h"
#include "models/DaxIqModel.h"
#include "models/RadioModel.h"

#include <QCoreApplication>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest>

#include <atomic>

namespace AetherSDR {
class RadioConnectionSessionTestAccess {
public:
    static void beginSession(RadioConnection& connection)
    {
        if (connection.thread() == QThread::currentThread()) {
            connection.resetSessionState();
            return;
        }
        QMetaObject::invokeMethod(&connection, [&connection] { connection.resetSessionState(); },
                                  Qt::BlockingQueuedConnection);
    }
};
class SessionControlTestAccess {
public:
    static quint64 beginConnect(RadioModel& model) { return model.beginStatusSession(); }
    static void connectedAt(RadioModel& model, quint64 epoch) { model.m_connectedStatusEpoch = epoch; }
};
} // namespace AetherSDR

using namespace AetherSDR;

namespace {
ParsedMessage line(const char* text) { return CommandParser::parseLine(QString::fromLatin1(text)); }
QStringList objects(const QSignalSpy& spy)
{
    QStringList out;
    for (const QList<QVariant>& args : spy) { out << args.at(0).value<ParsedMessage>().object; }
    return out;
}
} // namespace

// Each DAX IQ stream builds a FIFO and loads module-pipe-source through pactl
// on DaxIqModel's worker thread. Both stay off the host for the whole process:
// pactl is answered here, never cleared, and the FIFOs go to a private runtime
// dir (#6726).
static std::atomic<int> s_pactlCalls{0};

class FlexStatusIngressTest final : public QObject {
    Q_OBJECT
    QTemporaryDir m_runtime;
private slots:
    void initTestCase()
    {
        QVERIFY(m_runtime.isValid());
        qputenv("XDG_RUNTIME_DIR", m_runtime.path().toUtf8());
        DaxIqModel::setPactlRunnerForTest([](const QStringList&) { return quint32(++s_pactlCalls); });
    }

    // A line is accepted only with the current epoch: beginSession() refuses
    // everything queued before it, and retire() closes the current epoch but
    // never a newer one.
    void epochFence()
    {
        RadioConnection connection;
        FlexStatusIngress ingress;
        ingress.bind(&connection);
        QSignalSpy accepted(&ingress, &FlexStatusIngress::lineAccepted);
        emit connection.lineReceived(line("S0|transmit rfpower=1"), ingress.epoch());
        const quint64 first = ingress.beginSession();
        emit connection.lineReceived(line("S0|transmit rfpower=2"), first);
        QCoreApplication::sendPostedEvents();
        QCOMPARE(accepted.size(), 1);   // the pre-session line was refused
        QCOMPARE(accepted.at(0).at(0).value<ParsedMessage>().kvs.value("rfpower"), QStringLiteral("2"));

        emit connection.lineReceived(line("S0|interlock state=READY"), first);
        const quint64 second = ingress.beginSession();   // a reconnect before delivery
        ingress.retire(first);                           // the late end of the first
        emit connection.lineReceived(line("S0|atu status=NONE"), second);
        QCoreApplication::sendPostedEvents();
        QCOMPARE(objects(accepted), QStringList({QStringLiteral("transmit"), QStringLiteral("atu")}));
        ingress.retire(second);
        emit connection.lineReceived(line("S0|apd enable=1"), second);
        QCoreApplication::sendPostedEvents();
        QCOMPARE(accepted.size(), 2);
    }

    // Rebinding replaces the route: lines posted through the old one are
    // refused even when their epoch matches. A live bind stamps its own epoch.
    void routesAreExclusive()
    {
        RadioConnection lan;
        WanConnection wan;
        FlexStatusIngress ingress;
        ingress.bind(&lan);
        QSignalSpy accepted(&ingress, &FlexStatusIngress::lineAccepted);
        emit lan.lineReceived(line("S0|transmit rfpower=3"), ingress.epoch());
        const quint64 live = ingress.bindLive(&wan);
        QCOMPARE(wan.lineEpoch(), live);
        emit lan.lineReceived(line("S0|transmit rfpower=4"), live);
        emit wan.lineReceived(line("S0|atu status=NONE"), wan.lineEpoch());
        QCoreApplication::sendPostedEvents();
        QCOMPARE(objects(accepted), QStringList({QStringLiteral("atu")}));
        emit wan.lineReceived(line("S0|apd enable=1"), wan.lineEpoch());
        ingress.bind(&lan);
        QCoreApplication::sendPostedEvents();
        QCOMPARE(accepted.size(), 1);
    }

    // A typed profile line is decoded once, flags included; an untyped one
    // carries only flags.
    void oneProfileDeltaPerLine()
    {
        FlexBackend backend;
        QSignalSpy profiles(&backend, &IRadioBackend::profileChanged);
        for (const char* text : {"S0|profile cw unsaved_changes_cw=1", "S0|profile global importing=1",
                                 "S0|profile tx list=Default^Rag chew^", "S0|profile importing=0 exporting=0"}) {
            profiles.clear();
            backend.statusIngress()->deliverForTest(line(text));
            QCOMPARE(profiles.size(), 1);
        }
    }

    // A DAX TX stream line is decoded once: one ownership delta, and one
    // incarnation step when its TX state is malformed.
    void oneDaxTxDeltaPerLine()
    {
        FlexBackend backend;
        FlexStatusIngress* ingress = backend.statusIngress();
        QSignalSpy dax(&backend, &IRadioBackend::daxTxStreamChanged);
        QSignalSpy audio(&backend, &IRadioBackend::audioStreamChanged);
        ingress->deliverForTest(line("S0|stream 0x84000001 type=dax_tx client_handle=0x00000071 tx=1"));
        QCOMPARE(dax.size(), 1);
        QCOMPARE(audio.size(), 1);
        const quint64 before = ingress->daxTxStream(0x84000001)->incarnation;
        ingress->deliverForTest(line("S0|stream 0x84000001 tx=2"));
        QCOMPARE(dax.size(), 2);
        QCOMPARE(ingress->daxTxStream(0x84000001)->incarnation, before + 1);
        ingress->deliverForTest(line("S0|stream 0x84000001 removed"));
        QCOMPARE(dax.size(), 3);
        QVERIFY(!ingress->daxTxStream(0x84000001));
    }

    // Whole wire lines reach the moved decoders: key/value objects, raw-line
    // objects, and the bare words folded into apd and memory objects.
    void linesReachTheirDecoders()
    {
        FlexBackend backend;
        FlexStatusIngress* ingress = backend.statusIngress();
        QSignalSpy transmit(&backend, &IRadioBackend::transmitChanged);
        QSignalSpy gps(&backend, &IRadioBackend::gpsChanged);
        QSignalSpy meters(&backend, &IRadioBackend::meterDefined);
        QSignalSpy memories(&backend, &IRadioBackend::memoryChanged);
        QSignalSpy ntp(&backend, &IRadioBackend::ntpSourceChanged);
        ingress->deliverForTest(line("S0|transmit rfpower=42"));
        ingress->deliverForTest(line("S0|atu status=TUNE_SUCCESSFUL atu_enabled=1"));
        ingress->deliverForTest(line("S0|apd equalizer_reset"));
        ingress->deliverForTest(line("S0|apd sampler tx_ant=ANT1 selected_sampler=INTERNAL"));
        QCOMPARE(transmit.size(), 4);
        QCOMPARE(transmit.at(0).at(0).value<TransmitDelta>().rfPower, std::optional<int>(42));
        ingress->deliverForTest(line("S0|gps status=Fine Lock#fix=3D#tracked=8"));
        QCOMPARE(gps.size(), 1);
        QCOMPARE(gps.at(0).at(0).value<GpsDelta>().tracked, std::optional<int>(8));
        ingress->deliverForTest(line("S0|meter 7.src=SLC#7.num=0#7.nam=LEVEL#7.low=-150.0#7.hi=20.0#7.unit=dBm#7.fps=10#"));
        QCOMPARE(meters.size(), 1);
        ingress->deliverForTest(line("S0|memory 3 removed"));
        QCOMPARE(memories.size(), 1);
        QVERIFY(memories.at(0).at(0).value<MemoryDelta>().removed);
        ingress->deliverForTest(line("S0|ntp_sources id=pool address=pool.ntp.org description=\"Two words\" enabled=1"));
        QCOMPARE(ntp.size(), 1);
    }

    // With value decoding off (SimBackend) a line reaches only lineAccepted:
    // no deltas, no DAX TX state.
    void valueDecodingCanBeOff()
    {
        FlexStatusIngress ingress;
        ingress.setDecodeValues(false);
        QSignalSpy transmit(&ingress, &FlexStatusIngress::transmitChanged);
        QSignalSpy dax(&ingress, &FlexStatusIngress::daxTxStreamChanged);
        QSignalSpy accepted(&ingress, &FlexStatusIngress::lineAccepted);
        ingress.deliverForTest(line("S0|transmit rfpower=42"));
        ingress.deliverForTest(line("S0|stream 0x84000001 type=dax_tx client_handle=0x00000071 tx=1"));
        QCOMPARE(transmit.size(), 0);
        QCOMPARE(dax.size(), 0);
        QVERIFY(!ingress.daxTxStream(0x84000001));
        QCOMPARE(accepted.size(), 2);
    }

    // Without a Flex backend, a locally banked radio still folds memory status.
    void memoryWithoutFlexUsesTheLocalBank()
    {
        TestSettingsProfile profile(QStringLiteral("flex-status-ingress-memory"));
        QVERIFY(profile.isValid());
        RadioModel model;
        QVERIFY(model.rebuildBackendForTest(QStringLiteral("sim")));
        model.handleLineForTest(QStringLiteral("S0|memory 3 name=Repeater freq=146.940000"));
        QVERIFY(model.memories().contains(3));
        QCOMPARE(model.memories().value(3).name, QStringLiteral("Repeater"));
    }

    // The DAX IQ registry is the model's: an IQ stream with no owner of its
    // own is ours, another client's or a dead orphan's is not, and a removal
    // leaves it, whatever its type.
    void iqStreamRegistry()
    {
        TestSettingsProfile profile(QStringLiteral("flex-status-ingress-iq"));
        QVERIFY(profile.isValid());
        RadioModel model;
        QVERIFY(model.rebuildBackendForTest(QStringLiteral("flex")));
        model.enableDaxIqRegistry();
        model.handleLineForTest(QStringLiteral(
            "S0|stream 0x20000001 type=dax_iq daxiq_channel=1 daxiq_rate=96000 pan=0x40000000 client_handle=0x00000000 ip=10.0.0.5"));
        QVERIFY(model.daxIqModel().stream(1).exists);
        QCOMPARE(model.daxIqModel().stream(1).streamId, quint32(0x20000001));
        QCOMPARE(model.daxIqModel().stream(1).sampleRate, 96000);
        QCOMPARE(model.daxIqModel().stream(1).panId, QStringLiteral("0x40000000"));
        model.handleLineForTest(QStringLiteral(
            "S0|stream 0x20000002 type=dax_iq daxiq_channel=2 client_handle=0x0000AB12"));
        QVERIFY(!model.daxIqModel().stream(2).exists);
        model.handleLineForTest(QStringLiteral(
            "S0|stream 0x20000003 type=dax_iq daxiq_channel=3 client_handle=0x00000000 ip=0.0.0.0"));
        QVERIFY(!model.daxIqModel().stream(3).exists);
        // An owner that does not parse counts as no owner, as before.
        model.handleLineForTest(QStringLiteral(
            "S0|stream 0x20000004 type=dax_iq daxiq_channel=4 client_handle=0xZZ ip=10.0.0.5"));
        QVERIFY(model.daxIqModel().stream(4).exists);
        model.handleLineForTest(QStringLiteral("S0|stream 0x20000001 removed"));
        QVERIFY(!model.daxIqModel().stream(1).exists);
        // FlexLib reads in_use=0 as removal (Radio.cs:3643).
        model.handleLineForTest(QStringLiteral("S0|stream 0x20000004 in_use=0"));
        QVERIFY(!model.daxIqModel().stream(4).exists);
    }

    // Until the desktop enables it, an IQ stream reaches neither DaxIqModel
    // nor PipeWire (headless, tests).
    void iqRegistryIsOffByDefault()
    {
        TestSettingsProfile profile(QStringLiteral("flex-status-ingress-iq-off"));
        QVERIFY(profile.isValid());
        RadioModel model;
        QVERIFY(model.rebuildBackendForTest(QStringLiteral("flex")));
        model.handleLineForTest(QStringLiteral("S0|stream 0x20000001 type=dax_iq daxiq_channel=1"));
        QVERIFY(!model.daxIqModel().stream(1).exists);
    }

    // A radio status line's pll_done reaches the setup dialog typed, read as
    // FlexLib does (a byte; any nonzero value is done): running, finished
    // with that line's error, and nothing for a value that is not a byte.
    void frequencyCalibrationIsTyped()
    {
        TestSettingsProfile profile(QStringLiteral("flex-status-ingress-pll"));
        QVERIFY(profile.isValid());
        RadioModel model;
        QVERIFY(model.rebuildBackendForTest(QStringLiteral("flex")));
        QSignalSpy calibration(&model, &RadioModel::frequencyCalibrationReported);
        model.handleLineForTest(QStringLiteral("S0|radio pll_done=0"));
        model.handleLineForTest(QStringLiteral("S0|radio pll_done=x"));
        model.handleLineForTest(QStringLiteral("S0|radio pll_done=256"));
        model.handleLineForTest(QStringLiteral("S0|radio pll_done=1 freq_error_ppb=-23"));
        model.handleLineForTest(QStringLiteral("S0|radio pll_done=2"));
        QCOMPARE(calibration.size(), 3);
        QCOMPARE(calibration.at(0), QList<QVariant>({false, QString()}));
        QCOMPARE(calibration.at(1), QList<QVariant>({true, QStringLiteral("-23")}));
        QCOMPARE(calibration.at(2), QList<QVariant>({true, QString()}));
    }

    // A "file update" line reaches FirmwareUploader typed: progress, the
    // radio's verdict with its reason, and out-of-range values dropped.
    void firmwareUpdateIsTyped()
    {
        TestSettingsProfile profile(QStringLiteral("flex-status-ingress-fw"));
        QVERIFY(profile.isValid());
        RadioModel model;
        QVERIFY(model.rebuildBackendForTest(QStringLiteral("flex")));
        QSignalSpy updates(&model, &RadioModel::firmwareUpdateReported);
        model.handleLineForTest(QStringLiteral("S0|file update transfer=0.42"));
        model.handleLineForTest(QStringLiteral("S0|file update failed=2 transfer=1.5"));
        model.handleLineForTest(QStringLiteral("S0|file update failed=1 reason=bad_image"));
        QCOMPARE(updates.size(), 3);
        const auto at = [&updates](int i) { return updates.at(i).at(0).value<FirmwareUpdateStatus>(); };
        QVERIFY(!at(0).failed && at(0).transfer && qFuzzyCompare(*at(0).transfer, 0.42));
        QVERIFY(!at(1).failed && !at(1).transfer);
        QVERIFY(at(2).failed && *at(2).failed);
        QCOMPARE(at(2).reason, QStringLiteral("bad_image"));
    }

    // The TX band table announces its own changes: a band line does, any
    // other status does not.
    void txBandChangesAreSignalled()
    {
        TestSettingsProfile profile(QStringLiteral("flex-status-ingress-band"));
        QVERIFY(profile.isValid());
        RadioModel model;
        QVERIFY(model.rebuildBackendForTest(QStringLiteral("flex")));
        QSignalSpy bands(&model, &RadioModel::txBandSettingsChanged);
        model.handleLineForTest(QStringLiteral("S0|transmit band 9 band_name=20 rfpower=80"));
        QCOMPARE(bands.size(), 1);
        QCOMPARE(model.txBandSettings().value(9).rfPower, 80);
        model.handleLineForTest(QStringLiteral("S0|interlock band 9 tx1_enabled=1"));
        QCOMPARE(bands.size(), 2);
        model.handleLineForTest(QStringLiteral("S0|transmit rfpower=50"));
        model.handleLineForTest(QStringLiteral("S0|transmit band x rfpower=1"));
        QCOMPARE(bands.size(), 2);
    }

    // Stream status reaches the model's stream rules typed through the
    // Flex relay: our remote audio is adopted and its removal clears it.
    void streamRulesRunFromTheIngress()
    {
        TestSettingsProfile profile(QStringLiteral("flex-status-ingress-stream"));
        QVERIFY(profile.isValid());
        RadioModel model;
        QVERIFY(model.rebuildBackendForTest(QStringLiteral("flex")));
        const auto remoteAudio = [&model] {
            return model.troubleshootingSnapshot()[QStringLiteral("radio")].toObject()
                [QStringLiteral("remote_audio_rx")].toObject();
        };
        model.handleLineForTest(QStringLiteral(
            "S0|stream 0x04000010 type=remote_audio_rx client_handle=0x00000000 compression=none"));
        QVERIFY(remoteAudio()[QStringLiteral("stream_id_known")].toBool());
        model.handleLineForTest(QStringLiteral("S0|stream 0x04000010 removed"));
        QVERIFY(!remoteAudio()[QStringLiteral("stream_id_known")].toBool());
    }

    // The H line reaches the model before connected(): both take one queued
    // hop from the connection's thread, in the order the wire produced them.
    void handleLineArrivesBeforeConnected()
    {
        FlexBackend backend;
        RadioConnection* connection = backend.connection();
        RadioConnectionSessionTestAccess::beginSession(*connection);
        QStringList order;
        connect(backend.statusIngress(), &FlexStatusIngress::lineAccepted, this, [&order](const ParsedMessage& m) {
            order << (m.type == MessageType::Handle ? QStringLiteral("H") : m.object);
        });
        connect(&backend, &IRadioBackend::connected, this, [&order] { order << QStringLiteral("connected"); });
        QMetaObject::invokeMethod(connection, [connection] {
            emit connection->lineReceived(line("H00000071"), connection->lineEpoch());
            emit connection->connected();
            emit connection->lineReceived(line("S0|radio callsign=KK7GWY"), connection->lineEpoch());
        }, Qt::BlockingQueuedConnection);
        QTRY_COMPARE(order.size(), 3);
        QCOMPARE(order, QStringList({QStringLiteral("H"), QStringLiteral("connected"), QStringLiteral("radio")}));
    }

    // On SmartLink the lifecycle takes the same queued hop as the lines, so a
    // line the radio sent before closing lands before the session ends, and
    // nothing from the closed session lands after it.
    void wanLifecycleKeepsWireOrder()
    {
        TestSettingsProfile profile(QStringLiteral("flex-status-ingress"));
        QVERIFY(profile.isValid());
        WanConnection wan;
        RadioModel model;
        QVERIFY(model.rebuildBackendForTest(QStringLiteral("flex")));
        model.connectViaWan(&wan, QStringLiteral("192.0.2.1"), 4992);
        const quint64 epoch = wan.lineEpoch();
        emit wan.lineReceived(line("S0|profile global current=Before close"), epoch);
        emit wan.disconnected();
        emit wan.lineReceived(line("S0|profile global current=After close"), epoch);
        QCoreApplication::sendPostedEvents();
        QCOMPARE(model.activeGlobalProfile(), QStringLiteral("Before close"));
    }

    // A disconnect that lands after a new connect has begun ends the old
    // session only: the new connect's lines still reach the model.
    void lateDisconnectKeepsTheNewSession()
    {
        TestSettingsProfile profile(QStringLiteral("flex-status-ingress-late"));
        QVERIFY(profile.isValid());
        RadioModel model;
        QVERIFY(model.rebuildBackendForTest(QStringLiteral("flex")));
        RadioConnection* lan = model.connection();
        SessionControlTestAccess::connectedAt(model, SessionControlTestAccess::beginConnect(model));
        const quint64 next = SessionControlTestAccess::beginConnect(model);   // a quick reconnect
        QVERIFY(QMetaObject::invokeMethod(&model, "onDisconnected", Qt::DirectConnection));
        emit lan->lineReceived(line("S0|profile global current=New session"), next);
        QCoreApplication::sendPostedEvents();
        QCOMPARE(model.activeGlobalProfile(), QStringLiteral("New session"));
    }

    // MainWindow reuses one WanConnection; a disconnect queued by the previous
    // SmartLink session cannot end the session bound after it.
    void reusedWanRefusesEarlierLifecycle()
    {
        TestSettingsProfile profile(QStringLiteral("flex-status-ingress-reuse"));
        QVERIFY(profile.isValid());
        WanConnection wan;
        RadioModel model;
        QVERIFY(model.rebuildBackendForTest(QStringLiteral("flex")));
        model.connectViaWan(&wan, QStringLiteral("192.0.2.1"), 4992);
        emit wan.disconnected();                                  // queued, not yet run
        model.connectViaWan(&wan, QStringLiteral("192.0.2.1"), 4992);   // the next session
        QCoreApplication::sendPostedEvents();
        emit wan.lineReceived(line("S0|profile global current=Second session"), wan.lineEpoch());
        QCoreApplication::sendPostedEvents();
        QCOMPARE(model.activeGlobalProfile(), QStringLiteral("Second session"));
    }

    // Leaving SmartLink for the LAN takes the WAN route with it.
    void leavingWanDropsItsRoute()
    {
        TestSettingsProfile profile(QStringLiteral("flex-status-ingress-leave"));
        QVERIFY(profile.isValid());
        WanConnection wan;
        RadioModel model;
        QVERIFY(model.rebuildBackendForTest(QStringLiteral("flex")));
        model.connectViaWan(&wan, QStringLiteral("192.0.2.1"), 4992);
        QVERIFY(QMetaObject::invokeMethod(&model, "onDisconnected", Qt::DirectConnection));
        emit wan.lineReceived(line("S0|profile global current=Stray"), wan.lineEpoch());
        QCoreApplication::sendPostedEvents();
        QVERIFY(model.activeGlobalProfile() != QLatin1String("Stray"));
    }
};

QTEST_GUILESS_MAIN(FlexStatusIngressTest)
#include "flex_status_ingress_test.moc"
