// ControlActionRegistry: one declaration publishes to keyboard and MIDI.
// Socket-free; no radio, no MIDI device, no settings file is touched.

#include "gui/ControlActionRegistry.h"
#include "core/ShortcutManager.h"

#include <QtTest>

using namespace AetherSDR;

namespace {

void publish(const ControlActionRegistry& reg, ShortcutManager& sm)
{
    reg.publishShortcuts([&](const ControlActionShortcutSpec& spec) {
        sm.registerAction(spec.id, spec.name, spec.category, spec.defaultKey, spec.handler,
                          spec.autoRepeat);
    });
}

const ShortcutManager::Action* find(const ShortcutManager& m, const QString& id)
{
    for (const auto& a : m.actions())
        if (a.id == id) return &a;
    return nullptr;
}

} // namespace

class ControlActionRegistryTest : public QObject {
    Q_OBJECT

private slots:
    void triggerPublishesToBothSurfaces()
    {
        ControlActionRegistry reg;
        int fired = 0;
        QVERIFY(reg.add(ControlAction::trigger("band_up", "global.bandUp", "Band Up", "Band",
                                               [&](float) { ++fired; })
                            .inMidiCategory("Global")));
        ShortcutManager sm;
        publish(reg, sm);
        const auto* a = find(sm, "band_up");
        QVERIFY(a);
        QCOMPARE(a->displayName, QStringLiteral("Band Up"));
        QCOMPARE(a->category, QStringLiteral("Band"));
        QVERIFY(!a->keysTx);
        a->handler();
        QCOMPARE(fired, 1);

        QVector<ControlActionMidiSpec> specs;
        reg.publishMidi([&](const ControlActionMidiSpec& s) { specs << s; });
        QCOMPARE(specs.size(), 1);
        QCOMPARE(specs[0].id, QStringLiteral("global.bandUp"));
        QCOMPARE(specs[0].category, QStringLiteral("Global"));
        QCOMPARE(specs[0].kind, ControlAction::Kind::Trigger);
        specs[0].set(1.0f);
        QCOMPARE(fired, 2);
    }

    void toggleFlipsFromItsGetter()
    {
        ControlActionRegistry reg;
        float state = 0.0f;
        QVERIFY(reg.add(ControlAction::toggle("nr_toggle", "rx.nrEnable", "Noise Reduction", "DSP",
                                              [&](float v) { state = v; },
                                              [&] { return state; })));
        ShortcutManager sm;
        publish(reg, sm);
        const auto* a = find(sm, "nr_toggle");
        QVERIFY(a);
        a->handler();
        QCOMPARE(state, 1.0f);
        a->handler();
        QCOMPARE(state, 0.0f);
    }

    void levelGetsAClampedUpDownPair()
    {
        ControlActionRegistry reg;
        float level = 95.0f;
        QVERIFY(reg.add(ControlAction::level("rf_power", "tx.rfPower", "RF Power", "TX",
                                             0, 100, 5,
                                             [&](float v) { level = v; },
                                             [&] { return level; })));
        ShortcutManager sm;
        publish(reg, sm);
        QVERIFY(!find(sm, "rf_power"));
        const auto* up = find(sm, "rf_power_up");
        const auto* down = find(sm, "rf_power_down");
        QVERIFY(up && down);
        QCOMPARE(up->displayName, QStringLiteral("RF Power Up"));
        QVERIFY(up->autoRepeat);
        up->handler();
        QCOMPARE(level, 100.0f);
        up->handler();
        QCOMPARE(level, 100.0f);   // clamped at rangeMax
        level = 3.0f;
        down->handler();
        QCOMPARE(level, 0.0f);     // clamped at rangeMin

        QVector<ControlActionMidiSpec> specs;
        reg.publishMidi([&](const ControlActionMidiSpec& s) { specs << s; });
        QCOMPARE(specs.size(), 1);
        QCOMPARE(specs[0].kind, ControlAction::Kind::Level);
        QCOMPARE(specs[0].rangeMax, 100.0f);
        specs[0].set(42.0f);
        QCOMPARE(level, 42.0f);
        QCOMPARE(specs[0].get(), 42.0f);
    }

