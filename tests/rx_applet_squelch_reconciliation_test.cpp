#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/backends/RadioCapabilities.h"
#include "core/backends/flex/FlexBackend.h"
#include "core/backends/SquelchLevelScale.h"
#include "core/backends/SliceDelta.h"
#include "gui/RxApplet.h"
#include "gui/DisplaySquelchRouting.h"
#include "gui/VfoWidget.h"
#include "models/SliceModel.h"
#include "models/RadioModel.h"

#include <QApplication>
#include <QPushButton>
#include <QSignalSpy>
#include <QSlider>
#include <QtTest>

using namespace AetherSDR;

namespace AetherSDR {
struct RxAppletSquelchTestAccess {
    static QSlider* slider(RxApplet& applet) { return applet.m_sqlSlider; }
    static void mode(RxApplet& applet, RxApplet::SqlMode mode, bool propagate) {
        applet.setSqlMode(mode, propagate);
    }
    static void engineSql(RxApplet& applet) {
        applet.m_exclusiveSquelch = SquelchLevelScale{-120, 1.2, {"FM", "FMN"}, false, "dBFS/2048-bin", true, true, false};
    }
};
struct RadioModelWakeTestAccess {
    static void identity(RadioModel& radio, const QString& family, const QString& id)
    {
        radio.m_family = family;
        radio.m_lastInfo.serial = id;
    }
};
}

namespace {

class EngineSqlBackend final : public IRadioBackend {
public:
    bool refuse = false;
    bool optimisticPolicy = false;
    std::optional<RadioCapabilities> published;
    QVector<SliceSquelchRequest> requests;
    QVector<int> requestIds;
    RadioCapabilities capabilities() const override {
        if (published) { return *published; }
        RadioCapabilities caps;
        caps.squelchLevelScale = SquelchLevelScale{-120, 1.2, {"FM", "FMN"}, false,
            "dBFS/2048-bin", true, true, false};
        return caps;
    }
    bool isConnected() const override { return true; }
    ReceiveControlPolicy receiveControlPolicy() const override {
        return optimisticPolicy ? ReceiveControlPolicy::Optimistic : ReceiveControlPolicy::Confirmed;
    }
    ReceiveDispatch requestSliceSquelch(int id, const SliceSquelchRequest& request) override {
        requestIds.append(id);
        requests.append(request);
        return refuse ? ReceiveDispatch::Unsupported : ReceiveDispatch::Dispatched;
    }
    void connectRadio(const RadioConnectRequest&) override {}
    void disconnectRadio() override {}
    void setSliceFrequency(int, double) override {}
    void setSliceMode(int, const QString&) override {}
    void setSliceFilter(int, int, int) override {}
    void setSliceAgc(int, const QString&, int) override {}
    void setPanCenter(const QString&, double, PanCenterIntent) override {}
    void setKeying(bool, const TxCoordinator::Operation&, const TxCoordinator::Completion&) override {}
    void invokeExtension(const QString&, const QString&, quint64, const QVariant&) override {}
};

// Inject normalized radio state, never a socket peer or a copied UI machine.
void status(SliceModel& slice, bool on, int level, const QString& mode = {})
{
    SliceDelta delta;
    delta.squelchOn = on;
    delta.squelchLevel = level;
    if (!mode.isEmpty()) {
        delta.mode = mode;
    }
    slice.applyChanges(delta);
}

template <typename T>
T* control(QWidget& widget, const QString& name)
{
    for (T* child : widget.findChildren<T*>()) {
        if (child->accessibleName() == name) {
            return child;
        }
    }
    return nullptr;
}

} // namespace

class RxAppletSquelchReconciliationTest : public QObject
{
    Q_OBJECT

private slots:
    void flexPerSliceAndPan()
    {
        RadioModel radio;
        auto backend = std::make_unique<EngineSqlBackend>();
        backend->published = FlexBackend().capabilities();
        EngineSqlBackend* source = backend.get();
        radio.setBackendForTest(std::move(backend), QStringLiteral("test"));
        for (int id = 0; id < 4; ++id) {
            SliceDelta delta;
            delta.panId = id < 2 ? "0x40000000" : "0x40000001";
            delta.mode = "USB";
            delta.squelchOn = false;
            delta.squelchLevel = 54 + id;
            emit source->sliceChanged(id, delta);
        }
        SliceModel* a = radio.slice(0);
        SliceModel* b = radio.slice(1);
        SliceModel* c = radio.slice(2);
        SliceModel* d = radio.slice(3);
        const QString panA = a->panId();
        const QString panB = c->panId();
        QCOMPARE(b->panId(), panA);
        QCOMPARE(d->panId(), panB);
        QVERIFY(panA != panB);
        RxApplet rx;
        rx.setRadioModel(&radio);
        rx.setSlice(a);
        RxAppletSquelchTestAccess::mode(rx, RxApplet::SqlMode::Auto, true);
        QVERIFY(a->displayAutoSquelch());
        QCOMPARE(source->requestIds.last(), 0);
        QCOMPARE(source->requests.last().level, 54); // Never the 10 dB margin.
        status(*a, true, 60);
        QCOMPARE(a->manualSquelchLevel(), 54);
        rx.setSqlSliderValueExternal(7);
        source->requests.clear(); source->requestIds.clear();
        rx.setSlice(c);
        QCOMPARE(rx.sqlMode(), RxApplet::SqlMode::Off);
        QVERIFY(!c->displayAutoSquelch());
        QVERIFY(source->requests.isEmpty()); // Focus is not Auto activation.
        QVERIFY(a->displayAutoSquelch());
        status(*a, true, 61);
        QCOMPARE(a->manualSquelchLevel(), 54); // Background echoes stay Auto.
        RxAppletSquelchTestAccess::mode(rx, RxApplet::SqlMode::Auto, true);
        rx.setSqlSliderValueExternal(17);
        b->setDisplayAutoSquelchMarginDb(12);
        b->setDisplayAutoSquelch(true);
        source->requests.clear(); source->requestIds.clear();
        DisplaySquelchRouting::applyFloor(radio.slices(), panA, radio, -110);
        QCOMPARE(source->requestIds, QVector<int>({0, 1}));
        QCOMPARE(source->requests[0].level, 57);
        QCOMPARE(source->requests[1].level, 62);
        DisplaySquelchRouting::applyFloor(radio.slices(), panB, radio, -80);
        QCOMPARE(source->requestIds, QVector<int>({0, 1, 2}));
        QCOMPARE(source->requests.last().level, 97);
        QVERIFY(!d->displayAutoSquelch());
        DisplaySquelchRouting::applyFloor(radio.slices(), panB, radio, -80);
        QCOMPARE(source->requests.size(), 3); // Deduplicate per slice.
        QCOMPARE(DisplaySquelchRouting::displaySlice(radio.slices(), panA, c, 1), b);
        QCOMPARE(DisplaySquelchRouting::displaySlice(radio.slices(), panA, a, 1), a);
        rx.setSlice(a);
        QCOMPARE(rx.sqlMode(), RxApplet::SqlMode::Auto);
        QCOMPARE(rx.autoSqlMarginDb(), 7);
        QCOMPARE(c->displayAutoSquelchMarginDb().value(), 17);
        for (const QString& mode : QStringList{"DIGU", "DIGL", "RTTY", "NT", "CW", "CWU", "CWL", "DFM"}) {
            SliceDelta delta; delta.mode = mode;
            c->applyChanges(delta);
            source->requests.clear(); source->requestIds.clear();
            DisplaySquelchRouting::applyFloor(radio.slices(), panB, radio, -75);
            QVERIFY(source->requests.isEmpty()); // Includes inactive data/CW.
            QVERIFY(!DisplaySquelchRouting::scaleForSlice(c, radio));
        }
        SliceDelta migrated; migrated.panId = panB;
        a->applyChanges(migrated);
        source->requests.clear(); source->requestIds.clear();
        DisplaySquelchRouting::applyFloor(radio.slices(), panA, radio, -105);
        QCOMPARE(source->requestIds, QVector<int>({1}));
        DisplaySquelchRouting::applyFloor(radio.slices(), panB, radio, -85);
        QCOMPARE(source->requestIds, QVector<int>({1, 0}));
        status(*a, false, 82);
        QVERIFY(!a->displayAutoSquelch());
        source->requests.clear(); source->requestIds.clear();
        DisplaySquelchRouting::applyFloor(radio.slices(), panB, radio, -80);
        QVERIFY(source->requests.isEmpty()); // Radio Off wins over client intent.
        b->invalidateSquelchState();
        QVERIFY(!b->displayAutoSquelch()); // Reclaimed reconnect state cannot restart Auto.
        SliceModel reusedId(1);
        QVERIFY(!reusedId.displayAutoSquelch());
    }

