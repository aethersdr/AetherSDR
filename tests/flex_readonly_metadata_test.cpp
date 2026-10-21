// Published FlexLib Slice.ChainMode and Radio.UpdateInfo metadata through the
// production decoder, typed seam, model and bridge. No sockets are opened.
#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/AutomationServer.h"
#include "core/backends/RadioDelta.h"
#include "core/backends/SliceDelta.h"
#include "core/backends/flex/FlexBackend.h"
#include "core/backends/flex/RadioConnection.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QCoreApplication>
#include <QByteArray>
#include <QEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QMap>
#include <QMetaObject>
#include <QMetaProperty>
#include <QObject>
#include <QSignalSpy>
#include <QString>
#include <QStringList>
#include <QTest>
#include <QVariant>

#include <cstdio>
#include <functional>
#include <initializer_list>

namespace AetherSDR {
class AutomationServerTestAccess {
public:
    static QJsonObject request(AutomationServer& server, const QJsonObject& request)
    {
        return server.handleLine(QJsonDocument(request).toJson(QJsonDocument::Compact), nullptr);
    }
};

class RadioConnectionSessionTestAccess {
public:
    static void capture(RadioConnection& connection, QStringList* commands,
                        QMap<quint32, QString>* requests = nullptr)
    {
        QMetaObject::invokeMethod(&connection, [&connection, commands, requests] {
            connection.m_state.store(commands ? ConnectionState::Connected
                                              : ConnectionState::Disconnected);
            connection.m_commandSinkForTest = commands
                ? std::function<void(quint32, const QString&)>(
                      [commands, requests](quint32 seq, const QString& command) {
                          commands->append(command);
                          if (requests) {
                              requests->insert(seq, command);
                          }
                      })
                : std::function<void(quint32, const QString&)>{};
        }, Qt::BlockingQueuedConnection);
    }
    static void flush(RadioConnection& connection)
    {
        QMetaObject::invokeMethod(&connection, [] {}, Qt::BlockingQueuedConnection);
    }
    static void reply(RadioConnection& connection, quint32 seq, int code, const QString& body)
    {
        QMetaObject::invokeMethod(&connection, [&connection, seq, code, body] {
            connection.processLine(QStringLiteral("R%1|%2|%3")
                                       .arg(seq).arg(code, 0, 16).arg(body));
        }, Qt::BlockingQueuedConnection);
    }
};

class RadioNicknameTestAccess {
public:
    static void requestInfo(RadioModel& radio) { radio.requestRadioInfo(); }
    static RadioConnection::ResponseCallback callback(RadioModel& radio, quint32 seq)
    {
        return radio.m_pendingCallbacks.value(seq);
    }
    static quint64 session(const RadioModel& radio) { return radio.m_sessionGeneration; }
    static quint64 modelGeneration(const RadioModel& radio) { return radio.m_sessionModelGeneration; }
    static quint64 backendGeneration(const RadioModel& radio)
    {
        return radio.m_backendReceiverGeneration;
    }
    static void disconnect(RadioModel& radio)
    {
        radio.m_intentionalDisconnect = true;
        radio.onDisconnected();
        radio.m_reconnectTimer.stop();
    }
};
} // namespace AetherSDR

using namespace AetherSDR;

namespace {
QMap<QString, QString> sliceStatus()
{
    return {{QStringLiteral("in_use"), QStringLiteral("1")},
            {QStringLiteral("RF_frequency"), QStringLiteral("14.200000")},
            {QStringLiteral("mode"), QStringLiteral("USB")},
            {QStringLiteral("active"), QStringLiteral("1")}};
}

struct Fixture {
    // Captured storage outlives the transport and its worker thread.
    QStringList wireCommands;
    QMap<quint32, QString> wireRequests;
    QStringList sliceCommands;
    RadioModel radio;
    AutomationServer bridge;
    FlexBackend* backend = nullptr;

