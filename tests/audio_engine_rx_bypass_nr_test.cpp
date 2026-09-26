// RX BYPASS must take the whole receive path down and put it back: the client
// chain stages AND whichever AetherNR method is running. Before #5913 the
// engine snapshot covered the chain plus RN2 only, so a "bypassed" receive
// path still ran NR4, DFNR, NNR, BNR, MNR or NR2. Socket-free and device-free:
// the enable flags and the snapshot are plain engine state.
#include "TestSettingsProfile.h"
#include "core/AudioEngine.h"
#include "core/ClientGate.h"
#include "core/ClientEq.h"

#include <QCoreApplication>
#include <QString>

#include <cstdio>
#include <functional>

using namespace AetherSDR;

namespace {

int failures = 0;

void check(bool ok, const char* what)
{
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}

struct Method {
    const char* name;
    std::function<void(AudioEngine&, bool)> set;
    std::function<bool(const AudioEngine&)> enabled;
};

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile settings(QStringLiteral("audio-engine-rx-bypass-nr"));
    QCoreApplication application(argc, argv);
    check(settings.isValid(), "isolated settings profile");

    AudioEngine engine;

    // The chain stage part of the snapshot, always available.
    check(engine.clientGateRx() != nullptr, "engine has an RX gate");
    check(engine.clientEqRx() != nullptr, "engine has an RX EQ");
    engine.clientGateRx()->setEnabled(true);
    engine.clientEqRx()->setEnabled(false);

    // Whichever NR method this build can actually run; the model-backed ones
    // refuse without their files, and NR2 needs the wisdom prep, so the
    // list is tried in order and the first that turns on is the subject.
    const Method methods[] = {
        {"NR4",  [](AudioEngine& e, bool on) { e.setNr4Enabled(on); },   [](const AudioEngine& e) { return e.nr4Enabled(); }},
        {"MNR",  [](AudioEngine& e, bool on) { e.setMnrEnabled(on); },   [](const AudioEngine& e) { return e.mnrEnabled(); }},
        {"RN2",  [](AudioEngine& e, bool on) { e.setRn2Enabled(on); },   [](const AudioEngine& e) { return e.rn2Enabled(); }},
        {"DFNR", [](AudioEngine& e, bool on) { e.setDfnrEnabled(on); },  [](const AudioEngine& e) { return e.dfnrEnabled(); }},
        {"NNR",  [](AudioEngine& e, bool on) { e.setNnrEnabled(on); },   [](const AudioEngine& e) { return e.nnrEnabled(); }},
        {"BNR",  [](AudioEngine& e, bool on) { e.setNvAfxEnabled(on); }, [](const AudioEngine& e) { return e.nvAfxEnabled(); }},
    };
    const Method* subject = nullptr;
    for (const Method& m : methods) {
        m.set(engine, true);
        if (m.enabled(engine)) { subject = &m; break; }
    }
    if (!subject) {
        std::printf("SKIP: no NR method can run in this build; chain-only check follows\n");
    } else {
        std::printf("subject NR method: %s\n", subject->name);
    }

    check(!engine.isRxBypassed(), "starts unbypassed");
    engine.setRxBypassed(true);
    check(engine.isRxBypassed(), "bypass engages");
    check(!engine.clientGateRx()->isEnabled(), "bypass switches the gate off");
    check(!engine.clientEqRx()->isEnabled(),   "EQ that was off stays off");
    if (subject) {
        check(!subject->enabled(engine), "bypass switches the running NR method off");
    }
    for (const Method& m : methods) {
        if (m.enabled(engine)) { check(false, "no NR method runs while bypassed"); break; }
    }

    // A method selection after engage must not restart DSP behind BYPASS.
    // Try every built method: disabled build-time stubs are harmless here.
    for (const Method& m : methods) {
        m.set(engine, true);
        check(!m.enabled(engine), "NR enable cannot escape active bypass");
    }

    engine.setRxBypassed(false);
    check(!engine.isRxBypassed(), "bypass releases");
    check(engine.clientGateRx()->isEnabled(), "release restores the gate");
    check(!engine.clientEqRx()->isEnabled(),  "release leaves the EQ off, as it was");
    if (subject) {
        check(subject->enabled(engine), "release restores the original NR method after rejected changes");
        int running = 0;
        for (const Method& m : methods) running += m.enabled(engine) ? 1 : 0;
        check(running == 1, "exactly one NR method runs after release");
    }

    // A second engage/release with nothing on must not resurrect anything.
    if (subject) subject->set(engine, false);
    engine.clientGateRx()->setEnabled(false);
    engine.setRxBypassed(true);
    engine.setRxBypassed(false);
    bool anyNr = false;
    for (const Method& m : methods) anyNr = anyNr || m.enabled(engine);
    check(!anyNr, "a bypass over an idle path restores no NR method");
    check(!engine.clientGateRx()->isEnabled(), "a bypass over an idle path restores no stage");

    std::printf("%s\n", failures ? "FAILED" : "OK");
    return failures ? 1 : 0;
}