    void detachedBackgroundRadioOffCancelsAuto()
    {
        RadioModel radio;
        auto backend = std::make_unique<EngineSqlBackend>();
        backend->published = FlexBackend().capabilities();
        EngineSqlBackend* source = backend.get();
        radio.setBackendForTest(std::move(backend), QStringLiteral("test"));
        SliceDelta delta;
        delta.panId = "0x40000000";
        delta.mode = "USB";
        delta.squelchOn = true;
        delta.squelchLevel = 54;
        emit source->sliceChanged(0, delta);
        emit source->sliceChanged(1, delta);
        RxApplet rx;
        rx.setRadioModel(&radio);
        rx.setSlice(radio.slice(0));
        SliceModel* background = radio.slice(1);
        background->setDisplayAutoSquelchMarginDb(10);
        background->setDisplayAutoSquelch(true);
        QVERIFY(!background->panId().isEmpty());
        QVERIFY(DisplaySquelchRouting::tracks(background, background->panId(), radio));
        DisplaySquelchRouting::applyFloor(radio.slices(), background->panId(), radio, -110);
        QVERIFY(!source->requests.isEmpty());
        status(*background, false, 60);
        QVERIFY(!background->displayAutoSquelch());
        source->requests.clear();
        DisplaySquelchRouting::applyFloor(radio.slices(), background->panId(), radio, -100);
        QVERIFY(source->requests.isEmpty());
    }

    void unavailableRoundTripPreservesManual_data()
    {
        QTest::addColumn<QString>("mode");
        QTest::addColumn<bool>("optimistic");
        QTest::addColumn<bool>("vfoFirst");
        for (const QString& mode : QStringList{"CW", "DIGU", "DFM"}) {
            for (bool optimistic : {false, true}) {
                for (bool vfoFirst : {false, true}) {
                    const QString name = mode + (optimistic ? "-requested" : "-reported")
                        + (vfoFirst ? "-vfo-first" : "-rx-first");
                    QTest::newRow(qPrintable(name)) << mode << optimistic << vfoFirst;
                }
            }
        }
    }

    void unavailableRoundTripPreservesManual()
    {
        QFETCH(QString, mode);
        QFETCH(bool, optimistic);
        QFETCH(bool, vfoFirst);
        RadioModel radio;
        auto backend = std::make_unique<EngineSqlBackend>();
        backend->published = FlexBackend().capabilities();
        backend->optimisticPolicy = optimistic;
        EngineSqlBackend* source = backend.get();
        radio.setBackendForTest(std::move(backend), QStringLiteral("test"));
        SliceDelta initial;
        initial.mode = "USB";
        initial.squelchOn = true;
        initial.squelchLevel = 54;
        emit source->sliceChanged(0, initial);
        SliceModel* slice = radio.slice(0);
        RxApplet rx;
        rx.setRadioModel(&radio);
        VfoWidget vfo;
        vfo.setRadioModel(&radio);
        vfo.setRxApplet(&rx);
        if (vfoFirst) { vfo.setSlice(slice); rx.setSlice(slice); }
        else { rx.setSlice(slice); vfo.setSlice(slice); }
        RxAppletSquelchTestAccess::mode(rx, RxApplet::SqlMode::Auto, false);
        status(*slice, true, 80);
        source->requests.clear();
        if (optimistic) {
            slice->setMode(mode);
        }
        status(*slice, true, mode == "CW" ? 80 : 20, mode);
        rx.setAutoSqlAvailability(false, mode);
        QVERIFY(slice->restoreManualSquelchAfterMode());
        QVERIFY(!slice->displayAutoSquelch());
        QCOMPARE(slice->manualSquelchLevel(), 54);
        if (mode == "CW") { QVERIFY(source->requests.isEmpty()); }
        source->requests.clear();
        rx.setAutoSqlAvailability(true, {});
        if (optimistic) {
            slice->setMode("USB");
            QVERIFY(source->requests.isEmpty()); // Wait for radio mode and level.
        }
        SliceDelta returning;
        returning.mode = "USB";
        slice->applyChanges(returning);
        QCOMPARE(source->requests.size(), 1); // Mode-only readback releases recovery.
        status(*slice, true, 80);
        QCOMPARE(slice->manualSquelchLevel(), 54);
        QCOMPARE(source->requests.size(), 1);
        QVERIFY(source->requests.last().enabledChanged);
        QVERIFY(source->requests.last().levelChanged);
        QCOMPARE(source->requests.last().level, 54);
        status(*slice, true, 80); // A late retained Auto level is still not manual.
        QCOMPARE(slice->manualSquelchLevel(), 54);
        QCOMPARE(source->requests.size(), 1); // No status-driven retry loop.
        status(*slice, true, 54);
        QVERIFY(!slice->restoreManualSquelchAfterMode());
        QCOMPARE(rx.sqlMode(), RxApplet::SqlMode::Manual);
        QVERIFY(RxAppletSquelchTestAccess::slider(rx)->isEnabled());
        QCOMPARE(RxAppletSquelchTestAccess::slider(rx)->value(), 54);
        status(*slice, true, 57); // Fresh manual edits remain radio-authoritative.
        QCOMPARE(slice->manualSquelchLevel(), 57);
        slice->setRestoreManualSquelchAfterMode(true);
        slice->invalidateSquelchState();
        QVERIFY(!slice->restoreManualSquelchAfterMode());
        source->requests.clear();
        status(*slice, true, 41, "USB");
        QVERIFY(source->requests.isEmpty()); // A new session never replays recovery.
    }

    void modeRecoveryReassertsBothFieldsEvenWhenSnapshotMatches_data()
    {
        QTest::addColumn<bool>("levelFirst");
        QTest::newRow("level-first") << true;
        QTest::newRow("enable-first") << false;
    }