    void unavailableActionRefusesOnEverySurface()
    {
        ControlActionRegistry reg;
        QStringList refused;
        reg.setRefusalHandler([&](const ControlAction& a) { refused << a.name; });
        bool available = false;
        int fired = 0;
        QVERIFY(reg.add(ControlAction::trigger("slice_set_tx", "global.sliceSetTx",
                                               "Make Active Slice TX", "Slice",
                                               [&](float) { ++fired; })
                            .when([&] { return available; })));
        ShortcutManager sm;
        publish(reg, sm);
        QVector<ControlActionMidiSpec> specs;
        reg.publishMidi([&](const ControlActionMidiSpec& s) { specs << s; });

        find(sm, "slice_set_tx")->handler();
        specs[0].set(1.0f);
        QCOMPARE(fired, 0);
        QCOMPARE(refused, (QStringList{"Make Active Slice TX", "Make Active Slice TX"}));

        available = true;
        find(sm, "slice_set_tx")->handler();
        specs[0].set(1.0f);
        QCOMPARE(fired, 2);
        QCOMPARE(refused.size(), 2);
    }

    void malformedDeclarationsAreRejected()
    {
        ControlActionRegistry reg;
        const auto noop = [](float) {};
        const auto zero = [] { return 0.0f; };
        // A dotted id would bind but never persist (Shortcut_<id> key).
        QVERIFY(!reg.add(ControlAction::trigger("filter.preset", {}, "x", "Filter", noop)));
        QVERIFY(!reg.add(ControlAction::trigger({}, {}, "no ids", "Filter", noop)));
        QVERIFY(!reg.add(ControlAction::trigger("no_setter", {}, "x", "Filter", {})));
        ControlAction t = ControlAction::trigger("needs_get", {}, "x", "Filter", noop);
        t.kind = ControlAction::Kind::Toggle;
        QVERIFY(!reg.add(t));

        QVERIFY(reg.add(ControlAction::trigger("a", "m.a", "A", "Filter", noop)));
        QVERIFY(!reg.add(ControlAction::trigger("a", "m.b", "dup key", "Filter", noop)));
        QVERIFY(!reg.add(ControlAction::trigger("b", "m.a", "dup midi", "Filter", noop)));
        // A level's generated _up id collides with an existing trigger.
        QVERIFY(reg.add(ControlAction::trigger("vol_up", {}, "Vol Up", "Audio", noop)));
        QVERIFY(!reg.add(ControlAction::level("vol", {}, "Vol", "Audio", 0, 1, 0.1f, noop, zero)));
        // ...and a trigger may not take a level's generated id.
        QVERIFY(reg.add(ControlAction::level("gain", {}, "Gain", "Audio", 0, 1, 0.1f, noop, zero)));
        QVERIFY(!reg.add(ControlAction::trigger("gain_down", {}, "x", "Audio", noop)));
        // Pairs reserve their ids too.
        reg.addPair("legacy_key", "legacy.midi");
        QVERIFY(!reg.add(ControlAction::trigger("legacy_key", {}, "x", "Audio", noop)));
        QVERIFY(!reg.add(ControlAction::trigger({}, "legacy.midi", "x", "Audio", noop)));

        QCOMPARE(reg.actions().size(), 3);
        QCOMPARE(reg.errors().size(), 10);
    }