    bool init()
    {
        bridge.setRadioModel(&radio);
        if (!radio.rebuildBackendForTest(QStringLiteral("flex"))) {
            return false;
        }
        backend = dynamic_cast<FlexBackend*>(radio.backend());
        if (!backend) {
            return false;
        }
        RadioConnectionSessionTestAccess::capture(*backend->connection(), &wireCommands, &wireRequests);
        backend->setSliceCommandSink([this](const QString& command) {
            sliceCommands.append(command);
        });
        radio.handleSliceStatusForTest(0, sliceStatus());
        clearCommands();
        return radio.slice(0) != nullptr;
    }
    void clearCommands()
    {
        RadioConnectionSessionTestAccess::flush(*backend->connection());
        wireCommands.clear();
        wireRequests.clear();
        sliceCommands.clear();
    }
    bool noCommands()
    {
        RadioConnectionSessionTestAccess::flush(*backend->connection());
        return wireCommands.isEmpty() && sliceCommands.isEmpty();
    }
    quint32 requestInfo()
    {
        clearCommands();
        RadioNicknameTestAccess::requestInfo(radio);
        RadioConnectionSessionTestAccess::flush(*backend->connection());
        return wireRequests.isEmpty() ? 0 : wireRequests.lastKey();
    }
    void reply(quint32 seq, int code, const QString& body)
    {
        RadioConnectionSessionTestAccess::reply(*backend->connection(), seq, code, body);
        QCoreApplication::sendPostedEvents(&radio, QEvent::MetaCall);
    }
    void detach()
    {
        if (backend) {
            backend->setSliceCommandSink({});
            RadioConnectionSessionTestAccess::capture(*backend->connection(), nullptr);
            backend = nullptr;
        }
    }
    ~Fixture() { detach(); }

    QJsonObject get(const QString& model, const QString& property = QString())
    {
        QJsonObject request{{QStringLiteral("cmd"), QStringLiteral("get")},
                            {QStringLiteral("model"), model}};
        if (model == QLatin1String("slice")) {
            request.insert(QStringLiteral("selector"), QStringLiteral("0"));
        }
        if (!property.isEmpty()) {
            request.insert(QStringLiteral("property"), property);
        }
        return AutomationServerTestAccess::request(bridge, request);
    }
};
} // namespace

class FlexReadonlyMetadataTest : public QObject {
    Q_OBJECT
private slots:
    void chainReadbackIsIndependentOfDemodulationMode()
    {
        Fixture f;
        QVERIFY(f.init());
        SliceModel* slice = f.radio.slice(0);
        QVERIFY(slice->chainMode().isEmpty());
        const QMetaObject* meta = slice->metaObject();
        const int propertyIndex = meta->indexOfProperty("chainMode");
        QVERIFY(propertyIndex >= 0);
        const QMetaProperty property = meta->property(propertyIndex);
        QVERIFY(property.isReadable());
        QVERIFY(!property.isWritable());
        QVERIFY(property.hasNotifySignal());
        QSignalSpy changed(slice, &SliceModel::chainModeChanged);
        QSignalSpy modes(slice, &SliceModel::modeChanged);
        for (const QString& mode : {QStringLiteral("FDVU"), QStringLiteral("FDVL")}) {
            f.backend->decodeSliceStatus(0, {{QStringLiteral("mode"), mode},
                {QStringLiteral("chain_mode"), QStringLiteral("DIGU")}});
            QCOMPARE(slice->mode(), mode);
            QCOMPARE(slice->chainMode(), QStringLiteral("DIGU"));
        }
        QCOMPARE(changed.count(), 1);
        QCOMPARE(changed.first().first().toString(), QStringLiteral("DIGU"));
        QCOMPARE(modes.count(), 2);
        f.backend->decodeSliceStatus(0, {{QStringLiteral("chain_mode"), QStringLiteral("Future_Chain-v9")}});
        QCOMPARE(slice->chainMode(), QStringLiteral("Future_Chain-v9"));
        QCOMPARE(slice->mode(), QStringLiteral("FDVL"));
        QCOMPARE(modes.count(), 2);
        QVERIFY(f.noCommands());
    }

    void chainValidation_data()
    {
        QTest::addColumn<QString>("input");
        QTest::addColumn<QString>("expected");
        QTest::newRow("empty") << QString() << QString();
        QTest::newRow("boundary") << QString(32, QLatin1Char('X')) << QString(32, QLatin1Char('X'));
        QTest::newRow("oversize") << QString(33, QLatin1Char('X')) << QString();
        QTest::newRow("huge") << QString(65536, QLatin1Char('X')) << QString();
        QTest::newRow("space") << QStringLiteral("USB DIGU") << QString();
        QTest::newRow("leading-space") << QStringLiteral(" DIGU") << QString();
        QTest::newRow("tab") << QStringLiteral("DI\tGU") << QString();
        QTest::newRow("newline") << QStringLiteral("DIGU\n") << QString();
        QTest::newRow("nul") << (QStringLiteral("DI") + QChar(0) + QStringLiteral("GU")) << QString();
        QTest::newRow("delete") << (QStringLiteral("DIGU") + QChar(0x7f)) << QString();
        QTest::newRow("non-ascii") << (QStringLiteral("DIGU") + QChar(0xe9)) << QString();
        QTest::newRow("future-token") << QStringLiteral("~Future_Chain-2.0!") << QStringLiteral("~Future_Chain-2.0!");
    }