    void modeRecoveryReassertsBothFieldsEvenWhenSnapshotMatches()
    {
        QFETCH(bool, levelFirst);
        SliceModel slice(0);
        status(slice, true, 20, "USB");
        slice.setRestoreManualSquelchAfterMode(true);
        QSignalSpy requests(&slice, &SliceModel::receiveSquelchRequested);
        slice.restoreManualSquelchAfterModeChange();
        QCOMPARE(requests.size(), 1);
        const SliceSquelchRequest request = qvariant_cast<SliceSquelchRequest>(requests[0][0]);
        QVERIFY(request.enabledChanged);
        QVERIFY(request.levelChanged);
        QVERIFY(request.enabled);
        QCOMPARE(request.level, 20);
        QVERIFY(!slice.squelchStateKnown());
        QVERIFY(slice.restoreManualSquelchAfterMode());
        SliceDelta enable; enable.squelchOn = true;
        SliceDelta level; level.squelchLevel = 20;
        slice.applyChanges(levelFirst ? level : enable);
        QVERIFY(slice.restoreManualSquelchAfterMode());
        slice.applyChanges(levelFirst ? enable : level);
        QVERIFY(!slice.restoreManualSquelchAfterMode());
        slice.restoreManualSquelchAfterModeChange();
        QCOMPARE(requests.size(), 1); // All controllers share one consumed latch.
    }

    void flexLateAutoEchoAfterDataDetachPreservesManual_data()
    {
        QTest::addColumn<bool>("vfoFirst");
        QTest::newRow("rx-first") << false;
        QTest::newRow("vfo-first") << true;
    }

    void flexLateAutoEchoAfterDataDetachPreservesManual()
    {
        QFETCH(bool, vfoFirst);
        RadioModel radio;
        auto backend = std::make_unique<EngineSqlBackend>();
        backend->published = FlexBackend().capabilities();
        EngineSqlBackend* source = backend.get();
        radio.setBackendForTest(std::move(backend), QStringLiteral("test"));
        SliceDelta initial; initial.mode = "USB";
        initial.squelchOn = true; initial.squelchLevel = 54;
        emit source->sliceChanged(0, initial);
        initial.squelchOn = false; initial.squelchLevel = 35;
        emit source->sliceChanged(1, initial);
        SliceModel* a = radio.slice(0);
        RxApplet rx; rx.setRadioModel(&radio);
        VfoWidget vfo; vfo.setRadioModel(&radio); vfo.setRxApplet(&rx);
        if (vfoFirst) { vfo.setSlice(a); rx.setSlice(a); }
        else { rx.setSlice(a); vfo.setSlice(a); }
        RxAppletSquelchTestAccess::mode(rx, RxApplet::SqlMode::Auto, false);
        status(*a, true, 80);
        QCOMPARE(a->manualSquelchLevel(), 54);
        status(*a, true, 20, "DIGU");
        rx.setAutoSqlAvailability(false, "DIGU");
        QCOMPARE(rx.sqlMode(), RxApplet::SqlMode::Off);
        source->requests.clear();
        rx.setAutoSqlAvailability(true, {});
        SliceDelta recovered; recovered.mode = "USB"; a->applyChanges(recovered);
        status(*a, true, 80);
        QCOMPARE(source->requests.last().level, 54); // Recovery restores manual, not Auto/radio fixed level.
        RxAppletSquelchTestAccess::mode(rx, RxApplet::SqlMode::Auto, false);
        status(*a, true, 80);
        status(*a, true, 20, "DIGU");
        rx.setAutoSqlAvailability(false, "DIGU");
        status(*a, false, 20); // Off may be optimistic when the applet detaches.
        QVERIFY(a->restoreManualSquelchAfterMode());
        rx.setSlice(radio.slice(1));
        QVERIFY(a->restoreManualSquelchAfterMode());
        QVERIFY(!radio.slice(1)->restoreManualSquelchAfterMode());
        rx.setSlice(a);
        SliceDelta returning; returning.mode = "USB";
        source->requests.clear(); a->applyChanges(returning);
        status(*a, true, 80);
        QCOMPARE(source->requests.last().level, 54);
        QVERIFY(a->restoreManualSquelchAfterMode());
        status(*a, true, 54);
        QVERIFY(!a->restoreManualSquelchAfterMode());
        rx.setAutoSqlAvailability(true, {});
        RxAppletSquelchTestAccess::mode(rx, RxApplet::SqlMode::Auto, false);
        status(*a, true, 80);
        status(*a, true, 20, "DIGU");
        rx.setAutoSqlAvailability(false, "DIGU");
        rx.setSlice(radio.slice(1));
        status(*a, true, 80); // Late Auto readback while the data slice is detached.
        QCOMPARE(a->manualSquelchLevel(), 54);
        rx.setSlice(a); // Fixed SQL-On data readback must not become Manual on reattach.
        QCOMPARE(rx.sqlMode(), RxApplet::SqlMode::Off);
        status(*a, true, 80);
        QCOMPARE(a->manualSquelchLevel(), 54);
        SliceDelta voice; voice.mode = "USB"; a->applyChanges(voice);
        rx.setAutoSqlAvailability(true, {});
        source->requests.clear();
        RxAppletSquelchTestAccess::mode(rx, RxApplet::SqlMode::Auto, true);
        QCOMPARE(source->requests.last().level, 54);
    }

    void flexUnavailableModes_data()
    {
        QTest::addColumn<QString>("mode");
        for (const QString& mode : QStringList{"DIGU", "DIGL", "RTTY", "NT", "CW", "CWU", "CWL", "DFM"}) {
            QTest::newRow(qPrintable(mode)) << mode;
        }
    }

    void flexUnavailableModes()
    {
        QFETCH(QString, mode);
        RadioModel radio;
        auto backend = std::make_unique<EngineSqlBackend>();
        backend->published = FlexBackend().capabilities();
        EngineSqlBackend* source = backend.get();
        radio.setBackendForTest(std::move(backend), QStringLiteral("test"));
        SliceDelta initial;
        initial.mode = QStringLiteral("USB");
        initial.squelchOn = true;
        initial.squelchLevel = 54;
        emit source->sliceChanged(0, initial);
        SliceModel* slice = radio.slice(0);
        QVERIFY(slice);
        RxApplet rx;
        rx.setRadioModel(&radio);
        rx.setSlice(slice);
        VfoWidget vfo;
        vfo.setRadioModel(&radio);
        vfo.setSlice(slice);
        RxAppletSquelchTestAccess::mode(rx, RxApplet::SqlMode::Auto, false);
        QCOMPARE(rx.sqlMode(), RxApplet::SqlMode::Auto);
        source->requests.clear();
        status(*slice, true, 54, mode);
        QVERIFY(!control<QPushButton>(rx, "Squelch mode")->isEnabled());
        QVERIFY(!control<QSlider>(rx, "Squelch threshold")->isEnabled());
        QVERIFY(!control<QPushButton>(vfo, "Squelch")->isEnabled());
        // The effective display gate demotes Auto without a CW radio write.
        rx.setAutoSqlAvailability(false, "Unavailable receive mode");
        QCOMPARE(rx.sqlMode(), RxApplet::SqlMode::Off);
        for (const SliceSquelchRequest& request : source->requests) {
            QVERIFY(!request.enabled);
        }
        if (mode.startsWith("CW")) {
            QCOMPARE(source->requests.size(), 0);
        }
        source->requests.clear();
        // Late On readback and saved/programmatic Auto must not restart it.
        status(*slice, true, 69, mode);
        rx.setAutoSqlAvailability(true, {});
        RxAppletSquelchTestAccess::mode(rx, RxApplet::SqlMode::Auto, true);
        QCOMPARE(rx.sqlMode(), RxApplet::SqlMode::Off);
        QCOMPARE(source->requests.size(), 0);
        status(*slice, false, 69, "USB");
        QVERIFY(control<QPushButton>(rx, "Squelch mode")->isEnabled());
        RxAppletSquelchTestAccess::mode(rx, RxApplet::SqlMode::Auto, false);
        QCOMPARE(rx.sqlMode(), RxApplet::SqlMode::Auto);
    }

