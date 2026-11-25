// Squelch gate display (#6552 option C), socket-free: SquelchGateReporter's
// rate limit, which turns a per-block gate reading into the changes worth
// publishing, and SliceModel taking SliceDelta::squelchGate.

#include "core/backends/SliceDelta.h"
#include "core/backends/SquelchGate.h"
#include "models/SliceModel.h"

#include <QCoreApplication>
#include <QSignalSpy>

#include <cstdio>
#include <optional>

using namespace AetherSDR;

static int g_failures = 0;
static void check(bool cond, const char* what)
{
    std::fprintf(cond ? stdout : stderr, "%s: %s\n", cond ? "ok" : "FAIL", what);
    if (!cond) {
        ++g_failures;
    }
}

namespace {

constexpr double kBlock = 1024.0 / 192000.0;   // one HL2 DSP block at 192 kHz

void reporterTests()
{
    using S = SquelchGateState;
    {
        SquelchGateReporter r;
        check(!r.feed(std::nullopt, kBlock), "no gate at the start publishes nothing");
        check(r.feed(false, kBlock) == S::Closed, "the first reading is published at once");
        check(!r.feed(false, kBlock), "an unchanged reading publishes nothing");
        // A change inside the hold waits, then lands once the hold has passed.
        check(!r.feed(true, kBlock), "a change inside the hold is held");
        int blocks = 1;
        std::optional<S> out;
        while (!out && blocks < 1000) {
            out = r.feed(true, kBlock);
            ++blocks;
        }
        check(out == S::Open, "the held change is published");
        const double waited = (blocks + 1) * kBlock;   // the unchanged feed counts too
        check(waited >= SquelchGateReporter::kDefaultHoldSeconds
                  && waited < SquelchGateReporter::kDefaultHoldSeconds + 2 * kBlock,
              "the held change lands one hold after the last publication");
        check(r.published() == S::Open, "published() is the last published state");
        // Unavailable goes at once, inside the hold: squelch off clears the display.
        check(r.feed(std::nullopt, kBlock) == S::Unavailable, "Unavailable is published at once");
        check(!r.feed(std::nullopt, kBlock), "a repeated Unavailable publishes nothing");
    }
    {
        // Chatter: the gate flips every block for 2 s. At most one publication
        // per hold, and the last one is the state the gate settles in.
        SquelchGateReporter r;
        int published = 0;
        std::optional<S> last;
        const int blocks = static_cast<int>(2.0 / kBlock);
        for (int b = 0; b < blocks; ++b) {
            if (const auto out = r.feed(b % 2 == 0, kBlock)) {
                ++published;
                last = out;
            }
        }
        for (int b = 0; b < 100; ++b) {
            if (const auto out = r.feed(false, kBlock)) {
                ++published;
                last = out;
            }
        }
        std::printf("chatter over 2 s: %d publications\n", published);
        check(published <= 2.0 / SquelchGateReporter::kDefaultHoldSeconds + 2,
              "a chattering gate publishes at most once per hold");
        check(published >= 5, "a chattering gate is still shown changing");
        check(last == S::Closed && r.published() == S::Closed,
              "the reporter ends on the state the gate settles in");
    }
    {
        SquelchGateReporter r;
        check(r.feed(true, kBlock) == S::Open, "open published");
        r.reset();
        check(r.feed(true, kBlock) == S::Open, "after reset() the same state is published again");
        check(!r.feed(false, 0.0) && !r.feed(false, -1.0),
              "a block of no duration does not count towards the hold");
        SquelchGateReporter immediate(0.0);
        check(immediate.feed(true, kBlock) == S::Open && immediate.feed(false, kBlock) == S::Closed,
              "a zero hold publishes every change");
    }
}

void sliceModelTests()
{
    SliceModel slice(0);
    check(slice.squelchGate() == SquelchGateState::Unavailable, "a new slice shows no gate");
    QSignalSpy spy(&slice, &SliceModel::squelchGateChanged);
    SliceDelta d;
    d.squelchGate = SquelchGateState::Closed;
    slice.applyChanges(d);
    check(slice.squelchGate() == SquelchGateState::Closed && spy.size() == 1,
          "a reported gate is taken and announced");
    slice.applyChanges(d);
    check(spy.size() == 1, "the same gate again announces nothing");
    SliceDelta other;
    other.squelchLevel = 30;
    slice.applyChanges(other);
    check(slice.squelchGate() == SquelchGateState::Closed && spy.size() == 1,
          "a delta without the field leaves the gate alone");
    d.squelchGate = SquelchGateState::Open;
    slice.applyChanges(d);
    check(slice.squelchGate() == SquelchGateState::Open && spy.size() == 2,
          "a change is announced");
    d.squelchGate = static_cast<SquelchGateState>(7);
    slice.applyChanges(d);
    check(slice.squelchGate() == SquelchGateState::Open && spy.size() == 2,
          "an out-of-range state is ignored");
}

}  // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    reporterTests();
    sliceModelTests();
    if (g_failures == 0) {
        std::printf("squelch_gate_test: all checks passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}
