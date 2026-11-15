#include "ModelControlWireSpy.h"
// Unit tests for External APD plumbing on TransmitModel — covers status
// parsing, sampler-port commands, equalizer reset, and idempotency.

#include "models/TransmitModel.h"

#include <QCoreApplication>
#include <QSignalSpy>

#include <cstdio>
#include <optional>
#include <tuple>

using namespace AetherSDR;

namespace {

int g_failed = 0;

void report(const char* name, bool ok)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", name);
    if (!ok) ++g_failed;
}

// aetherd RFC 2.3: TransmitModel::applyChanges takes a typed TransmitDelta. The
// Flex wire decode (valid_samplers split + INTERNAL prepend, "1"→bool) lives in
// FlexBackend::decode*Status, covered by aetherd_transmit_decode_test; these
// tests exercise the model's apply side (map insert, selected fallback,
// idempotency, emits) via a pre-built delta.
template <class F>
TransmitDelta td(F&& build)
{
    TransmitDelta d;
    build(d);
    return d;
}

void testSamplerStatusPopulatesAvailableAndSelected()
{
    TransmitModel tx;
    QSignalSpy spy(&tx, &TransmitModel::apdSamplerChanged);

    tx.applyChanges(td([](TransmitDelta& d){
        d.apdSamplerTxAnt = QStringLiteral("ANT1");
        d.apdSamplerAvailable = QStringList{"INTERNAL", "RX_A", "XVTA"};
        d.apdSamplerSelected = QStringLiteral("RX_A");
    }));

    const auto s = tx.apdSampler("ANT1");
    report("sampler ANT1 selected = RX_A", s.selected == "RX_A");
    report("sampler ANT1 includes INTERNAL first",
        !s.available.isEmpty() && s.available.first() == "INTERNAL");
    report("sampler ANT1 contains RX_A and XVTA",
        s.available.contains("RX_A") && s.available.contains("XVTA"));
    report("apdSamplerChanged emitted once", spy.count() == 1);
}

void testSelectedFallbackWhenNotInValidList()
{
    TransmitModel tx;

    // selected_sampler isn't present in valid_samplers — FlexLib falls back
    // to INTERNAL rather than displaying a value the user can't pick.
    tx.applyChanges(td([](TransmitDelta& d){
        d.apdSamplerTxAnt = QStringLiteral("ANT2");
        d.apdSamplerAvailable = QStringList{"INTERNAL", "RX_B"};
        d.apdSamplerSelected = QStringLiteral("BOGUS");
    }));

    report("invalid selected_sampler falls back to INTERNAL",
        tx.apdSampler("ANT2").selected == "INTERNAL");
}

void testSetSamplerPortEmitsCommand()
{
    TransmitModel tx;
    ModelControlWireSpy cmds(&tx);

    tx.setApdSamplerPort("ANT2", "RX_B");
    report("setApdSamplerPort emits one command", cmds.count() == 1);
    report("command formatted correctly",
        cmds.first().first().toString() == "apd sampler tx_ant=ANT2 sample_port=RX_B");

    // Lowercase input is normalised to uppercase to match the radio's case.
    tx.setApdSamplerPort("xvta", "internal");
    report("sampler command upcases ant and port",
        cmds.last().first().toString() == "apd sampler tx_ant=XVTA sample_port=INTERNAL");

    // Empty inputs are ignored — no spurious command.
    int before = cmds.count();
    tx.setApdSamplerPort("", "RX_A");
    tx.setApdSamplerPort("ANT1", "");
    report("empty txAnt or port silently dropped", cmds.count() == before);
}

void testResetEqualizerEmitsCommand()
{
    TransmitModel tx;
    ModelControlWireSpy cmds(&tx);

    tx.resetApdEqualizer();
    report("resetApdEqualizer emits one command", cmds.count() == 1);
    report("reset command is 'apd reset'",
        cmds.first().first().toString() == "apd reset");
}