    void engineSqlPublishesOnlyAcceptedModeAndMargin()
    {
        RadioModel radio;
        auto backend = std::make_unique<EngineSqlBackend>();
        EngineSqlBackend* source = backend.get();
        radio.setBackendForTest(std::move(backend), QStringLiteral("test"));
        SliceDelta accepted;
        accepted.mode = QStringLiteral("FMN");
        accepted.squelchOn = true;
        accepted.squelchLevel = 61;
        accepted.automaticSquelch = false;
        accepted.automaticSquelchMarginDb = 12;
        emit source->sliceChanged(0, accepted);
        SliceModel* slice = radio.slice(0);
        QVERIFY(slice);
        RxApplet rx;
        RxAppletSquelchTestAccess::engineSql(rx);
        rx.setSlice(slice);
        QSignalSpy modes(&rx, &RxApplet::sqlModeChanged);
        QSignalSpy automatic(&rx, &RxApplet::sqlAutoChanged);
        QSignalSpy margins(&rx, &RxApplet::autoSqlMarginDbChanged);
        rx.cycleSqlModeExternal(); // pending Auto is not accepted Auto
        QCOMPARE(source->requests.size(), 1);
        QCOMPARE(rx.sqlMode(), RxApplet::SqlMode::Manual);
        QCOMPARE(rx.sqlManualLevel(), 61);
        QVERIFY(modes.isEmpty());
        QVERIFY(automatic.isEmpty());
        source->refuse = true;
        rx.cycleSqlModeExternal();
        QCOMPARE(rx.sqlMode(), RxApplet::SqlMode::Manual);
        QVERIFY(modes.isEmpty());
        QVERIFY(automatic.isEmpty());
        source->refuse = false;
        accepted.automaticSquelch = true;
        emit source->sliceChanged(0, accepted);
        QCOMPARE(rx.sqlMode(), RxApplet::SqlMode::Auto);
        QCOMPARE(rx.autoSqlMarginDb(), 12);
        QCOMPARE(modes.size(), 1);
        QCOMPARE(automatic.size(), 1);
        margins.clear();
        QSlider* slider = RxAppletSquelchTestAccess::slider(rx);
        QVERIFY(slider);
        QCOMPARE(slider->value(), 12);
        slider->setValue(16);
        QCOMPARE(slider->value(), 12);
        QCOMPARE(source->requests.last().marginDb, 16);
        rx.setSqlSliderValueExternal(17);
        QCOMPARE(rx.autoSqlMarginDb(), 12);
        QCOMPARE(margins.last().at(0).toInt(), 12);
        source->refuse = true;
        rx.setSqlSliderValueExternal(17);
        QCOMPARE(rx.autoSqlMarginDb(), 12);
        QCOMPARE(margins.last().at(0).toInt(), 12);
        accepted.automaticSquelchMarginDb = 17;
        emit source->sliceChanged(0, accepted);
        QCOMPARE(rx.autoSqlMarginDb(), 17);
        QCOMPARE(slider->value(), 17);
        QCOMPARE(margins.last().at(0).toInt(), 17);
        rx.cycleSqlModeExternal(); // refused Off stays accepted Auto
        QCOMPARE(rx.sqlMode(), RxApplet::SqlMode::Auto);
        accepted.squelchOn = false;
        accepted.automaticSquelch = false;
        emit source->sliceChanged(0, accepted);
        QCOMPARE(rx.sqlMode(), RxApplet::SqlMode::Off);
    }

    void engineAutoRestoreKeepsManualThresholdOnAttachedSlice()
    {
        SliceModel first(0);
        RxApplet rx;
        RxAppletSquelchTestAccess::engineSql(rx);
        rx.setSlice(&first);
        QSignalSpy commands(&first, &SliceModel::commandReady);
        SliceDelta restored;
        restored.mode = QStringLiteral("FMN");
        restored.squelchOn = true;
        restored.squelchLevel = 54;
        restored.automaticSquelch = true;
        restored.automaticSquelchMarginDb = 13;
        first.applyChanges(restored);
        QCOMPARE(rx.sqlMode(), RxApplet::SqlMode::Auto);
        QCOMPARE(rx.autoSqlMarginDb(), 13);
        QCOMPARE(first.manualSquelchLevel(), 54);
        QCOMPARE(rx.sqlManualLevel(), 54);
        QVERIFY(commands.isEmpty());
        rx.cycleSqlModeExternal(); // Auto -> Off
        rx.cycleSqlModeExternal(); // Off -> Manual restores the saved threshold
        QCOMPARE(rx.sqlManualLevel(), 54);
        QCOMPARE(first.squelchLevel(), 54);
    }

    void engineAutoFollowsSelectedSliceWithoutCrossWrites()
    {
        SliceModel first(0), eighth(7);
        status(first, true, 54, "FMN");
        status(eighth, true, 61, "FM");
        SliceDelta automatic;
        automatic.automaticSquelch = true;
        automatic.automaticSquelchMarginDb = 13;
        eighth.applyChanges(automatic);
        RxApplet rx;
        RxAppletSquelchTestAccess::engineSql(rx);
        QSignalSpy firstAuto(&first, &SliceModel::receiveSquelchRequested);
        QSignalSpy eighthAuto(&eighth, &SliceModel::receiveSquelchRequested);
        rx.setSlice(&eighth);
        QCOMPARE(rx.sqlMode(), RxApplet::SqlMode::Auto);
        QCOMPARE(rx.autoSqlMarginDb(), 13);
        rx.setSlice(&first);
        QCOMPARE(rx.sqlMode(), RxApplet::SqlMode::Manual);
        QCOMPARE(rx.sqlManualLevel(), 54);
        QVERIFY(firstAuto.isEmpty() && eighthAuto.isEmpty());
        rx.cycleSqlModeExternal();
        QCOMPARE(firstAuto.count(), 1);
        QCOMPARE(qvariant_cast<SliceSquelchRequest>(firstAuto.at(0).at(0)).marginDb, 10);
        QVERIFY(eighthAuto.isEmpty());
        rx.setSlice(&eighth);
        rx.setSqlSliderValueExternal(17);
        QCOMPARE(eighthAuto.count(), 1);
        QCOMPARE(qvariant_cast<SliceSquelchRequest>(eighthAuto.at(0).at(0)).marginDb, 17);
        QCOMPARE(firstAuto.count(), 1);
        QCOMPARE(first.manualSquelchLevel(), 54);
        QCOMPARE(eighth.manualSquelchLevel(), 61);
        SliceDelta manual;
        manual.automaticSquelch = false;
        manual.squelchOn = true;
        manual.squelchLevel = 72;
        eighth.applyChanges(manual);
        QCOMPARE(rx.sqlMode(), RxApplet::SqlMode::Manual);
        QCOMPARE(rx.sqlManualLevel(), 72);
        QCOMPARE(first.manualSquelchLevel(), 54);
    }

