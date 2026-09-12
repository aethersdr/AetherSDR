#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/backends/SliceDelta.h"
#include "gui/RxApplet.h"
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
struct RadioModelWakeTestAccess {
    static void identity(RadioModel& radio, const QString& family, const QString& id)
    {
        radio.m_family = family;
        radio.m_lastInfo.serial = id;
    }
};
}

namespace {

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
        QSignalSpy commands(&slice, &SliceModel::commandReady);
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
        QSignalSpy commands(&second, &SliceModel::commandReady);
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
        QSignalSpy commands(&slice, &SliceModel::commandReady);
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
        QSignalSpy commands(&slice, &SliceModel::commandReady);
        QSignalSpy line(&rx, &RxApplet::squelchStateChanged);
        QSignalSpy intents(&slice, &SliceModel::squelchCommandIssued);

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
        QVERIFY(intents.isEmpty());

        status(slice, true, 26); // repeated truth stays passive
        QVERIFY(commands.isEmpty());
        QVERIFY(intents.isEmpty());

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
        QCOMPARE(commands.count(), 2);
        QCOMPARE(commands.at(1).at(0).toString(),
                 QStringLiteral("slice set 0 squelch_level=26"));
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
        QSignalSpy commands(&slice, &SliceModel::commandReady);
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
        QSignalSpy commands(&slice, &SliceModel::commandReady);
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
        QSignalSpy commands(&slice, &SliceModel::commandReady);
        QSignalSpy intents(&slice, &SliceModel::squelchCommandIssued);

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
        QVERIFY(intents.isEmpty());

        if (operatorTurnsOff) {
            // The later acknowledgement of the operator's Off/45 request
            // supersedes that report and restores the retained manual value.
            status(slice, false, 45);
            QCOMPARE(rx.sqlMode(), RxApplet::SqlMode::Off);
            QCOMPARE(slice.squelchLevel(), 45);
            QCOMPARE(slice.manualSquelchLevel(), 45);
            QVERIFY(commands.isEmpty());
            QVERIFY(intents.isEmpty());
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
        QSignalSpy firstCommands(&first, &SliceModel::commandReady);
        QSignalSpy secondCommands(&second, &SliceModel::commandReady);
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
        QSignalSpy commands(&slice, &SliceModel::commandReady);
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
        QSignalSpy firstCommands(&first, &SliceModel::commandReady);
        SliceDelta digital;
        digital.mode = QStringLiteral("DIGU");
        first.applyChanges(digital);
        QVERIFY(!first.squelchOn());
        QVERIFY(!firstCommands.isEmpty()); // existing paired override
        QSignalSpy secondCommands(&second, &SliceModel::commandReady);
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
        QSignalSpy commands(&slice, &SliceModel::commandReady);
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
        QSignalSpy commands(&slice, &SliceModel::commandReady);
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
