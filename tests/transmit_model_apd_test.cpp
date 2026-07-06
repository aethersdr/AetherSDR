// Unit tests for External APD plumbing on TransmitModel — covers status
// parsing, sampler-port commands, equalizer reset, and idempotency.

#include "models/TransmitModel.h"

#include <QCoreApplication>
#include <QSignalSpy>

#include <cstdio>

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
    QSignalSpy cmds(&tx, &TransmitModel::commandReady);

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
    QSignalSpy cmds(&tx, &TransmitModel::commandReady);

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

    if (g_failed == 0) {
        std::printf("\nAll APD tests passed.\n");
        return 0;
    }
    std::printf("\n%d test(s) FAILED.\n", g_failed);
    return 1;
}
