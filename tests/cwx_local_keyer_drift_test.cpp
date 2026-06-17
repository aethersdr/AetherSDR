// Unit tests for CwxLocalKeyer drift-corrected scheduling.
//
// The harness overrides the elapsed-clock read and timer arming so onTick()
// can be driven synchronously without relying on Qt's event loop timing.

#include "core/CwxLocalKeyer.h"

#include <QCoreApplication>
#include <QObject>
#include <QString>
#include <QtGlobal>

#include <cstdio>
#include <string>
#include <vector>

using namespace AetherSDR;

namespace {

int g_failed = 0;

void report(const std::string& name, bool ok, const std::string& detail = {})
{
    std::printf("%s %-64s %s\n",
                ok ? "[ OK ]" : "[FAIL]",
                name.c_str(),
                detail.c_str());
    if (!ok) ++g_failed;
}

std::string vectorString(const std::vector<int>& values)
{
    std::string out = "[";
    for (size_t i = 0; i < values.size(); ++i) {
        if (i > 0) out += ", ";
        out += std::to_string(values[i]);
    }
    out += "]";
    return out;
}

void expectEq(const std::string& name, qint64 actual, qint64 expected)
{
    report(name,
           actual == expected,
           "actual=" + std::to_string(actual) +
               " expected=" + std::to_string(expected));
}

void expectStates(const std::string& name,
                  const std::vector<int>& actual,
                  const std::vector<int>& expected)
{
    report(name,
           actual == expected,
           "actual=" + vectorString(actual) +
               " expected=" + vectorString(expected));
}

class TestKeyer final : public CwxLocalKeyer {
public:
    // Drive the schedule synchronously — no real worker thread — so the
    // drift-correction math stays deterministic under onTick()/setElapsed().
    TestKeyer() : CwxLocalKeyer(/*spawnWorker=*/false) {}

    void setElapsed(qint64 elapsedMs) { m_elapsedMs = elapsedMs; }
    void tick() { onTick(); }

    qint64 nextEdgeMs() const { return nextEdgeMsForTest(); }
    bool epochValid() const { return elapsedValidForTest(); }
    int lastWait() const { return waits.empty() ? -1 : waits.back(); }

    std::vector<int> waits;

protected:
    qint64 elapsedMs() const override
    {
        return elapsedValidForTest() ? m_elapsedMs : 0;
    }

    void armTimer(int waitMs) override
    {
        waits.push_back(waitMs);
    }

private:
    qint64 m_elapsedMs{0};
};

struct Fixture {
    TestKeyer keyer;
    std::vector<int> states;