void testEqualizerResetStatusFlagClearsActive()
{
    TransmitModel tx;
    // Drive equalizer_active=1 first.
    tx.applyChanges(td([](TransmitDelta& d){ d.apdEqActive = true; }));
    report("equalizer_active goes true", tx.apdEqualizerActive());

    QSignalSpy resetSpy(&tx, &TransmitModel::apdEqualizerResetReceived);
    QSignalSpy stateSpy(&tx, &TransmitModel::apdStateChanged);

    // Bare flag form: dispatcher merges "equalizer_reset" into kvs as
    // an empty-valued key.
    tx.applyChanges(td([](TransmitDelta& d){ d.apdEqualizerReset = true; }));

    report("apdEqualizerResetReceived emitted", resetSpy.count() == 1);
    report("equalizer_active clears on reset", !tx.apdEqualizerActive());
    report("apdStateChanged emitted on reset", stateSpy.count() >= 1);
}

void testConfigurableEnablesUiVisibility()
{
    TransmitModel tx;
    QSignalSpy spy(&tx, &TransmitModel::apdStateChanged);

    report("apdConfigurable starts false", !tx.apdConfigurable());
    tx.applyChanges(td([](TransmitDelta& d){ d.apdConfigurable = true; }));
    report("configurable=1 flips apdConfigurable", tx.apdConfigurable());
    report("apdStateChanged emitted", spy.count() >= 1);

    tx.applyChanges(td([](TransmitDelta& d){ d.apdConfigurable = false; }));
    report("configurable=0 turns it back off", !tx.apdConfigurable());
}

void testSamplerStatusIdempotent()
{
    TransmitModel tx;

    tx.applyChanges(td([](TransmitDelta& d){
        d.apdSamplerTxAnt = QStringLiteral("ANT1");
        d.apdSamplerAvailable = QStringList{"INTERNAL", "RX_A", "XVTA"};
        d.apdSamplerSelected = QStringLiteral("RX_A");
    }));

    QSignalSpy spy(&tx, &TransmitModel::apdSamplerChanged);
    // Re-apply identical status — must not re-emit.
    tx.applyChanges(td([](TransmitDelta& d){
        d.apdSamplerTxAnt = QStringLiteral("ANT1");
        d.apdSamplerAvailable = QStringList{"INTERNAL", "RX_A", "XVTA"};
        d.apdSamplerSelected = QStringLiteral("RX_A");
    }));
    report("identical sampler status is idempotent", spy.count() == 0);
}

void testResetStateClearsSamplers()
{
    TransmitModel tx;
    tx.applyChanges(td([](TransmitDelta& d){
        d.apdSamplerTxAnt = QStringLiteral("ANT1");
        d.apdSamplerAvailable = QStringList{"INTERNAL", "RX_A"};
        d.apdSamplerSelected = QStringLiteral("RX_A");
    }));
    report("sampler set before reset", tx.apdSampler("ANT1").selected == "RX_A");

    tx.resetState();
    report("resetState clears sampler hash",
        tx.apdSampler("ANT1").selected == "INTERNAL"
        && tx.apdSampler("ANT1").available == QStringList{"INTERNAL"});
}

// A transmit model on ANT1 at 14.268 MHz, RF power 100, tune power 10.
void armTxSlice(TransmitModel& tx, double frequencyMhz = 14.268)
{
    tx.setApdTxSliceResolver([frequencyMhz] {
        return std::optional<TransmitModel::ApdTxSlice>(
            TransmitModel::ApdTxSlice{QStringLiteral("ANT1"), frequencyMhz});
    });
    tx.applyChanges(td([](TransmitDelta& d){ d.rfPower = 100; d.tunePower = 10; }));
}

TransmitDelta scopedEq(bool active, const QString& ant, double freq,
                       std::optional<int> rfpower = 100)
{
    return td([&](TransmitDelta& d){
        d.apdEqActive = active;
        d.apdEqAntenna = ant;
        d.apdEqFrequencyMhz = freq;
        d.apdEqRfPower = rfpower;
    });
}

