// Production parsing, transport dispatch and owner-thread lifecycle, with
// in-memory ingress. No socket is connected, bound or used as a fake radio.
#include "FlexSliceStatusTestSupport.h"
#include "TestSettingsProfile.h"
#include "core/backends/flex/RadioConnection.h"
#include "core/backends/flex/WanConnection.h"
#include "core/control/RadioResourceAdapter.h"
#include "models/SliceModel.h"

#include <QCoreApplication>
#include <QEvent>
#include <QPointer>
#include <QSignalSpy>
#include <QTest>
#include <memory>

namespace AetherSDR {
class RadioConnectionSessionTestAccess {
public:
    static void line(RadioConnection& connection, const QString& text)
    { connection.processLine(text); }
    static void reset(RadioConnection& connection) { connection.resetSessionState(); }
    static void owner(RadioConnection& connection) { connection.m_handle.store(1); }
};
class WanConnectionTestAccess {
public:
    static void line(WanConnection& connection, const QString& text)
    { connection.processLine(text); }
    static void reset(WanConnection& connection) { connection.resetSessionState(); }
};
class RadioModelSliceLifecycleTestAccess {
public:
    static void quarantine(RadioModel& radio, int id)
    {
        SliceModel* model = radio.slice(id);
        radio.m_slices.removeOne(model);
        radio.m_staleSlices.insert(id, model);
    }
    static bool owns(const RadioModel& radio, int id) { return radio.m_ownedSliceIds.contains(id); }
    static quint64 retirement(const RadioModel& radio, int id) { return radio.m_sliceRetirements.value(id); }
    static void newSession(RadioModel& radio) { ++radio.m_sessionGeneration; }
};
}

using namespace AetherSDR;
namespace {
const QMap<QString, QString> kPresent{{"in_use", "1"}, {"client_handle", "1"},
    {"RF_frequency", "14.2"}, {"mode", "USB"}, {"pan", "0x40000000"},
    {"filter_lo", "100"}, {"filter_hi", "2900"}};

struct Fixture {
    RadioModel radio;
    FlexBackend* backend{};
    Fixture()
    {
        radio.rebuildBackendForTest(QStringLiteral("flex"));
        backend = static_cast<FlexBackend*>(radio.backend());
        QMetaObject::invokeMethod(backend->connection(), [this] {
            RadioConnectionSessionTestAccess::owner(*backend->connection());
        }, Qt::BlockingQueuedConnection);
    }
    void report(int id, const QMap<QString, QString>& fields)
    { publishFlexSliceStatus(*backend, id, fields); }
};
}

class FlexSliceObservationTest final : public QObject {
    Q_OBJECT
private slots:
    void parserRejectsMalformedIdentityAndPreservesAbsence()
    {
        for (const QString& object : {QStringLiteral("slice -1"), QStringLiteral("slice 2147483648"),
                QStringLiteral("slice invalid"), QStringLiteral("slice 3 waveform_status")}) {
            QVERIFY(!FlexSliceStatus::decode(object, kPresent));
        }
        auto result = FlexSliceStatus::decode(QStringLiteral("slice 3"), {
            {"RF_frequency", "nan"}, {"audio_level", "inf"}, {"client_handle", "100000000"},
            {"in_use", "bad"}, {"active", "0"}, {"audio_mute", "0"}});
        QVERIFY(result);
        QVERIFY(!result->ownerHandle);
        QVERIFY(!result->values.frequency);
        QVERIFY(!result->values.audioGain);
        QVERIFY(!result->values.inUse);
        QCOMPARE(result->values.active, std::optional<bool>(false));
        QCOMPARE(result->values.audioMute, std::optional<bool>(false));
        result = FlexSliceStatus::decode(QStringLiteral("slice 3"), {{"removed", "1"}});
        QVERIFY(result && result->values.inUse == false);
        QVERIFY(!FlexSliceStatus::decode(QStringLiteral("slice 3"), {{"mode_list", QString(65537, 'x')}}));
    }

