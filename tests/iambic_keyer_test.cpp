#include "core/IambicKeyer.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

using namespace AetherSDR;
using namespace std::chrono_literals;

namespace {

struct KeyEvent {
    bool down;
    std::chrono::steady_clock::time_point at;
};

class Recorder {
public:
    void onKeyDownChange(bool down)
    {
        std::lock_guard<std::mutex> lk(m_mu);
        m_events.push_back({down, std::chrono::steady_clock::now()});
    }

    void onPaddleEvent(bool dit, bool dah)
    {
        std::lock_guard<std::mutex> lk(m_mu);
        m_paddleDit = dit;
        m_paddleDah = dah;
        ++m_paddleEventCount;
    }

    std::vector<KeyEvent> events()
    {
        std::lock_guard<std::mutex> lk(m_mu);
        return m_events;
    }

    int paddleEventCount()
    {
        std::lock_guard<std::mutex> lk(m_mu);
        return m_paddleEventCount;
    }

    bool lastPaddleDit()
    {
        std::lock_guard<std::mutex> lk(m_mu);
        return m_paddleDit;
    }

    bool lastPaddleDah()
    {
        std::lock_guard<std::mutex> lk(m_mu);
        return m_paddleDah;
    }

    int downCount()
    {
        std::lock_guard<std::mutex> lk(m_mu);
        int n = 0;
        for (const auto& e : m_events) if (e.down) ++n;
        return n;
    }

private:
    std::mutex m_mu;
    std::vector<KeyEvent> m_events;
    bool m_paddleDit{false};
    bool m_paddleDah{false};
    int  m_paddleEventCount{0};
};

int g_failures = 0;

bool report(const char* label, bool ok)
{
    std::cout << (ok ? "[ OK ] " : "[FAIL] ") << label << '\n';
    if (!ok) ++g_failures;
    return ok;
}

// Wall-clock durations are DIAGNOSTIC ONLY in this suite — printed, never
// gating.  condition_variable::wait_until overshoot is environment-dependent
// (macOS timer coalescing was measured returning 155–310 ms for 30–180 ms
// requests; sanitizer instrumentation stretches everything further), so a
// fixed ms tolerance would fail machines the keyer is fine on.  What this
// suite gates is element COUNT and SEQUENCE — deterministic on any scheduler
// and exactly what discriminates Mode A from Mode B (#4032).  Wall-clock
// timing correctness is proven separately by measured captures (#4809).
void noteMs(const char* what, std::chrono::steady_clock::duration d, int nominalMs)
{
    const int actual = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(d).count());
    std::cout << "    info: " << what << " actual=" << actual
              << "ms nominal=" << nominalMs << "ms\n";
}

// Block until the recorder has seen at least `n` key-down edges, or the cap
// expires.  Lets tests release paddles relative to OBSERVED element starts
// instead of guessing element phase with absolute sleeps — the count
// expectations then hold on any scheduler.
bool waitForNthDown(Recorder& rec, int n,
                    std::chrono::milliseconds cap = std::chrono::milliseconds(3000))
{
    const auto deadline = std::chrono::steady_clock::now() + cap;
    while (std::chrono::steady_clock::now() < deadline) {
        if (rec.downCount() >= n) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

// Block until no new key events have arrived for `idle`, or the cap expires.
// Replaces fixed post-release sleeps, which under-count on slow schedulers.
void waitForQuiescence(Recorder& rec,
                       std::chrono::milliseconds idle = std::chrono::milliseconds(300),
                       std::chrono::milliseconds cap = std::chrono::milliseconds(4000))
{
    const auto deadline = std::chrono::steady_clock::now() + cap;
    size_t lastSize = rec.events().size();
    auto lastChange = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        const size_t now = rec.events().size();
        if (now != lastSize) {
            lastSize = now;
            lastChange = std::chrono::steady_clock::now();
        } else if (std::chrono::steady_clock::now() - lastChange > idle) {
            return;
        }
    }
}

// Convenience: compute durations between consecutive on/off events.
std::vector<int> elementDurationsMs(const std::vector<KeyEvent>& evs)
{
    std::vector<int> out;
    for (size_t i = 1; i < evs.size(); ++i) {
        const auto d = evs[i].at - evs[i - 1].at;
        out.push_back(static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(d).count()));
    }
    return out;
}

// 20 WPM → 1200 / 20 = 60 ms unit.  At this rate, dit = 60, dah = 180, gap = 60.
constexpr int kTestUnitMs = 60;
constexpr int kTestWpm = 20;