    void chainValidation()
    {
        QFETCH(QString, input);
        QFETCH(QString, expected);
        Fixture f;
        QVERIFY(f.init());
        f.backend->decodeSliceStatus(0, {{QStringLiteral("chain_mode"), QStringLiteral("USB")}});
        QSignalSpy typed(f.backend, &IRadioBackend::sliceChanged);
        f.backend->decodeSliceStatus(0, {{QStringLiteral("chain_mode"), input}});
        QCOMPARE(typed.count(), 1);
        const SliceDelta delta = typed.first().at(1).value<SliceDelta>();
        QVERIFY(delta.chainMode.has_value());
        QCOMPARE(*delta.chainMode, expected);
        QCOMPARE(f.radio.slice(0)->chainMode(), expected);
        QVERIFY(f.noCommands());
    }

    void partialSliceStatusPreservesAndExplicitUnknownClears()
    {
        Fixture f;
        QVERIFY(f.init());
        f.backend->decodeSliceStatus(0, {{QStringLiteral("chain_mode"), QStringLiteral("USB")}});
        QSignalSpy changed(f.radio.slice(0), &SliceModel::chainModeChanged);
        QSignalSpy typed(f.backend, &IRadioBackend::sliceChanged);
        f.backend->decodeSliceStatus(0, {{QStringLiteral("mode"), QStringLiteral("DIGL")}});
        QVERIFY(!typed.first().at(1).value<SliceDelta>().chainMode.has_value());
        QCOMPARE(f.radio.slice(0)->chainMode(), QStringLiteral("USB"));
        QCOMPARE(changed.count(), 0);
        f.backend->decodeSliceStatus(0, {{QStringLiteral("chain_mode"), QStringLiteral("USB")}});
        QCOMPARE(changed.count(), 0);
        f.backend->decodeSliceStatus(0, {{QStringLiteral("chain_mode"), QString()}});
        QVERIFY(f.radio.slice(0)->chainMode().isEmpty());
        QCOMPARE(changed.count(), 1);
        f.backend->decodeSliceStatus(0, {});
        QCOMPARE(changed.count(), 1);
        QVERIFY(f.noCommands());
    }

    void infoRegionValidation_data()
    {
        QTest::addColumn<QString>("body");
        QTest::addColumn<int>("expected");
        QTest::newRow("region-1") << QStringLiteral("model=FLEX-8600,iaru_region=1,region=USA") << 1;
        QTest::newRow("region-2") << QStringLiteral("iaru_region=2") << 2;
        QTest::newRow("region-3") << QStringLiteral("iaru_region=3") << 3;
        QTest::newRow("quoted") << QStringLiteral("callsign=TEST, iaru_region=\"2\", region=USA") << 2;
        QTest::newRow("escaped-quotes") << QStringLiteral("iaru_region=\\\"2\\\"") << 2;
        QTest::newRow("absent") << QStringLiteral("region=USA,model=FLEX-8600") << 0;
        QTest::newRow("empty-info") << QString() << 0;
        QTest::newRow("empty-value") << QStringLiteral("iaru_region=") << 0;
        QTest::newRow("duplicate-invalid") << QStringLiteral("iaru_region=2,iaru_region=999999999999999999999999") << 0;
        QTest::newRow("zero") << QStringLiteral("iaru_region=0") << 0;
        QTest::newRow("negative") << QStringLiteral("iaru_region=-1") << 0;
        QTest::newRow("out-of-range") << QStringLiteral("iaru_region=4") << 0;
        QTest::newRow("decimal") << QStringLiteral("iaru_region=2.0") << 0;
        QTest::newRow("suffix") << QStringLiteral("iaru_region=2oops") << 0;
        QTest::newRow("overflow") << QStringLiteral("iaru_region=999999999999999999999999") << 0;
        QTest::newRow("control") << QStringLiteral("iaru_region=2\n3") << 0;
        QTest::newRow("missing-equals") << QStringLiteral("iaru_region,region=USA") << 0;
        QTest::newRow("lookalike-key") << QStringLiteral("some_iaru_region=1,region=2") << 0;
    }

