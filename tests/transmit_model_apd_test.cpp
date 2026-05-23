// Unit tests for External APD plumbing on TransmitModel — covers status
// parsing, sampler-port commands, equalizer reset, and idempotency.

#include "models/TransmitModel.h"

#include <QCoreApplication>
#include <QSignalSpy>

#include <cstdio>

using namespace MasterSDR;

namespace {

int g_failed = 0;

void report(const char* name, bool ok)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", name);
    if (!ok) ++g_failed;
}

QMap<QString, QString> kvs(std::initializer_list<std::pair<QString, QString>> pairs)
{
    QMap<QString, QString> m;
    for (const auto& p : pairs) m.insert(p.first, p.second);
    return m;
}

void testSamplerStatusPopulatesAvailableAndSelected()
{
    TransmitModel tx;
    QSignalSpy spy(&tx, &TransmitModel::apdSamplerChanged);

    tx.applyApdSamplerStatus(kvs({
        {"tx_ant", "ANT1"},
        {"selected_sampler", "RX_A"},
        {"valid_samplers", "RX_A,XVTA"},
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
    tx.applyApdSamplerStatus(kvs({
        {"tx_ant", "ANT2"},
        {"selected_sampler", "BOGUS"},
        {"valid_samplers", "RX_B"},
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
    tx.applyApdStatus(kvs({{"equalizer_active", "1"}}));
    report("equalizer_active goes true", tx.apdEqualizerActive());

    QSignalSpy resetSpy(&tx, &TransmitModel::apdEqualizerResetReceived);
    QSignalSpy stateSpy(&tx, &TransmitModel::apdStateChanged);

    // Bare flag form: dispatcher merges "equalizer_reset" into kvs as
    // an empty-valued key.
    tx.applyApdStatus(kvs({{"equalizer_reset", ""}}));

    report("apdEqualizerResetReceived emitted", resetSpy.count() == 1);
    report("equalizer_active clears on reset", !tx.apdEqualizerActive());
    report("apdStateChanged emitted on reset", stateSpy.count() >= 1);
}

void testConfigurableEnablesUiVisibility()
{
    TransmitModel tx;
    QSignalSpy spy(&tx, &TransmitModel::apdStateChanged);

    report("apdConfigurable starts false", !tx.apdConfigurable());
    tx.applyApdStatus(kvs({{"configurable", "1"}}));
    report("configurable=1 flips apdConfigurable", tx.apdConfigurable());
    report("apdStateChanged emitted", spy.count() >= 1);

    tx.applyApdStatus(kvs({{"configurable", "0"}}));
    report("configurable=0 turns it back off", !tx.apdConfigurable());
}

void testSamplerStatusIdempotent()
{
    TransmitModel tx;

    tx.applyApdSamplerStatus(kvs({
        {"tx_ant", "ANT1"},
        {"selected_sampler", "RX_A"},
        {"valid_samplers", "RX_A,XVTA"},
    }));

    QSignalSpy spy(&tx, &TransmitModel::apdSamplerChanged);
    // Re-apply identical status — must not re-emit.
    tx.applyApdSamplerStatus(kvs({
        {"tx_ant", "ANT1"},
        {"selected_sampler", "RX_A"},
        {"valid_samplers", "RX_A,XVTA"},
    }));
    report("identical sampler status is idempotent", spy.count() == 0);
}

void testResetStateClearsSamplers()
{
    TransmitModel tx;
    tx.applyApdSamplerStatus(kvs({
        {"tx_ant", "ANT1"},
        {"selected_sampler", "RX_A"},
        {"valid_samplers", "RX_A"},
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