void testStraightDitProducesOneElement()
{
    Recorder rec;
    IambicKeyer k;
    k.setOnKeyDownChange([&](bool d) { rec.onKeyDownChange(d); });
    k.setOnPaddleEvent([&](bool d, bool h) { rec.onPaddleEvent(d, h); });
    k.setWpm(kTestWpm);
    k.setMode(IambicKeyer::Mode::IambicB);
    k.start();

    // Press dit, release as soon as the element start is observed, wait out
    // the element.
    k.setPaddleState(true, false);
    waitForNthDown(rec, 1);
    k.setPaddleState(false, false);
    waitForQuiescence(rec);
    k.stop();

    const auto evs = rec.events();
    bool ok = (rec.downCount() == 1
               && evs.size() >= 2 && evs[0].down && !evs[1].down);
    if (evs.size() >= 2) noteMs("dit element", evs[1].at - evs[0].at, kTestUnitMs);
    report("single dit press produces exactly one element", ok);
}

void testStraightDahProducesOneElement()
{
    Recorder rec;
    IambicKeyer k;
    k.setOnKeyDownChange([&](bool d) { rec.onKeyDownChange(d); });
    k.setOnPaddleEvent([&](bool d, bool h) { rec.onPaddleEvent(d, h); });
    k.setWpm(kTestWpm);
    k.setMode(IambicKeyer::Mode::IambicB);
    k.start();

    k.setPaddleState(false, true);
    waitForNthDown(rec, 1);
    k.setPaddleState(false, false);
    waitForQuiescence(rec);
    k.stop();

    const auto evs = rec.events();
    bool ok = (rec.downCount() == 1
               && evs.size() >= 2 && evs[0].down && !evs[1].down);
    if (evs.size() >= 2) noteMs("dah element", evs[1].at - evs[0].at, kTestUnitMs * 3);
    report("single dah press produces exactly one element", ok);
}

void testSqueezeAlternatesDitDah()
{
    Recorder rec;
    IambicKeyer k;
    k.setOnKeyDownChange([&](bool d) { rec.onKeyDownChange(d); });
    k.setOnPaddleEvent([&](bool d, bool h) { rec.onPaddleEvent(d, h); });
    k.setWpm(kTestWpm);
    k.setMode(IambicKeyer::Mode::IambicB);
    k.start();

    // Squeeze until four element starts have been observed, then release.
    // Gating on observed count (not a fixed squeeze duration) keeps the
    // expectation scheduler-independent.
    k.setPaddleState(true, true);
    const bool sawFour = waitForNthDown(rec, 4);
    k.setPaddleState(false, false);
    waitForQuiescence(rec);
    k.stop();

    const auto evs = rec.events();
    // Sequence gate: strictly alternating down/up starting with down.
    bool alternates = evs.size() >= 8;
    for (size_t i = 0; i < evs.size() && alternates; ++i)
        alternates &= (evs[i].down == (i % 2 == 0));
    const bool ok = sawFour && alternates;
    const auto durations = elementDurationsMs(evs);
    if (durations.size() >= 3) {
        noteMs("element 1", evs[1].at - evs[0].at, kTestUnitMs);
        noteMs("element 2", evs[3].at - evs[2].at, kTestUnitMs * 3);
    }
    report("squeeze produces a sustained alternating element stream", ok);
}

void testInterElementGapIsOneUnit()
{
    Recorder rec;
    IambicKeyer k;
    k.setOnKeyDownChange([&](bool d) { rec.onKeyDownChange(d); });
    k.setOnPaddleEvent([&](bool d, bool h) { rec.onPaddleEvent(d, h); });
    k.setWpm(kTestWpm);
    k.setMode(IambicKeyer::Mode::IambicB);
    k.start();

    k.setPaddleState(true, true);
    waitForNthDown(rec, 2);
    k.setPaddleState(false, false);
    waitForQuiescence(rec);
    k.stop();

    const auto evs = rec.events();
    // Gate: consecutive elements are separated by a real key-up interval
    // (down, up, down ordering).  The gap's LENGTH is diagnostic only.
    bool ok = (evs.size() >= 3 && evs[0].down && !evs[1].down && evs[2].down);
    if (evs.size() >= 3)
        noteMs("inter-element gap", evs[2].at - evs[1].at, kTestUnitMs);
    report("consecutive elements are separated by a key-up gap", ok);
}