    void infoRegionValidation()
    {
        QFETCH(QString, body);
        QFETCH(int, expected);
        Fixture f;
        QVERIFY(f.init());
        f.backend->decodeRadioInfoMetadata(QStringLiteral("iaru_region=3"));
        QSignalSpy typed(f.backend, &IRadioBackend::radioChanged);
        f.backend->decodeRadioInfoMetadata(body);
        QCOMPARE(typed.count(), 1);
        const RadioDelta delta = typed.first().first().value<RadioDelta>();
        QVERIFY(delta.iaruRegion.has_value());
        QCOMPARE(*delta.iaruRegion, expected);
        QCOMPARE(f.radio.iaruRegion().value_or(0), expected);
        QCOMPARE(f.radio.iaruRegion().has_value(), expected != 0);
        QVERIFY(f.noCommands());
    }

    void ordinaryRadioDeltaPreservesFullInfoAbsenceClears()
    {
        Fixture f;
        QVERIFY(f.init());
        f.backend->decodeRadioInfoMetadata(QStringLiteral("region=USA,iaru_region=2"));
        f.backend->decodeRadioStatus({{QStringLiteral("region"), QStringLiteral("USA")}});
        QCOMPARE(f.radio.iaruRegion().value_or(0), 2);
        f.backend->decodeRadioInfoMetadata(QStringLiteral("region=USA"));
        QVERIFY(!f.radio.iaruRegion().has_value());
        f.backend->decodeRadioInfoMetadata(QStringLiteral("iaru_region=1"));
        f.backend->decodeRadioInfoMetadata(QStringLiteral("iaru_region=not-a-region"));
        QVERIFY(!f.radio.iaruRegion().has_value());
        QVERIFY(f.noCommands());
    }

    void chainNotificationSeesTheCompleteStatusAndNewerReportWins()
    {
        Fixture f;
        QVERIFY(f.init());
        SliceModel* slice = f.radio.slice(0);
        QString modeAtNotification;
        bool nested = false;
        connect(slice, &SliceModel::chainModeChanged, slice, [&](const QString& chain) {
            if (chain == QLatin1String("DIGU") && !nested) {
                modeAtNotification = slice->mode();
                nested = true;
                f.backend->decodeSliceStatus(0, {{"mode", "LSB"}, {"chain_mode", "LSB"}});
            }
        });
        f.backend->decodeSliceStatus(0, {{"mode", "FDVU"}, {"chain_mode", "DIGU"}});
        QCOMPARE(modeAtNotification, QStringLiteral("FDVU"));
        QCOMPARE(slice->mode(), QStringLiteral("LSB"));
        QCOMPARE(slice->chainMode(), QStringLiteral("LSB"));
    }

    void infoNotificationSeesCompleteIdentityAndDisconnectStopsTheReply()
    {
        Fixture f;
        QVERIFY(f.init());
        const quint32 seq = f.requestInfo();
        QString regionAtNotification;
        bool retired = false;
        connect(&f.radio, &RadioModel::infoChanged, &f.radio, [&] {
            if (!retired && f.radio.iaruRegion() == 3) {
                retired = true;
                regionAtNotification = f.radio.region();
                RadioNicknameTestAccess::disconnect(f.radio);
            }
        });
        f.reply(seq, 0, QStringLiteral("region=Current,iaru_region=3,name=Current"));
        QCOMPARE(regionAtNotification, QStringLiteral("Current"));
        QVERIFY(!f.radio.iaruRegion());
        QVERIFY(f.radio.region().isEmpty());
        QVERIFY(f.radio.nickname().isEmpty());
    }

    void callsignNotificationCannotPublishRetiredMetadata()
    {
        Fixture f;
        QVERIFY(f.init());
        const quint32 seq = f.requestInfo();
        bool retired = false;
        connect(&f.radio, &RadioModel::callsignChanged, &f.radio, [&] {
            if (!retired) {
                retired = true;
                RadioNicknameTestAccess::disconnect(f.radio);
            }
        });
        f.reply(seq, 0, QStringLiteral("callsign=NEW,region=Retired,iaru_region=3"));
        QVERIFY(retired);
        QVERIFY(!f.radio.iaruRegion());
        QVERIFY(f.radio.region().isEmpty());
    }

    void stagingNotificationCannotDispatchThroughRetiringSlices()
    {
        Fixture f;
        QVERIFY(f.init());
        f.backend->decodeRadioInfoMetadata(QStringLiteral("iaru_region=2"));
        SliceModel* slice = f.radio.slice(0);
        connect(&f.radio, &RadioModel::infoChanged, &f.radio, [slice] {
            slice->setFrequency(14.3);
        });
        f.radio.stageSessionModelsForReconnectForTest();
        QVERIFY(f.noCommands());
    }