    void deferredWritesCoalesceAndFlush()
    {
        int writes = 0;
        int value = 0;
        {
            AetherSDR::DeferredSettingsWrites pending;
            for (int i = 0; i < 100; ++i) {
                pending.schedule(QStringLiteral("a"), [&, i] { ++writes; value = i; });
            }
            QCOMPARE(writes, 0);
            QTRY_COMPARE(writes, 1);
            QCOMPARE(value, 99);
            pending.schedule(QStringLiteral("a"), [&] { ++writes; });
            pending.schedule(QStringLiteral("b"), [&] { ++writes; });
            pending.flush();
            QCOMPARE(writes, 3);
            pending.schedule(QStringLiteral("c"), [&] { ++writes; });
        }
        QCOMPARE(writes, 4); // owner teardown flushes without a live model
    }

    void icomSliderBurstFlushesLatestIntentOnDisconnect()
    {
        RadioModel radio;
        RadioModelWakeTestAccess::identity(radio, QStringLiteral("icom"), QStringLiteral("icom:batch"));
        const RadioSettingsScope scope = radio.settingsScope();
        QVERIFY(scope.setFeature(QStringLiteral("SquelchIntent"), 1,
            {{QStringLiteral("manualLevel"), 10}, {QStringLiteral("autoEnabled"), false}}));
        SliceModel slice(0);
        RxApplet rx;
        rx.setRadioModel(&radio);
        rx.setSlice(&slice);
        status(slice, true, 26, QStringLiteral("USB"));
        for (int level = 30; level < 100; ++level) {
            rx.setSqlSliderValueExternal(level);
        }
        QCOMPARE(scope.featureExact(QStringLiteral("SquelchIntent"))
                     .value(QStringLiteral("manualLevel")).toInt(), 10);
        radio.connectionStateChanged(false);
        QCOMPARE(scope.featureExact(QStringLiteral("SquelchIntent"))
                     .value(QStringLiteral("manualLevel")).toInt(), 99);
    }

    void squelchReadbackValidityExcludesOptimisticState()
    {
        SliceModel slice(0);
        QVERIFY(!slice.squelchStateKnown());
        slice.setSquelch(true, 26);
        QVERIFY(!slice.squelchStateKnown());
        SliceDelta on;
        on.squelchOn = true;
        slice.applyChanges(on);
        QVERIFY(!slice.squelchStateKnown());
        SliceDelta level;
        level.squelchLevel = 26;
        slice.applyChanges(level);
        QVERIFY(slice.squelchStateKnown());
        slice.setSquelch(true, 30);
        QVERIFY(!slice.squelchStateKnown());
        level.squelchLevel = 30;
        slice.applyChanges(level);
        QVERIFY(slice.squelchStateKnown());
        slice.invalidateSquelchState();
        QVERIFY(!slice.squelchStateKnown());
    }

    void icomReattachUsesCurrentSessionReport()
    {
        RadioModel radio;
        RadioModelWakeTestAccess::identity(radio, QStringLiteral("icom"), QStringLiteral("icom:reattach"));
        SliceModel slice(0);
        status(slice, true, 26, QStringLiteral("USB"));
        QVERIFY(slice.squelchStateKnown());
        RxApplet rx;
        rx.setRadioModel(&radio);
        QSignalSpy commands(&slice, &SliceModel::receiveSquelchRequested);
        rx.setSlice(&slice);
        QCOMPARE(rx.sqlMode(), RxApplet::SqlMode::Manual);
        QCOMPARE(rx.sqlManualLevel(), 26);
        rx.cycleSqlModeExternal();
        status(slice, true, slice.squelchLevel()); // confirm the explicit Auto threshold
        rx.setSlice(nullptr);
        commands.clear();
        rx.setSlice(&slice);
        QCOMPARE(rx.sqlMode(), RxApplet::SqlMode::Auto);
        QVERIFY(commands.isEmpty());
        rx.setSlice(nullptr);
        slice.invalidateSquelchState(); // reclaimed model from a disconnected session
        rx.setSlice(&slice);
        QCOMPARE(rx.sqlMode(), RxApplet::SqlMode::Off);
        QVERIFY(!slice.squelchStateKnown());
        status(slice, false, 0);
        QCOMPARE(rx.sqlMode(), RxApplet::SqlMode::Off);
    }

    void icomClientIntentSurvivesRecreation_data()
    {
        QTest::addColumn<bool>("automatic");
        QTest::newRow("manual-memory-while-off") << false;
        QTest::newRow("explicit-auto") << true;
    }

    void icomClientIntentSurvivesRecreation()
    {
        QFETCH(bool, automatic);
        RadioModel radio;
        RadioModelWakeTestAccess::identity(radio, QStringLiteral("icom"),
            automatic ? QStringLiteral("icom:auto-test") : QStringLiteral("icom:off-test"));
        {
            SliceModel first(0);
            RxApplet rx;
            rx.setRadioModel(&radio);
            rx.setSlice(&first);
            status(first, true, 26, QStringLiteral("USB"));
            rx.cycleSqlModeExternal(); // Manual -> Auto
            first.setSquelch(true, 8); // algorithm changes threshold, not manual choice
            status(first, true, 8);
            if (!automatic) {
                rx.cycleSqlModeExternal(); // Auto -> Off
                status(first, false, 0);
            }
            QCOMPARE(first.manualSquelchLevel(), 26);
            rx.setSlice(nullptr);
        }
        SliceModel second(0);
        RxApplet restarted;
        restarted.setRadioModel(&radio);
        QSignalSpy commands(&second, &SliceModel::receiveSquelchRequested);
        QSignalSpy algorithm(&restarted, &RxApplet::sqlAutoChanged);
        restarted.setSlice(&second);
        QCOMPARE(restarted.sqlMode(), RxApplet::SqlMode::Off);
        for (const auto& args : algorithm) { QVERIFY(!args[0].toBool()); }
        QVERIFY(commands.isEmpty()); // no attach/default threshold replay
        status(second, automatic, automatic ? 8 : 0, QStringLiteral("USB"));
        QCOMPARE(restarted.sqlMode(), automatic ? RxApplet::SqlMode::Auto : RxApplet::SqlMode::Off);
        QCOMPARE(second.manualSquelchLevel(), 26);
        QVERIFY(commands.isEmpty()); // adopting state is passive
        if (automatic) {
            restarted.cycleSqlModeExternal(); // Auto -> Off
            status(second, false, 0);
        }
        restarted.cycleSqlModeExternal(); // Off -> Manual is explicit intent
        QCOMPARE(second.squelchLevel(), 26);
        QCOMPARE(restarted.sqlManualLevel(), 26);
        QCOMPARE(control<QSlider>(restarted, QStringLiteral("Squelch threshold"))->value(), 26);
    }