void testReleaseStopsAfterCurrentElement()
{
    Recorder rec;
    IambicKeyer k;
    k.setOnKeyDownChange([&](bool d) { rec.onKeyDownChange(d); });
    k.setOnPaddleEvent([&](bool d, bool h) { rec.onPaddleEvent(d, h); });
    k.setWpm(kTestWpm);
    k.setMode(IambicKeyer::Mode::IambicA);   // mode A: stop at element boundary
    k.start();

    // Press dit, release as soon as its start is observed (mid-element on any
    // scheduler) — the element must still complete, with no extras.
    k.setPaddleState(true, false);
    waitForNthDown(rec, 1);
    k.setPaddleState(false, false);
    waitForQuiescence(rec);
    k.stop();

    const bool ok = (rec.downCount() == 1);
    report("release before element completes still finishes that element (no truncation)", ok);
}

void testWpmChangesElementDuration()
{
    Recorder rec;
    IambicKeyer k;
    k.setOnKeyDownChange([&](bool d) { rec.onKeyDownChange(d); });
    k.setOnPaddleEvent([&](bool d, bool h) { rec.onPaddleEvent(d, h); });
    k.setWpm(40);    // 1200/40 = 30ms unit
    k.setMode(IambicKeyer::Mode::IambicB);
    k.start();

    k.setPaddleState(true, false);
    waitForNthDown(rec, 1);
    k.setPaddleState(false, false);
    waitForQuiescence(rec);
    k.stop();

    const auto evs = rec.events();
    const bool ok = (rec.downCount() == 1 && evs.size() >= 2);
    if (evs.size() >= 2) noteMs("dit at WPM=40", evs[1].at - evs[0].at, 30);
    report("WPM=40 still produces exactly one element per press", ok);
}

void testSwapPaddlesInvertsDitDah()
{
    Recorder rec;
    IambicKeyer k;
    k.setOnKeyDownChange([&](bool d) { rec.onKeyDownChange(d); });
    k.setOnPaddleEvent([&](bool d, bool h) { rec.onPaddleEvent(d, h); });
    k.setWpm(kTestWpm);
    k.setMode(IambicKeyer::Mode::IambicB);
    k.setSwapPaddles(true);
    k.start();

    // With swap on, pressing physical "dit" must register as a dah.  The
    // portable proof is the forwarded paddle event (the keyer swaps before
    // storing, so the radio-facing callback sees dah=true); the element's
    // dah-length duration is diagnostic only.
    k.setPaddleState(true, false);
    waitForNthDown(rec, 1);
    const bool sawSwappedDah = rec.lastPaddleDah() && !rec.lastPaddleDit();
    k.setPaddleState(false, false);
    waitForQuiescence(rec);
    k.stop();

    const auto evs = rec.events();
    const bool ok = sawSwappedDah && rec.downCount() == 1 && evs.size() >= 2;
    if (evs.size() >= 2)
        noteMs("swapped element (dah nominal)", evs[1].at - evs[0].at, kTestUnitMs * 3);
    report("swapPaddles=true routes the physical dit-side to dah", ok);
}

// #4032: a clean simultaneous release of both paddles — one combined
// setPaddleState(false, false), exactly what SerialPortController emits when
// both contacts open inside one ~10 ms poll tick — must still produce the
// Mode B extra opposite element.  Before the fix the memory latch could only
// observe paddle state on a condition-variable wake, and the release wake
// arrives with the held state already overwritten, so Mode B silently
// degraded to Mode A.
void testModeBSimultaneousReleaseDuringDitAddsDah()
{
    Recorder rec;
    IambicKeyer k;
    k.setOnKeyDownChange([&](bool d) { rec.onKeyDownChange(d); });
    k.setOnPaddleEvent([&](bool d, bool h) { rec.onPaddleEvent(d, h); });
    k.setWpm(kTestWpm);
    k.setMode(IambicKeyer::Mode::IambicB);
    k.start();

    // Squeeze both — the first element is the dit.  Release both in a single
    // combined event as soon as the dit's key-down is OBSERVED, which is
    // guaranteed to be inside its on-period on any scheduler (elements can
    // only ever run long, never short).
    k.setPaddleState(true, true);
    waitForNthDown(rec, 1);
    k.setPaddleState(false, false);
    waitForQuiescence(rec);
    k.stop();

    const auto evs = rec.events();
    // Canonical Mode B: the dit in progress, then one extra dah from memory.
    // Count is the discriminator — the unfixed keyer emits exactly one.
    const bool ok = (rec.downCount() == 2 && evs.size() >= 4);
    if (evs.size() >= 4) {
        noteMs("dit", evs[1].at - evs[0].at, kTestUnitMs);
        noteMs("trailing dah", evs[3].at - evs[2].at, kTestUnitMs * 3);
    }
    report("mode B: simultaneous release during dit still sends the extra dah", ok);
}