    void stagingNotificationCannotReviveARetiredInfoReply()
    {
        Fixture f;
        QVERIFY(f.init());
        f.backend->decodeRadioInfoMetadata(QStringLiteral("iaru_region=2"));
        const quint32 seq = f.requestInfo();
        const RadioConnection::ResponseCallback old = RadioNicknameTestAccess::callback(f.radio, seq);
        QVERIFY(old);
        bool attempted = false;
        connect(&f.radio, &RadioModel::infoChanged, &f.radio, [&] {
            if (!attempted) {
                attempted = true;
                old(0, QStringLiteral("iaru_region=1"));
            }
        });
        f.radio.stageSessionModelsForReconnectForTest();
        QVERIFY(attempted);
        QVERIFY(!f.radio.iaruRegion());
    }

    void productionInfoReplyUpdatesBridgeAndErrorPreservesObservation()
    {
        Fixture f;
        QVERIFY(f.init());
        const quint32 current = f.requestInfo();
        QVERIFY(current != 0);
        QCOMPARE(f.wireCommands, QStringList{QStringLiteral("info")});
        QVERIFY(RadioNicknameTestAccess::callback(f.radio, current));
        f.reply(current, 0, QStringLiteral("region=USA,iaru_region=2,name=Current"));
        QCOMPARE(f.radio.iaruRegion().value_or(0), 2);
        QCOMPARE(f.radio.region(), QStringLiteral("USA"));
        QCOMPARE(f.get(QStringLiteral("radio"), QStringLiteral("iaruRegion"))
                     .value(QStringLiteral("value")), QJsonValue(2));
        QVERIFY(!RadioNicknameTestAccess::callback(f.radio, current));

        const quint32 error = f.requestInfo();
        QVERIFY(error != 0);
        QSignalSpy changed(&f.radio, &RadioModel::infoChanged);
        f.reply(error, 1, QStringLiteral("region=Other,iaru_region=3"));
        QCOMPARE(f.radio.iaruRegion().value_or(0), 2);
        QCOMPARE(f.radio.region(), QStringLiteral("USA"));
        QCOMPARE(changed.count(), 0);
        QCOMPARE(f.wireCommands, QStringList{QStringLiteral("info")});

        const quint32 absent = f.requestInfo();
        QVERIFY(absent != 0);
        f.reply(absent, 0, QStringLiteral("region=USA"));
        QVERIFY(!f.radio.iaruRegion().has_value());
        const QJsonObject property = f.get(QStringLiteral("radio"), QStringLiteral("iaruRegion"));
        QVERIFY(property.value(QStringLiteral("ok")).toBool());
        QVERIFY(property.value(QStringLiteral("value")).isNull());
        QCOMPARE(f.wireCommands, QStringList{QStringLiteral("info")});
    }

    void retainedInfoCallbackCannotCrossDisconnectAndReconnect()
    {
        Fixture f;
        QVERIFY(f.init());
        const quint32 oldSeq = f.requestInfo();
        QVERIFY(oldSeq != 0);
        const RadioConnection::ResponseCallback old = RadioNicknameTestAccess::callback(f.radio, oldSeq);
        QVERIFY(old);
        const quint64 oldSession = RadioNicknameTestAccess::session(f.radio);
        const quint64 oldModels = RadioNicknameTestAccess::modelGeneration(f.radio);
        const quint64 oldBackend = RadioNicknameTestAccess::backendGeneration(f.radio);
        RadioNicknameTestAccess::disconnect(f.radio);
        // Reopen only transport admission: no connect callback can start the
        // UDP stream, and the session guard is tested without a model edge.
        RadioConnectionSessionTestAccess::capture(*f.backend->connection(), &f.wireCommands, &f.wireRequests);
        QVERIFY(RadioNicknameTestAccess::session(f.radio) != oldSession);
        QCOMPARE(RadioNicknameTestAccess::modelGeneration(f.radio), oldModels);
        QCOMPARE(RadioNicknameTestAccess::backendGeneration(f.radio), oldBackend);
        const quint32 current = f.requestInfo();
        QVERIFY(current != 0);
        f.reply(current, 0, QStringLiteral("region=Current,iaru_region=3"));
        QCOMPARE(f.radio.iaruRegion().value_or(0), 3);
        QSignalSpy changed(&f.radio, &RadioModel::infoChanged);
        old(0, QStringLiteral("region=Old,iaru_region=1"));
        QCOMPARE(f.radio.iaruRegion().value_or(0), 3);
        QCOMPARE(f.radio.region(), QStringLiteral("Current"));
        QCOMPARE(changed.count(), 0);
        QCOMPARE(f.get(QStringLiteral("radio"), QStringLiteral("iaruRegion"))
                     .value(QStringLiteral("value")), QJsonValue(3));
    }