    Fixture()
    {
        keyer.setOnKeyDownChange([this](bool down) {
            states.push_back(down ? 1 : 0);
        });
    }
};

void testCumulativeTargetsAtExactCadence()
{
    Fixture f;
    f.keyer.setElapsed(0);
    f.keyer.start(QStringLiteral("S"), 20);

    expectEq("exact: first dit target", f.keyer.nextEdgeMs(), 60);
    expectEq("exact: first dit wait", f.keyer.lastWait(), 60);

    const std::vector<qint64> elapsedBeforeTick{60, 120, 180, 240};
    const std::vector<qint64> expectedTargets{120, 180, 240, 300};
    for (size_t i = 0; i < expectedTargets.size(); ++i) {
        f.keyer.setElapsed(elapsedBeforeTick[i]);
        f.keyer.tick();
        expectEq("exact: cumulative target " + std::to_string(i + 2),
                 f.keyer.nextEdgeMs(),
                 expectedTargets[i]);
        expectEq("exact: full wait " + std::to_string(i + 2),
                 f.keyer.lastWait(),
                 60);
    }

    const auto waitCountBeforeDrain = static_cast<qint64>(f.keyer.waits.size());
    f.keyer.setElapsed(300);
    f.keyer.tick();

    report("exact: natural drain leaves keyer idle", f.keyer.isIdle());
    expectEq("exact: natural drain resets target", f.keyer.nextEdgeMs(), 0);
    report("exact: natural drain invalidates elapsed epoch",
           !f.keyer.epochValid());
    expectEq("exact: natural drain does not rearm timer",
             static_cast<qint64>(f.keyer.waits.size()),
             waitCountBeforeDrain);
    expectStates("exact: S key transitions", f.states, {1, 0, 1, 0, 1, 0});
}

void testLateTicksShortenNextWait()
{
    Fixture f;
    f.keyer.setElapsed(0);
    f.keyer.start(QStringLiteral("S"), 20);

    f.keyer.setElapsed(f.keyer.nextEdgeMs() + 40);
    f.keyer.tick();
    expectEq("late: target remains absolute after 40 ms slip",
             f.keyer.nextEdgeMs(),
             120);
    expectEq("late: next wait shortens by slip", f.keyer.lastWait(), 20);

    f.keyer.setElapsed(f.keyer.nextEdgeMs() + 30);
    f.keyer.tick();
    expectEq("late: second target remains cumulative",
             f.keyer.nextEdgeMs(),
             180);
    expectEq("late: second wait shortens independently",
             f.keyer.lastWait(),
             30);
}

void testSustainedOverloadClampsWait()
{
    Fixture f;
    f.keyer.setElapsed(0);
    f.keyer.start(QStringLiteral("SS"), 20);

    const std::vector<qint64> expectedTargets{120, 180, 240, 300, 480};
    for (size_t i = 0; i < expectedTargets.size(); ++i) {
        f.keyer.setElapsed(5000);
        f.keyer.tick();
        expectEq("overload: cumulative target " + std::to_string(i + 2),
                 f.keyer.nextEdgeMs(),
                 expectedTargets[i]);
        expectEq("overload: catch-up wait clamps to 1 ms " +
                     std::to_string(i + 1),
                 f.keyer.lastWait(),
                 1);
    }
}

void testQueuedMacrosKeepTimingEpoch()
{
    Fixture f;
    f.keyer.setElapsed(0);
    f.keyer.start(QStringLiteral("S"), 20);
    f.keyer.start(QStringLiteral("E"), 20);

    const std::vector<qint64> elapsedBeforeTick{60, 120, 180, 240};
    const std::vector<qint64> firstMacroTargets{120, 180, 240, 300};
    for (size_t i = 0; i < firstMacroTargets.size(); ++i) {
        f.keyer.setElapsed(elapsedBeforeTick[i]);
        f.keyer.tick();
        expectEq("queued: first macro target " + std::to_string(i + 2),
                 f.keyer.nextEdgeMs(),
                 firstMacroTargets[i]);
    }

    f.keyer.setElapsed(300);
    f.keyer.tick();
    expectEq("queued: inter-macro char gap stays cumulative",
             f.keyer.nextEdgeMs(),
             480);
    expectEq("queued: char-gap wait uses same epoch",
             f.keyer.lastWait(),
             180);
    report("queued: elapsed epoch stays valid between macros",
           f.keyer.epochValid());

    f.keyer.setElapsed(480);
    f.keyer.tick();
    expectEq("queued: second macro continues absolute timeline",
             f.keyer.nextEdgeMs(),
             540);
    expectEq("queued: second macro first dit wait", f.keyer.lastWait(), 60);

    f.keyer.setElapsed(540);
    f.keyer.tick();
    report("queued: final drain idles keyer", f.keyer.isIdle());
    expectEq("queued: final drain resets target", f.keyer.nextEdgeMs(), 0);
    report("queued: final drain invalidates elapsed epoch",
           !f.keyer.epochValid());
    expectStates("queued: key transitions",
                 f.states,
                 {1, 0, 1, 0, 1, 0, 1, 0});
}

void testStopResetsTimingEpoch()
{
    Fixture f;
    f.keyer.setElapsed(0);
    f.keyer.start(QStringLiteral("E"), 20);
    f.keyer.stop();

    report("stop: keyer idles after stop", f.keyer.isIdle());
    expectEq("stop: target resets", f.keyer.nextEdgeMs(), 0);
    report("stop: elapsed epoch invalidates", !f.keyer.epochValid());
    expectStates("stop: key-up emitted", f.states, {1, 0});

    f.keyer.waits.clear();
    f.keyer.setElapsed(0);
    f.keyer.start(QStringLiteral("E"), 20);
    expectEq("stop: next run starts fresh target",
             f.keyer.nextEdgeMs(),
             60);
    expectEq("stop: next run starts fresh wait", f.keyer.lastWait(), 60);
}

void testNaturalDrainResetsTimingEpoch()
{
    Fixture f;
    f.keyer.setElapsed(0);
    f.keyer.start(QStringLiteral("E"), 20);

    f.keyer.setElapsed(60);
    f.keyer.tick();
    report("drain: keyer idles after final edge", f.keyer.isIdle());
    expectEq("drain: target resets", f.keyer.nextEdgeMs(), 0);
    report("drain: elapsed epoch invalidates", !f.keyer.epochValid());
    expectStates("drain: key-up emitted", f.states, {1, 0});

    f.keyer.waits.clear();
    f.keyer.setElapsed(0);
    f.keyer.start(QStringLiteral("E"), 20);
    expectEq("drain: next run starts fresh target",
             f.keyer.nextEdgeMs(),
             60);
    expectEq("drain: next run starts fresh wait", f.keyer.lastWait(), 60);
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    std::printf("CWX local keyer drift-correction test harness\n\n");

    testCumulativeTargetsAtExactCadence();
    testLateTicksShortenNextWait();
    testSustainedOverloadClampsWait();
    testQueuedMacrosKeepTimingEpoch();
    testStopResetsTimingEpoch();
    testNaturalDrainResetsTimingEpoch();

    std::printf("\n%s\n",
                g_failed == 0
                    ? "All tests passed."
                    : (std::to_string(g_failed) + " test(s) failed.").c_str());
    return g_failed == 0 ? 0 : 1;
}