    void bothTransportsEmitTypedOnlyAndRevokeQueuedEpochs()
    {
        RadioConnection lan;
        WanConnection wan;
        QSignalSpy lanRaw(&lan, &RadioConnection::statusReceived);
        QSignalSpy wanRaw(&wan, &WanConnection::statusReceived);
        QSignalSpy lanTyped(&lan, &RadioConnection::sliceObserved);
        QSignalSpy wanTyped(&wan, &WanConnection::sliceObserved);
        const QString text = QStringLiteral("S1|slice 3 in_use=1 client_handle=0x1 RF_frequency=14.2 active=0");
        RadioConnectionSessionTestAccess::line(lan, text);
        WanConnectionTestAccess::line(wan, text);
        QCOMPARE(lanTyped.size(), 1);
        QCOMPARE(wanTyped.size(), 1);
        QVERIFY(lanRaw.isEmpty() && wanRaw.isEmpty());
        const SliceObservation a = lanTyped.at(0).at(0).value<SliceObservation>();
        const SliceObservation b = wanTyped.at(0).at(0).value<SliceObservation>();
        QVERIFY(a.current() && b.current());
        QCOMPARE(a.ownerHandle, b.ownerHandle);
        QCOMPARE(a.values.frequency, b.values.frequency);
        RadioConnectionSessionTestAccess::reset(lan);
        WanConnectionTestAccess::reset(wan);
        QVERIFY(!a.current() && !b.current());
        RadioConnectionSessionTestAccess::line(lan, QStringLiteral("S1|slice 3 waveform_status=ready"));
        WanConnectionTestAccess::line(wan, QStringLiteral("S1|slice 3 waveform_status=ready"));
        QCOMPARE(lanRaw.size(), 1);
        QCOMPARE(wanRaw.size(), 1);
    }

    void reentrantRawObserverCannotRelabelOldReport()
    {
        RadioConnection lan;
        WanConnection wan;
        QSignalSpy lanTyped(&lan, &RadioConnection::sliceObserved);
        QSignalSpy wanTyped(&wan, &WanConnection::sliceObserved);
        connect(&lan, &RadioConnection::messageReceived, &lan, [&] {
            RadioConnectionSessionTestAccess::reset(lan);
        });
        connect(&wan, &WanConnection::messageReceived, &wan, [&] {
            WanConnectionTestAccess::reset(wan);
        });
        const QString text = QStringLiteral("S1|slice 0 in_use=1 RF_frequency=14.2");
        RadioConnectionSessionTestAccess::line(lan, text);
        WanConnectionTestAccess::line(wan, text);
        QVERIFY(lanTyped.isEmpty() && wanTyped.isEmpty());
    }

    void workerQueueCannotResurrectPreviousSession()
    {
        Fixture f;
        QSignalSpy observed(f.backend, &IRadioBackend::sliceObserved);
        RadioConnection* source = f.backend->connection();
        QMetaObject::invokeMethod(source, [source] {
            RadioConnectionSessionTestAccess::line(*source,
                QStringLiteral("S1|slice 0 in_use=1 client_handle=1 RF_frequency=14.2"));
            RadioConnectionSessionTestAccess::reset(*source);
        }, Qt::BlockingQueuedConnection);
        QCoreApplication::sendPostedEvents(f.backend, QEvent::MetaCall);
        QVERIFY(observed.isEmpty());
        QVERIFY(!f.radio.slice(0));
        QVERIFY(!f.radio.isSlotForeign(0));
        QMetaObject::invokeMethod(source, [source] {
            RadioConnectionSessionTestAccess::owner(*source);
            RadioConnectionSessionTestAccess::line(*source,
                QStringLiteral("S1|slice 0 in_use=1 client_handle=1 RF_frequency=7.244 mode=LSB"));
        }, Qt::BlockingQueuedConnection);
        QCoreApplication::sendPostedEvents(f.backend, QEvent::MetaCall);
        QCOMPARE(observed.size(), 1);
        QVERIFY(f.radio.slice(0));
        QCOMPARE(f.radio.slice(0)->frequency(), 7.244);
    }