    void retainedInfoCallbackCannotCrossStagingWithoutDisconnect()
    {
        Fixture f;
        QVERIFY(f.init());
        const quint32 oldSeq = f.requestInfo();
        QVERIFY(oldSeq != 0);
        const RadioConnection::ResponseCallback old = RadioNicknameTestAccess::callback(f.radio, oldSeq);
        QVERIFY(old);
        const quint64 oldSession = RadioNicknameTestAccess::session(f.radio);
        const quint64 oldModels = RadioNicknameTestAccess::modelGeneration(f.radio);
        const quint64 oldBackend = RadioNicknameTestAccess::backendGeneration(f.radio);
        f.radio.stageSessionModelsForReconnectForTest();
        QCOMPARE(RadioNicknameTestAccess::session(f.radio), oldSession);
        QVERIFY(RadioNicknameTestAccess::modelGeneration(f.radio) != oldModels);
        QCOMPARE(RadioNicknameTestAccess::backendGeneration(f.radio), oldBackend);
        const quint32 current = f.requestInfo();
        QVERIFY(current != 0);
        f.reply(current, 0, QStringLiteral("region=New-session,iaru_region=2"));
        QCOMPARE(f.radio.iaruRegion().value_or(0), 2);
        QSignalSpy changed(&f.radio, &RadioModel::infoChanged);
        old(0, QStringLiteral("region=Old-session,iaru_region=1"));
        QCOMPARE(f.radio.iaruRegion().value_or(0), 2);
        QCOMPARE(f.radio.region(), QStringLiteral("New-session"));
        QCOMPARE(changed.count(), 0);
    }

    void retainedInfoCallbackCannotCrossSameFamilyBackendReplacement()
    {
        Fixture f;
        QVERIFY(f.init());
        const quint32 oldSeq = f.requestInfo();
        QVERIFY(oldSeq != 0);
        const RadioConnection::ResponseCallback old = RadioNicknameTestAccess::callback(f.radio, oldSeq);
        QVERIFY(old);
        const quint64 oldBackend = RadioNicknameTestAccess::backendGeneration(f.radio);
        const quint64 oldSession = RadioNicknameTestAccess::session(f.radio);
        const quint64 oldModels = RadioNicknameTestAccess::modelGeneration(f.radio);
        f.detach();
        QVERIFY(f.radio.rebuildBackendForTest(QStringLiteral("flex")));
        f.backend = dynamic_cast<FlexBackend*>(f.radio.backend());
        QVERIFY(f.backend);
        QVERIFY(RadioNicknameTestAccess::backendGeneration(f.radio) != oldBackend);
        QCOMPARE(RadioNicknameTestAccess::session(f.radio), oldSession);
        QCOMPARE(RadioNicknameTestAccess::modelGeneration(f.radio), oldModels);
        RadioConnectionSessionTestAccess::capture(*f.backend->connection(), &f.wireCommands, &f.wireRequests);
        const quint32 current = f.requestInfo();
        QVERIFY(current != 0);
        QCOMPARE(f.wireCommands, QStringList{QStringLiteral("info")});
        f.reply(current, 0, QStringLiteral("region=Replacement,iaru_region=3"));
        QCOMPARE(f.radio.iaruRegion().value_or(0), 3);
        QSignalSpy changed(&f.radio, &RadioModel::infoChanged);
        old(0, QStringLiteral("region=Retired,iaru_region=1"));
        QCOMPARE(f.radio.iaruRegion().value_or(0), 3);
        QCOMPARE(f.radio.region(), QStringLiteral("Replacement"));
        QCOMPARE(changed.count(), 0);
        const QJsonObject snapshot = f.radio.troubleshootingSnapshot();
        QCOMPARE(snapshot.value(QStringLiteral("radio")).toObject()
                     .value(QStringLiteral("iaru_region")), QJsonValue(3));
    }