    void icomRadioTruthAndScopeWin()
    {
        RadioModel radio;
        RadioModelWakeTestAccess::identity(radio, QStringLiteral("icom"), QStringLiteral("icom:authority"));
        QVERIFY(radio.settingsScope().setFeature(QStringLiteral("SquelchIntent"), 1,
            {{QStringLiteral("manualLevel"), 26}, {QStringLiteral("autoEnabled"), false}}));
        SliceModel slice(0);
        RxApplet rx;
        rx.setRadioModel(&radio);
        rx.setSlice(&slice);
        QSignalSpy commands(&slice, &SliceModel::receiveSquelchRequested);
        status(slice, true, 43, QStringLiteral("USB"));
        QCOMPARE(rx.sqlManualLevel(), 43); // fresh radio value overrides saved cache
        QVERIFY(commands.isEmpty());
        rx.cycleSqlModeExternal(); // saves Auto and current manual 43
        rx.setSlice(nullptr);
        RadioModelWakeTestAccess::identity(radio, QStringLiteral("icom"), QStringLiteral("icom:other"));
        SliceModel other(0);
        rx.setSlice(&other);
        status(other, true, 17, QStringLiteral("USB"));
        QCOMPARE(rx.sqlMode(), RxApplet::SqlMode::Manual);
        QCOMPARE(rx.sqlManualLevel(), 17); // neither Auto nor manual leaks to another radio
        rx.setSlice(nullptr);
        RadioModelWakeTestAccess::identity(radio, QStringLiteral("icom"), QStringLiteral("icom:authority"));
        SliceModel off(0);
        rx.setSlice(&off);
        status(off, false, 0, QStringLiteral("USB"));
        QCOMPARE(rx.sqlMode(), RxApplet::SqlMode::Off); // radio changed while disconnected
        QCOMPARE(rx.sqlManualLevel(), 43);
        rx.setSlice(nullptr); // disconnect flushes the bounded pending write
        QVERIFY(!radio.settingsScope().featureExact(QStringLiteral("SquelchIntent"))
                     .value(QStringLiteral("autoEnabled")).toBool());
    }

    void icomFutureIntentIsPreserved()
    {
        RadioModel radio;
        RadioModelWakeTestAccess::identity(radio, QStringLiteral("icom"), QStringLiteral("icom:future"));
        const QJsonObject future{{QStringLiteral("manualLevel"), 81}, {QStringLiteral("autoEnabled"), true}};
        QVERIFY(radio.settingsScope().setFeature(QStringLiteral("SquelchIntent"), 2, future));
        SliceModel slice(0);
        RxApplet rx;
        rx.setRadioModel(&radio);
        rx.setSlice(&slice);
        status(slice, true, 39, QStringLiteral("USB"));
        QCOMPARE(rx.sqlMode(), RxApplet::SqlMode::Manual);
        QCOMPARE(rx.sqlManualLevel(), 39);
        rx.cycleSqlModeExternal();
        rx.setSlice(nullptr); // exercise the deferred write before checking protection
        int version = 0;
        QCOMPARE(radio.settingsScope().featureExact(QStringLiteral("SquelchIntent"), &version), future);
        QCOMPARE(version, 2);
    }

    void bandRestore_data()
    {
        QTest::addColumn<bool>("vfoFirst");
        QTest::newRow("applet-connected-first") << false;
        QTest::newRow("vfo-connected-first") << true;
    }

    void bandRestore()
    {
        QFETCH(bool, vfoFirst);
        SliceModel slice(0);
        status(slice, true, 26, QStringLiteral("USB"));
        RxApplet rx;
        VfoWidget vfo;
        if (vfoFirst) {
            vfo.setSlice(&slice);
            rx.setSlice(&slice);
        } else {
            rx.setSlice(&slice);
            vfo.setSlice(&slice);
        }
        vfo.setRxApplet(&rx);
        QSlider* slider = control<QSlider>(rx, QStringLiteral("Squelch threshold"));
        QSlider* mirror = control<QSlider>(vfo, QStringLiteral("Squelch threshold"));
        QVERIFY(slider);
        QVERIFY(mirror);
        QSignalSpy commands(&slice, &SliceModel::receiveSquelchRequested);
        QSignalSpy line(&rx, &RxApplet::squelchStateChanged);

        // Ordered mode/off burst captured on FLEX-8400M fw 4.2.18.41174
        // (#5501), including the disabled CW SQL surface between bands.
        status(slice, false, 20, QStringLiteral("LSB"));
        status(slice, true, 20, QStringLiteral("CW"));
        status(slice, false, 20, QStringLiteral("USB"));
        status(slice, true, 26);
        QCOMPARE(slice.squelchLevel(), 26);
        QCOMPARE(slice.manualSquelchLevel(), 26);
        QCOMPARE(rx.sqlMode(), RxApplet::SqlMode::Manual);
        QCOMPARE(slider->value(), 26);
        QCOMPARE(mirror->value(), 26);
        QCOMPARE(line.last().at(0).toBool(), true);
        QCOMPARE(line.last().at(1).toInt(), 26);
        QVERIFY(commands.isEmpty());

        status(slice, true, 26); // repeated truth stays passive
        QVERIFY(commands.isEmpty());

        // A later real SQL-button cycle must replay the adopted 26.
        QPushButton* button = control<QPushButton>(rx, QStringLiteral("Squelch mode"));
        QVERIFY(button);
        button->click(); // Manual -> Auto
        button->click(); // Auto -> Off
        status(slice, false, 20); // a different off-state level is not intent
        commands.clear();
        button->click(); // Off -> Manual
        QCOMPARE(slice.squelchLevel(), 26);
        QCOMPARE(slice.manualSquelchLevel(), 26);
        QCOMPARE(commands.count(), 1);
        const SliceSquelchRequest request = qvariant_cast<SliceSquelchRequest>(commands.at(0).at(0));
        QVERIFY(request.enabled && request.enabledChanged && request.levelChanged);
        QCOMPARE(request.level, 26);
    }

    void splitStatus_data()
    {
        QTest::addColumn<bool>("levelFirst");
        QTest::newRow("level-before-enable") << true;
        QTest::newRow("enable-before-level") << false;
    }

    void splitStatus()
    {
        QFETCH(bool, levelFirst);
        SliceModel slice(0);
        status(slice, false, 20, QStringLiteral("USB"));
        RxApplet rx;
        rx.setSlice(&slice);
        QSignalSpy commands(&slice, &SliceModel::receiveSquelchRequested);
        SliceDelta level;
        level.squelchLevel = 37;
        SliceDelta enabled;
        enabled.squelchOn = true;
        slice.applyChanges(levelFirst ? level : enabled);
        slice.applyChanges(levelFirst ? enabled : level);
        QCOMPARE(slice.squelchLevel(), 37);
        QCOMPARE(slice.manualSquelchLevel(), 37);
        QCOMPARE(rx.sqlMode(), RxApplet::SqlMode::Manual);
        QSlider* slider = control<QSlider>(rx, QStringLiteral("Squelch threshold"));
        QVERIFY(slider);
        QCOMPARE(slider->value(), 37);
        QVERIFY(commands.isEmpty());
    }

    void autoKeepsManualChoice()
    {
        AppSettings::instance().setValue("AutoSqlMarginDb", "10");
        SliceModel slice(0);
        status(slice, true, 45, QStringLiteral("USB"));
        RxApplet rx;
        rx.setSlice(&slice);
        QSlider* slider = control<QSlider>(rx, QStringLiteral("Squelch threshold"));
        QVERIFY(slider);
        rx.cycleSqlModeExternal(); // Manual -> Auto
        slice.setSquelch(true, 8); // production algorithm's entry point
        QSignalSpy commands(&slice, &SliceModel::receiveSquelchRequested);
        status(slice, true, 8);
        QCOMPARE(rx.sqlMode(), RxApplet::SqlMode::Auto);
        QCOMPARE(slider->value(), 10);
        QCOMPARE(slice.manualSquelchLevel(), 45);
        QVERIFY(commands.isEmpty());

        rx.cycleSqlModeExternal(); // Auto -> Off
        commands.clear();
        // A late threshold-only update and the Off echo must not replace
        // manual memory with an Auto threshold or margin (#4604).
        SliceDelta lateLevel;
        lateLevel.squelchLevel = 8;
        slice.applyChanges(lateLevel);
        status(slice, false, 8);
        QCOMPARE(rx.sqlMode(), RxApplet::SqlMode::Off);
        QCOMPARE(slice.manualSquelchLevel(), 45);
        QVERIFY(commands.isEmpty());
        rx.cycleSqlModeExternal(); // Off -> Manual is operator intent
        QCOMPARE(slice.squelchLevel(), 45);
        QCOMPARE(slice.manualSquelchLevel(), 45);
        QCOMPARE(slider->value(), 45);
    }