    void ownedForeignRemovalAndRecreatePublishOnce()
    {
        Fixture f;
        QSignalSpy added(&f.radio, &RadioModel::sliceAdded);
        QSignalSpy removed(&f.radio, &RadioModel::sliceRemoved);
        QSignalSpy occupancy(&f.radio, &RadioModel::slotOccupancyChanged);
        f.report(0, {{"RF_frequency", "7.1"}});
        QVERIFY(!f.radio.slice(0));
        f.report(0, kPresent);
        QVERIFY(f.radio.slice(0));
        QCOMPARE(added.size(), 1);
        const QPointer<SliceModel> first = f.radio.slice(0);
        f.report(0, {{"audio_mute", "1"}});
        QCOMPARE(f.radio.slice(0)->frequency(), 14.2);
        QCOMPARE(added.size(), 1);
        occupancy.clear();
        const quint64 revision = RadioModelSliceLifecycleTestAccess::retirement(f.radio, 0);
        f.report(0, {{"client_handle", "99"}, {"in_use", "1"}});
        QVERIFY(!f.radio.slice(0) && f.radio.isSlotForeign(0));
        QCOMPARE(removed.size(), 1);
        QCOMPARE(occupancy.size(), 1);
        QCOMPARE(RadioModelSliceLifecycleTestAccess::retirement(f.radio, 0), revision + 1);
        f.report(0, {{"client_handle", "99"}, {"in_use", "0"}});
        QVERIFY(!f.radio.isSlotForeign(0));
        QVERIFY(!RadioModelSliceLifecycleTestAccess::owns(f.radio, 0));
        QCOMPARE(removed.size(), 1);
        QCOMPARE(occupancy.size(), 2);
        f.report(0, kPresent);
        QVERIFY(f.radio.slice(0) && f.radio.slice(0) != first);
        QCOMPARE(added.size(), 2);
        QCOMPARE(occupancy.size(), 3);
    }

    void staleObjectIsReclaimedButCannotSurviveOwnershipLoss()
    {
        Fixture f;
        f.report(0, kPresent);
        const QPointer<SliceModel> first = f.radio.slice(0);
        QSignalSpy added(&f.radio, &RadioModel::sliceAdded);
        QSignalSpy removed(&f.radio, &RadioModel::sliceRemoved);
        RadioModelSliceLifecycleTestAccess::quarantine(f.radio, 0);
        f.report(0, kPresent);
        QCOMPARE(f.radio.slice(0), first.data());
        QVERIFY(added.isEmpty());
        RadioModelSliceLifecycleTestAccess::quarantine(f.radio, 0);
        f.report(0, {{"client_handle", "99"}});
        QCOMPARE(removed.size(), 1);
        f.report(0, kPresent);
        QVERIFY(f.radio.slice(0) != first);
        QCOMPARE(added.size(), 1);
    }

    void reentrantMetadataCannotOverwriteNewerSameSlice()
    {
        Fixture f;
        f.report(0, kPresent);
        connect(&f.radio, &RadioModel::rawSliceModeListsChanged, &f.radio, [&] {
            f.report(0, {{"RF_frequency", "7.244"}});
        });
        f.report(0, {{"mode_list", "USB,LSB"}, {"RF_frequency", "14.3"}});
        QCOMPARE(f.radio.slice(0)->frequency(), 7.244);
    }

    void reentrantOtherSliceDoesNotDropCurrentUpdate()
    {
        Fixture f;
        f.report(0, kPresent);
        f.report(1, kPresent);
        connect(&f.radio, &RadioModel::rawSliceModeListsChanged, &f.radio, [&] {
            f.report(1, {{"RF_frequency", "7.244"}});
        });
        f.report(0, {{"mode_list", "USB,LSB"}, {"RF_frequency", "14.3"}});
        QCOMPARE(f.radio.slice(0)->frequency(), 14.3);
        QCOMPARE(f.radio.slice(1)->frequency(), 7.244);
    }