    void pairsResolveBothWays()
    {
        ControlActionRegistry reg;
        const auto noop = [](float) {};
        reg.addPair("split_toggle", "global.splitToggle");
        QVERIFY(reg.add(ControlAction::trigger("band_up", "global.bandUp", "Band Up", "Band", noop)));
        QVERIFY(reg.add(ControlAction::level("rf_power", "tx.rfPower", "RF Power", "TX", 0, 100, 5,
                                             noop, [] { return 0.0f; })));
        QCOMPARE(reg.midiIdForShortcut("split_toggle"), QStringLiteral("global.splitToggle"));
        QCOMPARE(reg.shortcutIdForMidi("global.splitToggle"), QStringLiteral("split_toggle"));
        QCOMPARE(reg.shortcutIdForMidi("global.bandUp"), QStringLiteral("band_up"));
        // A level is a slider on MIDI and two keys on the keyboard, not one twin.
        QVERIFY(reg.shortcutIdForMidi("tx.rfPower").isEmpty());
        QVERIFY(reg.midiIdForShortcut("unknown").isEmpty());
    }

    void relativeEncoderStepsALevel()
    {
        ControlActionRegistry reg;
        float hz = 0.0f;
        bool available = true;
        QStringList refused;
        reg.setRefusalHandler([&](const ControlAction& a) { refused << a.name; });
        QVERIFY(reg.add(ControlAction::level("rit_offset", "rx.ritOffset", "RIT Offset", "RIT/XIT",
                                             -5000, 5000, 10,
                                             [&](float v) { hz = v; }, [&] { return hz; })
                            .when([&] { return available; })));
        QVERIFY(reg.add(ControlAction::trigger("t", "rx.trigger", "T", "DSP", [](float) {})));
        QVERIFY(reg.stepLevel("rx.ritOffset", 3));
        QCOMPARE(hz, 30.0f);
        QVERIFY(reg.stepLevel("rx.ritOffset", -1000));
        QCOMPARE(hz, -5000.0f);                       // clamped
        QVERIFY(!reg.stepLevel("rx.trigger", 1));     // not a level
        QVERIFY(!reg.stepLevel("rx.unknown", 1));
        available = false;
        QVERIFY(reg.stepLevel("rx.ritOffset", 1));    // handled, but refused
        QCOMPARE(hz, -5000.0f);
        QCOMPARE(refused, QStringList{"RIT Offset"});
    }

    void steppingHookTakesRelativeSteps()
    {
        ControlActionRegistry reg;
        float value = 0.0f;
        int stepped = 0;
        QVERIFY(reg.add(ControlAction::level({}, "rx.filterWidth", "Filter Width", "Filter",
                                             0, 100, 2,
                                             [&](float v) { value = v; }, [&] { return value; })
                            .stepping([&](int steps) { stepped += steps; })));
        QVERIFY(reg.stepLevel("rx.filterWidth", 3));
        QVERIFY(reg.stepLevel("rx.filterWidth", -1));
        QCOMPARE(stepped, 2);
        QCOMPARE(value, 0.0f);   // the hook replaces get() + steps * keyStep
    }

    void refusalCarriesTheAction()
    {
        ControlActionRegistry reg;
        bool txSetup = false;
        reg.setRefusalHandler([&](const ControlAction& a) { txSetup = a.txSetup; });
        QVERIFY(reg.add(ControlAction::trigger("tx_antenna_1", {}, "TX Antenna 1", "TX", [](float) {})
                            .when([] { return false; }).transmitSetup()));
        ShortcutManager sm;
        publish(reg, sm);
        find(sm, "tx_antenna_1")->handler();
        QVERIFY(txSetup);
    }

    void everyPublishedShortcutIdIsPersistable()
    {
        ControlActionRegistry reg;
        const auto noop = [](float) {};
        QVERIFY(reg.add(ControlAction::level("mic_level", "phone.micLevel", "Mic", "TX", 0, 100, 5,
                                             noop, [] { return 0.0f; })));
        QVERIFY(reg.add(ControlAction::trigger("cw_keyer_iambic_a", "cw.keyerIambicA", "A", "CW", noop)));
        ShortcutManager sm;
        publish(reg, sm);
        for (const auto& a : sm.actions())
            QVERIFY2(ControlActionRegistry::isValidShortcutId(a.id), qPrintable(a.id));
    }
};

QTEST_MAIN(ControlActionRegistryTest)
#include "control_action_registry_test.moc"