// Mirror of the above with the dah in progress, so the dit-memory branch of
// the start-of-element latch is exercised too (a fix that only latched the
// dah side would pass the first test and fail this one).
void testModeBSimultaneousReleaseDuringDahAddsDit()
{
    Recorder rec;
    IambicKeyer k;
    k.setOnKeyDownChange([&](bool d) { rec.onKeyDownChange(d); });
    k.setOnPaddleEvent([&](bool d, bool h) { rec.onPaddleEvent(d, h); });
    k.setWpm(kTestWpm);
    k.setMode(IambicKeyer::Mode::IambicB);
    k.start();

    // Squeeze: the alternation runs dit, then dah.  Release both in one
    // combined event as soon as the SECOND key-down (the dah) is observed —
    // event-driven, so the release provably lands inside the dah on any
    // scheduler, instead of hoping an absolute sleep stays phase-aligned.
    k.setPaddleState(true, true);
    waitForNthDown(rec, 2);
    k.setPaddleState(false, false);
    waitForQuiescence(rec);
    k.stop();

    const auto evs = rec.events();
    // dit, dah in progress, then one extra dit from memory.
    const bool ok = (rec.downCount() == 3 && evs.size() >= 6);
    if (evs.size() >= 6) {
        noteMs("dah", evs[3].at - evs[2].at, kTestUnitMs * 3);
        noteMs("trailing dit", evs[5].at - evs[4].at, kTestUnitMs);
    }
    report("mode B: simultaneous release during dah still sends the extra dit", ok);
}

// Mode A control: the identical simultaneous release must NOT gain an extra
// element — the start-of-element latch is Mode B only.
void testModeASimultaneousReleaseAddsNothing()
{
    Recorder rec;
    IambicKeyer k;
    k.setOnKeyDownChange([&](bool d) { rec.onKeyDownChange(d); });
    k.setOnPaddleEvent([&](bool d, bool h) { rec.onPaddleEvent(d, h); });
    k.setWpm(kTestWpm);
    k.setMode(IambicKeyer::Mode::IambicA);
    k.start();

    k.setPaddleState(true, true);
    waitForNthDown(rec, 1);
    k.setPaddleState(false, false);
    waitForQuiescence(rec);
    k.stop();

    const bool ok = (rec.downCount() == 1);
    report("mode A: simultaneous release adds no extra element", ok);
}

void testIdleProducesNoElements()
{
    Recorder rec;
    IambicKeyer k;
    k.setOnKeyDownChange([&](bool d) { rec.onKeyDownChange(d); });
    k.setOnPaddleEvent([&](bool d, bool h) { rec.onPaddleEvent(d, h); });
    k.setWpm(kTestWpm);
    k.start();

    std::this_thread::sleep_for(200ms);
    k.stop();

    bool ok = rec.events().empty();
    report("no paddle press produces no key-down events", ok);
}

void testStartIsIdempotent()
{
    Recorder rec;
    IambicKeyer k;
    k.setOnKeyDownChange([&](bool d) { rec.onKeyDownChange(d); });
    k.setWpm(kTestWpm);
    k.start();
    k.start();   // second call should be a no-op
    k.stop();
    report("start() is idempotent (no crash on second call)", true);
}

} // namespace

int main()
{
    testIdleProducesNoElements();
    testStraightDitProducesOneElement();
    testStraightDahProducesOneElement();
    testSqueezeAlternatesDitDah();
    testInterElementGapIsOneUnit();
    testReleaseStopsAfterCurrentElement();
    testWpmChangesElementDuration();
    testSwapPaddlesInvertsDitDah();
    testModeBSimultaneousReleaseDuringDitAddsDah();
    testModeBSimultaneousReleaseDuringDahAddsDit();
    testModeASimultaneousReleaseAddsNothing();
    testStartIsIdempotent();
    std::cout << "iambic_keyer_test: done, failures=" << g_failures << '\n';
    return g_failures == 0 ? 0 : 1;
}