void testUnscopedStatusStaysGlobal()
{
    // Firmware without ant/freq keeps today's behavior, resolver or not.
    TransmitModel tx;
    tx.applyChanges(td([](TransmitDelta& d){ d.apdEqActive = true; }));
    report("unscoped equalizer_active=1 applies without a TX slice", tx.apdEqualizerActive());
    tx.applyChanges(td([](TransmitDelta& d){ d.apdEqActive = false; }));
    report("unscoped equalizer_active=0 applies without a TX slice", !tx.apdEqualizerActive());
    report("lifecycle unreported by default", !tx.apdLifecycleState());
}

void testScopedStatusMatchesTxSlice()
{
    TransmitModel tx;
    armTxSlice(tx);

    tx.applyChanges(scopedEq(true, QStringLiteral("ANT2"), 14.268));
    report("other antenna does not apply", !tx.apdEqualizerActive());
    tx.applyChanges(scopedEq(true, QStringLiteral("ANT1"), 14.268001));
    report("frequency 1 Hz off does not apply", !tx.apdEqualizerActive());
    tx.applyChanges(scopedEq(true, QStringLiteral("ANT1"), 14.268, 40));
    report("other RF power does not apply", !tx.apdEqualizerActive());

    tx.applyChanges(scopedEq(true, QStringLiteral("ant1"), 14.268));
    report("matching antenna (any case), frequency and power applies", tx.apdEqualizerActive());

    QSignalSpy spy(&tx, &TransmitModel::apdStateChanged);
    tx.applyChanges(scopedEq(false, QStringLiteral("ANT1"), 7.19, 40));
    report("another slice's status leaves the indicator", tx.apdEqualizerActive());
    report("and emits nothing", spy.count() == 0);
}

void testScopedStatusSubHzSliceAndMissingPower()
{
    TransmitModel tx;
    armTxSlice(tx, 14.2680004);
    tx.applyChanges(scopedEq(true, QStringLiteral("ANT1"), 14.268));
    report("sub-Hz slice frequency matches the Hz-precision status", tx.apdEqualizerActive());

    tx.applyChanges(scopedEq(false, QStringLiteral("ANT1"), 14.268, std::nullopt));
    report("a status without rfpower matches on antenna and frequency", !tx.apdEqualizerActive());
}

void testScopedStatusUsesTunePowerWhileTuning()
{
    TransmitModel tx;
    armTxSlice(tx);
    tx.applyChanges(td([](TransmitDelta& d){ d.tune = true; }));

    tx.applyChanges(scopedEq(true, QStringLiteral("ANT1"), 14.268, 100));
    report("RF power does not match while tuning", !tx.apdEqualizerActive());
    tx.applyChanges(scopedEq(true, QStringLiteral("ANT1"), 14.268, 10));
    report("tune power matches while tuning", tx.apdEqualizerActive());
}

void testScopedStatusNeedsATxSlice()
{
    TransmitModel tx;
    tx.applyChanges(scopedEq(true, QStringLiteral("ANT1"), 14.268));
    report("no resolver: scoped status does not apply", !tx.apdEqualizerActive());

    tx.setApdTxSliceResolver([] { return std::optional<TransmitModel::ApdTxSlice>(); });
    tx.applyChanges(scopedEq(true, QStringLiteral("ANT1"), 14.268));
    report("no TX slice: scoped status does not apply", !tx.apdEqualizerActive());

    tx.applyChanges(td([](TransmitDelta& d){
        d.apdEqActive = true;
        d.apdEqAntenna = QStringLiteral("ANT1");
    }));
    report("antenna without frequency does not apply", !tx.apdEqualizerActive());
}