    void resourceAdapterTracksRemovalRecreationAndAuthority()
    {
        Fixture f;
        control::ControlResourceStore store;
        control::RadioResourceAdapter adapter(&f.radio, &store, QStringLiteral("radio-1"));
        const control::ResourceAddress address{QStringLiteral("slice"), QStringLiteral("radio-1"), QStringLiteral("0")};
        f.report(0, kPresent);
        const auto first = store.get(address);
        QVERIFY(first);
        QVERIFY(first->value.value(QStringLiteral("owned")).toBool());
        QCOMPARE(first->value.value(QStringLiteral("frequencyHz")).toInteger(), 14200000);
        f.report(0, {{"client_handle", "99"}});
        QVERIFY(!store.get(address));
        f.report(0, kPresent);
        const auto replacement = store.get(address);
        QVERIFY(replacement && replacement->revision > first->revision);
        QCOMPARE(replacement->value.value(QStringLiteral("frequencyHz")).toInteger(), 14200000);
        f.report(0, {{"RF_frequency", "7.244"}});
        QVERIFY(store.get(address)->revision > replacement->revision);
        QCOMPARE(store.get(address)->value.value(QStringLiteral("frequencyHz")).toInteger(), 7244000);
    }

    void clientDisconnectClearsEveryForeignSlotBeforeNotification()
    {
        Fixture f;
        f.report(1, {{"client_handle", "99"}});
        f.report(2, {{"client_handle", "99"}});
        bool allReleased = true;
        bool replaced = false;
        connect(&f.radio, &RadioModel::slotOccupancyChanged, &f.radio, [&](int id) {
            if (id == 1 && !replaced) {
                replaced = true;
                allReleased = !f.radio.isSlotForeign(1) && !f.radio.isSlotForeign(2);
                f.report(1, {{"client_handle", "88"}});
            }
        });
        f.radio.handleStatusForTest(QStringLiteral("client 0x99 disconnected"), {});
        QVERIFY(allReleased);
        QVERIFY(f.radio.isSlotForeign(1));
        QVERIFY(!f.radio.isSlotForeign(2));
    }

    void wanSourceReplacementRejectsRetiredEpoch()
    {
        Fixture f;
        WanConnection first;
        WanConnection next;
        f.backend->attachWanSliceSource(&first);
        WanConnectionTestAccess::line(first,
            QStringLiteral("S1|slice 0 in_use=1 RF_frequency=14.2"));
        QVERIFY(f.radio.slice(0));
        f.backend->attachWanSliceSource(&next);
        WanConnectionTestAccess::line(first,
            QStringLiteral("S1|slice 0 in_use=0"));
        QVERIFY(f.radio.slice(0));
        WanConnectionTestAccess::line(next,
            QStringLiteral("S1|slice 0 RF_frequency=7.244"));
        QCOMPARE(f.radio.slice(0)->frequency(), 7.244);
        f.backend->attachWanSliceSource(nullptr);
    }

    void removalDuringNotificationCannotPublishRetiredSlice()
    {
        Fixture f;
        QSignalSpy added(&f.radio, &RadioModel::sliceAdded);
        connect(&f.radio, &RadioModel::rawSliceModeListsChanged, &f.radio, [&] {
            f.report(0, {{"in_use", "0"}});
        });
        QMap<QString, QString> initial = kPresent;
        initial.insert(QStringLiteral("mode_list"), QStringLiteral("USB,LSB"));
        f.report(0, initial);
        QVERIFY(!f.radio.slice(0));
        QVERIFY(added.isEmpty());
    }
};

int main(int argc, char** argv)
{
    TestSettingsProfile settings(QStringLiteral("flex-slice-observation"));
    QCoreApplication app(argc, argv);
    FlexSliceObservationTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "flex_slice_observation_test.moc"