    void bridgeAndTroubleshootingCarryKnownAndUnknown()
    {
        Fixture f;
        QVERIFY(f.init());
        for (const bool known : {false, true, false}) {
            f.backend->decodeSliceStatus(0, {{QStringLiteral("chain_mode"),
                known ? QStringLiteral("DIGU") : QString()}});
            f.backend->decodeRadioInfoMetadata(known ? QStringLiteral("iaru_region=2") : QString());
            const QJsonValue chain = known ? QJsonValue(QStringLiteral("DIGU")) : QJsonValue();
            const QJsonValue region = known ? QJsonValue(2) : QJsonValue();
            for (const QString& model : {QStringLiteral("slice"), QStringLiteral("radio")}) {
                const QString field = model == QLatin1String("slice")
                    ? QStringLiteral("chainMode") : QStringLiteral("iaruRegion");
                const QJsonValue expected = model == QLatin1String("slice") ? chain : region;
                const QJsonObject whole = f.get(model);
                const QJsonObject property = f.get(model, field);
                std::printf("FIXTURE bridge %s %s whole %s\n", qPrintable(model),
                            known ? "known" : "unknown",
                            QJsonDocument(whole).toJson(QJsonDocument::Compact).constData());
                std::printf("FIXTURE bridge %s %s property %s\n", qPrintable(model),
                            known ? "known" : "unknown",
                            QJsonDocument(property).toJson(QJsonDocument::Compact).constData());
                QVERIFY(whole.value(QStringLiteral("ok")).toBool());
                QVERIFY(property.value(QStringLiteral("ok")).toBool());
                QVERIFY(whole.value(model).toObject().contains(field));
                QCOMPARE(whole.value(model).toObject().value(field), expected);
                QCOMPARE(property.value(QStringLiteral("value")), expected);
            }
            const QJsonObject snapshot = f.radio.troubleshootingSnapshot();
            QCOMPARE(snapshot.value(QStringLiteral("radio")).toObject()
                         .value(QStringLiteral("iaru_region")), region);
            const QJsonArray slices = snapshot.value(QStringLiteral("slices")).toArray();
            QCOMPARE(slices.size(), 1);
            QCOMPARE(slices.first().toObject().value(QStringLiteral("chain_mode")), chain);
            const QJsonObject all = f.get(QStringLiteral("slices"));
            QVERIFY(all.value(QStringLiteral("ok")).toBool());
            QCOMPARE(all.value(QStringLiteral("slices")).toArray().first().toObject()
                         .value(QStringLiteral("chainMode")), chain);
        }
        QVERIFY(f.noCommands());
    }

    void disconnectAndReconnectReuseClearSessionMetadata()
    {
        Fixture f;
        QVERIFY(f.init());
        SliceModel* original = f.radio.slice(0);
        f.backend->decodeSliceStatus(0, {{QStringLiteral("chain_mode"), QStringLiteral("DIGU")}});
        f.backend->decodeRadioInfoMetadata(QStringLiteral("iaru_region=2"));
        RadioNicknameTestAccess::disconnect(f.radio);
        QVERIFY(original->chainMode().isEmpty());
        QVERIFY(!f.radio.iaruRegion().has_value());
        f.radio.stageSessionModelsForReconnectForTest();
        QVERIFY(!f.radio.slice(0));
        f.radio.handleSliceStatusForTest(0, sliceStatus());
        QCOMPARE(f.radio.slice(0), original);
        QVERIFY(original->chainMode().isEmpty());
        f.backend->decodeSliceStatus(0, {{QStringLiteral("chain_mode"), QStringLiteral("FutureChain")}});
        QCOMPARE(original->chainMode(), QStringLiteral("FutureChain"));
        f.backend->decodeRadioInfoMetadata(QStringLiteral("iaru_region=3"));
        QCOMPARE(f.radio.iaruRegion().value_or(0), 3);
    }

    void stagingWithoutDisconnectClearsAndRepeatedStagingStaysUnknown()
    {
        Fixture f;
        QVERIFY(f.init());
        SliceModel* original = f.radio.slice(0);
        f.backend->decodeSliceStatus(0, {{QStringLiteral("chain_mode"), QStringLiteral("DIGU")}});
        f.backend->decodeRadioInfoMetadata(QStringLiteral("iaru_region=2"));
        f.radio.stageSessionModelsForReconnectForTest();
        QVERIFY(original->chainMode().isEmpty());
        QVERIFY(!f.radio.iaruRegion().has_value());
        f.radio.stageSessionModelsForReconnectForTest();
        QVERIFY(original->chainMode().isEmpty());
        f.radio.handleSliceStatusForTest(0, sliceStatus());
        QCOMPARE(f.radio.slice(0), original);
        QVERIFY(original->chainMode().isEmpty());
        QVERIFY(f.noCommands());
    }