    // #5859: the squelch button's own texts come back from the registry alone.
    // With no slice, updateModeSettings never runs, so RxApplet re-sets nothing.
    void squelchButtonTextsSurviveAvailability()
    {
        RadioModel radio;
        RxApplet rx;
        QPushButton* button = control<QPushButton>(rx, QStringLiteral("Squelch mode"));
        QVERIFY(button);
        const QString ownTip = button->toolTip();
        const QString ownDescription = button->accessibleDescription();
        QVERIFY(ownTip.startsWith(QStringLiteral("Click to cycle")));
        QVERIFY(!ownDescription.isEmpty());
        // Squelch is off, so the button is Inactive: the announcement is appended.
        const QString trimmed = ownDescription.trimmed();
        const QString inactive = trimmed
            + (trimmed.endsWith(u'.') ? QStringLiteral(" ") : QStringLiteral(". "))
            + QStringLiteral("Available, not currently active");
        rx.setRadioModel(&radio);
        QCOMPARE(button->toolTip(), ownTip);
        QCOMPARE(button->accessibleDescription(), inactive);

        RadioCapabilities exclusive;
        SquelchLevelScale scale;
        scale.modes = {QStringLiteral("FM")};
        scale.modesExclusive = true;
        exclusive.squelchLevelScale = scale;
        emit radio.capabilitiesChanged(true, exclusive);
        const QString reason = QStringLiteral("Squelch is unavailable in this receive mode");
        QVERIFY(!button->isEnabled());
        QCOMPARE(button->toolTip(), reason);
        QCOMPARE(button->accessibleDescription(), reason);

        emit radio.capabilitiesChanged(true, RadioCapabilities{});
        QVERIFY(button->isEnabled());
        QCOMPARE(button->toolTip(), ownTip);
        QCOMPARE(button->accessibleDescription(), inactive);
    }

    // #6092: a radio that publishes no squelch-to-pan mapping has no Auto SQL.
    void autoSkippedWhenUnavailable()
    {
        SliceModel slice(0);
        status(slice, false, 26, QStringLiteral("USB"));
        RxApplet rx;
        rx.setSlice(&slice);
        VfoWidget vfo;
        vfo.setSlice(&slice);
        vfo.setRxApplet(&rx);
        QSignalSpy algorithm(&rx, &RxApplet::sqlAutoChanged);
        rx.setAutoSqlAvailability(false, QStringLiteral("no squelch scale"));
        QVERIFY(!rx.autoSqlAvailable());
        rx.cycleSqlModeExternal(); // Off -> Manual
        QCOMPARE(rx.sqlMode(), RxApplet::SqlMode::Manual);
        rx.cycleSqlModeExternal(); // Manual -> Off: Auto is skipped
        QCOMPARE(rx.sqlMode(), RxApplet::SqlMode::Off);
        for (const auto& args : algorithm) { QVERIFY(!args[0].toBool()); }

        // Dimmed with a stated reason, not hidden: the button still cycles,
        // and both surfaces announce why Auto is missing.
        QPushButton* button = control<QPushButton>(rx, QStringLiteral("Squelch mode"));
        QVERIFY(button);
        QVERIFY(button->isVisibleTo(&rx) && button->isEnabled());
        QVERIFY(button->accessibleDescription().contains(QStringLiteral("no squelch scale")));
        QPushButton* mirror = control<QPushButton>(vfo, QStringLiteral("Squelch"));
        QVERIFY(mirror);
        QVERIFY(mirror->accessibleDescription().contains(QStringLiteral("no squelch scale")));

        rx.setAutoSqlAvailability(true, QString());
        QVERIFY(!button->accessibleDescription().contains(QStringLiteral("unavailable")));
        QVERIFY(!mirror->accessibleDescription().contains(QStringLiteral("unavailable")));
        rx.cycleSqlModeExternal(); // Off -> Manual
        rx.cycleSqlModeExternal(); // Manual -> Auto once it is available
        QCOMPARE(rx.sqlMode(), RxApplet::SqlMode::Auto);
    }

    void restoredAutoIntentLandsOnManualWhenUnavailable()
    {
        RadioModel radio;
        RadioModelWakeTestAccess::identity(radio, QStringLiteral("icom"),
                                           QStringLiteral("icom:auto-unavailable"));
        QVERIFY(radio.settingsScope().setFeature(QStringLiteral("SquelchIntent"), 1,
            {{QStringLiteral("manualLevel"), 26}, {QStringLiteral("autoEnabled"), true}}));
        SliceModel slice(0);
        RxApplet rx;
        rx.setRadioModel(&radio);
        rx.setAutoSqlAvailability(false, QStringLiteral("no squelch scale"));
        QSignalSpy algorithm(&rx, &RxApplet::sqlAutoChanged);
        rx.setSlice(&slice);
        status(slice, true, 8, QStringLiteral("USB")); // the saved Auto intent's echo
        QCOMPARE(rx.sqlMode(), RxApplet::SqlMode::Manual);
        for (const auto& args : algorithm) { QVERIFY(!args[0].toBool()); }
    }

    void autoDropsToManualWhenItBecomesUnavailable()
    {
        AppSettings::instance().setValue("AutoSqlMarginDb", "10");
        SliceModel slice(0);
        status(slice, true, 45, QStringLiteral("USB"));
        RxApplet rx;
        rx.setSlice(&slice);
        rx.cycleSqlModeExternal(); // Manual -> Auto
        QCOMPARE(rx.sqlMode(), RxApplet::SqlMode::Auto);
        QSignalSpy algorithm(&rx, &RxApplet::sqlAutoChanged);
        rx.setAutoSqlAvailability(false, QStringLiteral("no squelch scale"));
        // Squelch stays on at the operator's manual threshold.
        QCOMPARE(rx.sqlMode(), RxApplet::SqlMode::Manual);
        QVERIFY(slice.squelchOn());
        QCOMPARE(slice.squelchLevel(), 45);
        QCOMPARE(algorithm.count(), 1);
        QVERIFY(!algorithm.first()[0].toBool());
    }

    void fullOnReportAfterAuto_data()
    {
        QTest::addColumn<bool>("operatorTurnsOff");
        QTest::newRow("operator-off-late-auto-report") << true;
        QTest::newRow("radio-off-then-on-report") << false;
    }