void testLifecycleDrivesIndicator()
{
    TransmitModel tx;
    armTxSlice(tx);
    tx.applyChanges(scopedEq(true, QStringLiteral("ANT1"), 14.268));
    report("pre-lifecycle equalizer_active=1 applies", tx.apdEqualizerActive());

    QSignalSpy spy(&tx, &TransmitModel::apdStateChanged);
    // apd ant=ANT1 freq=14.268000 rfpower=100 state=IDLE deployed=0
    tx.applyChanges(td([](TransmitDelta& d){
        d.apdLifecycle = ApdLifecycleState::Idle;
        d.apdEqAntenna = QStringLiteral("ANT1");
        d.apdEqFrequencyMhz = 14.268;
        d.apdEqRfPower = 100;
    }));
    report("lifecycle IDLE recorded", tx.apdLifecycleState() == ApdLifecycleState::Idle);
    report("IDLE takes the correction off the air", !tx.apdEqualizerActive());
    report("apdStateChanged emitted", spy.count() == 1);

    tx.applyChanges(scopedEq(true, QStringLiteral("ANT1"), 14.268));
    report("equalizer_active=1 no longer drives the indicator", !tx.apdEqualizerActive());
    tx.applyChanges(td([](TransmitDelta& d){ d.apdEqActive = true; }));
    report("nor does an unscoped equalizer_active=1", !tx.apdEqualizerActive());

    for (const auto [state, onAir, name] : {
             std::tuple{ApdLifecycleState::Training, false, "TRAINING is off the air"},
             std::tuple{ApdLifecycleState::Active, true, "ACTIVE is on the air"},
             std::tuple{ApdLifecycleState::Hold, true, "HOLD is on the air"},
             std::tuple{ApdLifecycleState::Fault, false, "FAULT is off the air"},
             std::tuple{ApdLifecycleState::Off, false, "OFF is off the air"}}) {
        tx.applyChanges(td([state = state](TransmitDelta& d){
            d.apdLifecycle = state;
            d.apdEqAntenna = QStringLiteral("ANT1");
            d.apdEqFrequencyMhz = 14.268;
            d.apdEqRfPower = 100;
        }));
        report(name, tx.apdEqualizerActive() == onAir);
    }

    // The lifecycle is radio-wide; only the indicator is scoped.
    tx.applyChanges(td([](TransmitDelta& d){
        d.apdLifecycle = ApdLifecycleState::Active;
        d.apdEqAntenna = QStringLiteral("ANT1");
        d.apdEqFrequencyMhz = 7.19;
        d.apdEqRfPower = 40;
    }));
    report("lifecycle follows a status for another slice",
        tx.apdLifecycleState() == ApdLifecycleState::Active);
    report("but the indicator does not", !tx.apdEqualizerActive());
    tx.applyChanges(scopedEq(false, QStringLiteral("ANT1"), 14.268));
    report("a matching status then shows ACTIVE on the air", tx.apdEqualizerActive());
}

void testResetStateForgetsLifecycle()
{
    TransmitModel tx;
    tx.applyChanges(td([](TransmitDelta& d){ d.apdLifecycle = ApdLifecycleState::Hold; }));
    tx.resetState();
    report("resetState forgets the lifecycle", !tx.apdLifecycleState());
    tx.applyChanges(td([](TransmitDelta& d){ d.apdEqActive = true; }));
    report("and the next radio's unscoped form applies again", tx.apdEqualizerActive());
    report("lifecycle names", TransmitModel::apdLifecycleStateName(ApdLifecycleState::Hold)
                                  == QStringLiteral("hold"));
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    testSamplerStatusPopulatesAvailableAndSelected();
    testSelectedFallbackWhenNotInValidList();
    testSetSamplerPortEmitsCommand();
    testResetEqualizerEmitsCommand();
    testEqualizerResetStatusFlagClearsActive();
    testConfigurableEnablesUiVisibility();
    testSamplerStatusIdempotent();
    testResetStateClearsSamplers();
    testUnscopedStatusStaysGlobal();
    testScopedStatusMatchesTxSlice();
    testScopedStatusSubHzSliceAndMissingPower();
    testScopedStatusUsesTunePowerWhileTuning();
    testScopedStatusNeedsATxSlice();
    testLifecycleDrivesIndicator();
    testResetStateForgetsLifecycle();

    if (g_failed == 0) {
        std::printf("\nAll APD tests passed.\n");
        return 0;
    }
    std::printf("\n%d test(s) FAILED.\n", g_failed);
    return 1;
}