    void removalAndIdReuseDoNotInheritChain()
    {
        Fixture f;
        QVERIFY(f.init());
        f.backend->decodeSliceStatus(0, {{QStringLiteral("chain_mode"), QStringLiteral("DIGU")}});
        f.radio.handleSliceStatusForTest(0, {}, true);
        QVERIFY(!f.radio.slice(0));
        f.radio.handleSliceStatusForTest(0, sliceStatus());
        QVERIFY(f.radio.slice(0));
        QVERIFY(f.radio.slice(0)->chainMode().isEmpty());
        f.backend->decodeSliceStatus(0, {{QStringLiteral("chain_mode"), QStringLiteral("USB")}});
        QCOMPARE(f.radio.slice(0)->chainMode(), QStringLiteral("USB"));
    }

    void backendAndFamilyReplacementClearMetadata()
    {
        Fixture f;
        QVERIFY(f.init());
        f.backend->decodeSliceStatus(0, {{QStringLiteral("chain_mode"), QStringLiteral("DIGU")}});
        f.backend->decodeRadioInfoMetadata(QStringLiteral("iaru_region=2"));
        f.detach();
        QVERIFY(f.radio.rebuildBackendForTest(QStringLiteral("sim")));
        QVERIFY(!f.radio.iaruRegion().has_value());
        QVERIFY(!f.radio.slice(0));
        QVERIFY(f.radio.rebuildBackendForTest(QStringLiteral("flex")));
        f.backend = dynamic_cast<FlexBackend*>(f.radio.backend());
        QVERIFY(f.backend);
        RadioConnectionSessionTestAccess::capture(*f.backend->connection(), &f.wireCommands);
        f.radio.handleSliceStatusForTest(0, sliceStatus());
        QVERIFY(f.radio.slice(0)->chainMode().isEmpty());
        QVERIFY(!f.radio.iaruRegion().has_value());
    }

    void passiveMetadataDoesNotRequestCommandsOrPersistBandPlan()
    {
        Fixture f;
        QVERIFY(f.init());
        AppSettings& settings = AppSettings::instance();
        settings.setValue(QStringLiteral("BandPlanName"), QStringLiteral("Operator-selected plan"));
        const QStringList keys{QStringLiteral("BandPlanName"), QStringLiteral("IaruRegion"),
            QStringLiteral("iaru_region"), QStringLiteral("ChainMode"), QStringLiteral("chain_mode")};
        QMap<QString, QVariant> before;
        for (const QString& key : keys) {
            before.insert(key, settings.value(key));
        }
        SliceModel* slice = f.radio.slice(0);
        QSignalSpy tune(slice, &SliceModel::receiveTuneRequested);
        QSignalSpy filter(slice, &SliceModel::receiveFilterRequested);
        QSignalSpy dsp(slice, &SliceModel::receiveDspRequested);
        QSignalSpy control(slice, &SliceModel::controlRequested);
        QSignalSpy legacyFrequency(slice, &SliceModel::frequencyCommandIssued);
        QSignalSpy legacyFilter(slice, &SliceModel::filterCommandIssued);
        for (const QString& chain : {QStringLiteral("USB"), QStringLiteral("DIGU"), QStringLiteral("FutureChain")}) {
            f.backend->decodeSliceStatus(0, {{QStringLiteral("chain_mode"), chain}});
            for (int region = 1; region <= 3; ++region) {
                f.backend->decodeRadioInfoMetadata(QStringLiteral("region=USA,iaru_region=%1").arg(region));
                f.get(QStringLiteral("slice"), QStringLiteral("chainMode"));
                f.get(QStringLiteral("radio"), QStringLiteral("iaruRegion"));
                f.radio.troubleshootingSnapshot();
            }
        }
        QCOMPARE(tune.count(), 0);
        QCOMPARE(filter.count(), 0);
        QCOMPARE(dsp.count(), 0);
        QCOMPARE(control.count(), 0);
        QCOMPARE(legacyFrequency.count(), 0);
        QCOMPARE(legacyFilter.count(), 0);
        QVERIFY(f.noCommands());
        for (const QString& key : keys) {
            QCOMPARE(settings.value(key), before.value(key));
        }
    }
};

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("flex-readonly-metadata"));
    qputenv("AETHER_AUTOMATION", "1");
    qunsetenv("AETHER_AUTOMATION_ALLOW_TX");
    QCoreApplication app(argc, argv);
    if (!profile.isValid()) {
        return 1;
    }
    FlexReadonlyMetadataTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "flex_readonly_metadata_test.moc"