    void fullOnReportAfterAuto()
    {
        QFETCH(bool, operatorTurnsOff);
        SliceModel slice(0);
        status(slice, true, 45, QStringLiteral("USB"));
        RxApplet rx;
        rx.setSlice(&slice);
        VfoWidget vfo;
        vfo.setSlice(&slice);
        vfo.setRxApplet(&rx);
        QSlider* slider = control<QSlider>(rx, QStringLiteral("Squelch threshold"));
        QSlider* mirror = control<QSlider>(vfo, QStringLiteral("Squelch threshold"));
        QVERIFY(slider);
        QVERIFY(mirror);

        rx.cycleSqlModeExternal(); // Manual -> Auto
        slice.setSquelch(true, 8); // production algorithm's entry point
        if (operatorTurnsOff) {
            rx.cycleSqlModeExternal(); // requests Off with manual 45
        } else {
            status(slice, false, 8); // radio-driven Off retains computed 8
        }
        QCOMPARE(rx.sqlMode(), RxApplet::SqlMode::Off);
        QCOMPARE(slice.manualSquelchLevel(), 45);
        QSignalSpy commands(&slice, &SliceModel::receiveSquelchRequested);

        // A full on-report has no provenance that distinguishes a delayed
        // Auto echo from a radio restore. Reconcile to its reported state
        // (Principle II), including the manual cache, without writing back.
        status(slice, true, 8);
        QCOMPARE(rx.sqlMode(), RxApplet::SqlMode::Manual);
        QCOMPARE(slice.squelchLevel(), 8);
        QCOMPARE(slice.manualSquelchLevel(), 8);
        QCOMPARE(slider->value(), 8);
        QCOMPARE(mirror->value(), 8);
        QVERIFY(commands.isEmpty());

        if (operatorTurnsOff) {
            // The later acknowledgement of the operator's Off/45 request
            // supersedes that report and restores the retained manual value.
            status(slice, false, 45);
            QCOMPARE(rx.sqlMode(), RxApplet::SqlMode::Off);
            QCOMPARE(slice.squelchLevel(), 45);
            QCOMPARE(slice.manualSquelchLevel(), 45);
            QVERIFY(commands.isEmpty());
            }
    }

    void detachedSlicesRemainIndependent()
    {
        SliceModel first(0);
        SliceModel second(1);
        status(first, false, 20, QStringLiteral("USB"));
        status(second, true, 63, QStringLiteral("USB"));
        RxApplet rx;
        rx.setSlice(&first); // closes first's manual-echo gate
        rx.setSlice(&second); // production detach reopens it
        QSignalSpy firstCommands(&first, &SliceModel::receiveSquelchRequested);
        QSignalSpy secondCommands(&second, &SliceModel::receiveSquelchRequested);
        status(first, true, 37);
        QCOMPARE(first.manualSquelchLevel(), 37);
        QCOMPARE(second.manualSquelchLevel(), 63);
        QCOMPARE(rx.sqlManualLevel(), 63);
        rx.setSlice(&first);
        QCOMPARE(rx.sqlManualLevel(), 37);
        rx.setSlice(nullptr);
        status(first, true, 48);
        QCOMPARE(first.manualSquelchLevel(), 48);
        QVERIFY(firstCommands.isEmpty());
        QVERIFY(secondCommands.isEmpty());
    }

    void externalReceiveDoesNotOverwriteFlexMemory()
    {
        SliceModel slice(0);
        status(slice, true, 45, QStringLiteral("USB"));
        slice.setExternalReceiveAudioReplacementMute(true);
        slice.setManualSquelch(false, 17);
        RxApplet rx;
        rx.setSlice(&slice);
        QSignalSpy commands(&slice, &SliceModel::receiveSquelchRequested);
        slice.setManualSquelch(true, 71);
        QCOMPARE(rx.sqlMode(), RxApplet::SqlMode::Manual);
        QCOMPARE(rx.sqlManualLevel(), 71);
        QCOMPARE(slice.manualSquelchLevel(), 45);
        status(slice, true, 33); // underlying Flex state is hidden by Kiwi
        QCOMPARE(rx.sqlManualLevel(), 71);
        QCOMPARE(slice.manualSquelchLevel(), 45);
        QVERIFY(commands.isEmpty());
    }

    void digitalOverrideResetsOnDetach()
    {
        SliceModel first(0);
        SliceModel second(1);
        status(first, true, 45, QStringLiteral("USB"));
        status(second, false, 20, QStringLiteral("USB"));
        RxApplet rx;
        rx.setSlice(&first);
        QSignalSpy firstCommands(&first, &SliceModel::receiveSquelchRequested);
        SliceDelta digital;
        digital.mode = QStringLiteral("DIGU");
        first.applyChanges(digital);
        QVERIFY(!first.squelchOn());
        QVERIFY(!firstCommands.isEmpty()); // existing paired override
        QSignalSpy secondCommands(&second, &SliceModel::receiveSquelchRequested);
        rx.setSlice(&second);
        QCOMPARE(rx.sqlMode(), RxApplet::SqlMode::Off);
        QVERIFY(!second.squelchOn());
        QVERIFY(secondCommands.isEmpty()); // no orphaned restore (#3268)
    }

    void combinedModeStatusUsesIncomingMode()
    {
        SliceModel slice(0);
        status(slice, false, 45, QStringLiteral("USB"));
        RxApplet rx;
        rx.setSlice(&slice);
        QSignalSpy commands(&slice, &SliceModel::receiveSquelchRequested);
        // The button still describes USB when this combined delta publishes
        // SQL, so eligibility must use the incoming model mode instead.
        status(slice, true, 19, QStringLiteral("CW"));
        QCOMPARE(rx.sqlMode(), RxApplet::SqlMode::Off);
        QCOMPARE(slice.manualSquelchLevel(), 45);
        QPushButton* button = control<QPushButton>(rx, QStringLiteral("Squelch mode"));
        QVERIFY(button);
        QVERIFY(!button->isEnabled());
        QVERIFY(commands.isEmpty());

        // The converse must adopt USB's manual value even though the button
        // is still disabled when the combined delta first publishes SQL.
        status(slice, true, 37, QStringLiteral("USB"));
        QCOMPARE(rx.sqlMode(), RxApplet::SqlMode::Manual);
        QCOMPARE(slice.manualSquelchLevel(), 37);
        QSlider* slider = control<QSlider>(rx, QStringLiteral("Squelch threshold"));
        QVERIFY(slider);
        QCOMPARE(slider->value(), 37);
        QVERIFY(button->isEnabled());
        QVERIFY(commands.isEmpty());
    }

    void digitalRoundTripWithSqlOffStaysOff()
    {
        // #3505's clarified starting state: Auto and SQL both off. This
        // pins the injected path only, not that issue's hardware outcome.
        SliceModel slice(0);
        status(slice, false, 20, QStringLiteral("USB"));
        RxApplet rx;
        rx.setSlice(&slice);
        QSignalSpy commands(&slice, &SliceModel::receiveSquelchRequested);
        SliceDelta mode;
        mode.mode = QStringLiteral("DIGU");
        slice.applyChanges(mode);
        mode.mode = QStringLiteral("USB");
        slice.applyChanges(mode);
        QCOMPARE(rx.sqlMode(), RxApplet::SqlMode::Off);
        QVERIFY(!slice.squelchOn());
        QVERIFY(commands.isEmpty());
    }
};

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("rx-squelch-reconciliation"));
    if (!profile.isValid()) {
        return 1;
    }
    QApplication app(argc, argv);
    AppSettings::instance().load();
    RxAppletSquelchReconciliationTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "rx_applet_squelch_reconciliation_test.moc"
