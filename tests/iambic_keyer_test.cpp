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

bool nearMs(std::chrono::steady_clock::duration d, int expectMs, int toleranceMs = 15)
{
    const int actual = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(d).count());
    const bool ok = std::abs(actual - expectMs) <= toleranceMs;
    if (!ok) std::cout << "    actual=" << actual << "ms expected=" << expectMs << "ms\n";
    return ok;
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

    // Press dit briefly, release, wait for one element to complete.
    k.setPaddleState(true, false);
    std::this_thread::sleep_for(10ms);
    k.setPaddleState(false, false);
    std::this_thread::sleep_for(150ms);
    k.stop();

    const auto evs = rec.events();
    bool ok = (evs.size() >= 2 && evs[0].down == true && evs[1].down == false);
    if (ok) ok &= nearMs(evs[1].at - evs[0].at, kTestUnitMs);
    report("single dit press produces one ~60ms key-down element", ok);
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
    std::this_thread::sleep_for(10ms);
    k.setPaddleState(false, false);
    std::this_thread::sleep_for(300ms);
    k.stop();

    const auto evs = rec.events();
    bool ok = (evs.size() >= 2 && evs[0].down == true && evs[1].down == false);
    if (ok) ok &= nearMs(evs[1].at - evs[0].at, kTestUnitMs * 3);
    report("single dah press produces one ~180ms key-down element", ok);
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

    // Squeeze for 1 second — should produce alternating dit/dah elements.
    k.setPaddleState(true, true);
    std::this_thread::sleep_for(1000ms);
    k.setPaddleState(false, false);
    std::this_thread::sleep_for(200ms);
    k.stop();

    const auto evs = rec.events();
    // We expect on/off pairs; even-indexed events are key-down, odd are
    // key-up.  Element durations should alternate 60/180/60/180...
    bool ok = (evs.size() >= 6);
    if (ok) {
        const auto durations = elementDurationsMs(evs);
        // First on→off should be 60 (dit), next on→off should be 180 (dah).
        // durations alternate [on, off, on, off, ...] so even indices are
        // element durations.
        ok &= (std::abs(durations[0] - kTestUnitMs) <= 15);
        ok &= (std::abs(durations[2] - kTestUnitMs * 3) <= 15);
        ok &= (std::abs(durations[4] - kTestUnitMs) <= 15);
    }
    report("squeezed paddle alternates dit/dah", ok);
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
    std::this_thread::sleep_for(500ms);
    k.setPaddleState(false, false);
    std::this_thread::sleep_for(200ms);
    k.stop();

    const auto evs = rec.events();
    bool ok = (evs.size() >= 4);
    if (ok) {
        // Gap = (on→off→on) where the off→on transition is the gap.
        // Index 1 is first key-up, index 2 is next key-down.
        ok &= nearMs(evs[2].at - evs[1].at, kTestUnitMs);
    }
    report("inter-element gap is 1 unit (~60ms at 20 WPM)", ok);
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

    // Press dit, release immediately — should still complete one full dit.
    k.setPaddleState(true, false);
    std::this_thread::sleep_for(20ms);
    k.setPaddleState(false, false);
    std::this_thread::sleep_for(300ms);
    k.stop();

    const auto evs = rec.events();
    // Should have exactly one on/off pair, no extra elements.
    int onCount = 0;
    for (const auto& e : evs) if (e.down) ++onCount;
    bool ok = (onCount == 1);
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
    std::this_thread::sleep_for(10ms);
    k.setPaddleState(false, false);
    std::this_thread::sleep_for(100ms);
    k.stop();

    const auto evs = rec.events();
    bool ok = (evs.size() >= 2);
    if (ok) ok &= nearMs(evs[1].at - evs[0].at, 30);
    report("WPM=40 produces ~30ms dit", ok);
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

    // With swap on, pressing physical "dit" should produce a dah.
    k.setPaddleState(true, false);
    std::this_thread::sleep_for(10ms);
    k.setPaddleState(false, false);
    std::this_thread::sleep_for(300ms);
    k.stop();

    const auto evs = rec.events();
    bool ok = (evs.size() >= 2);
    if (ok) ok &= nearMs(evs[1].at - evs[0].at, kTestUnitMs * 3);  // dah duration
    report("swapPaddles=true makes physical dit-side produce dah timing", ok);
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

    // Squeeze both — the first element is a dit (60 ms).  Release both in a
    // single combined event 20 ms in, well inside the dit's on-period.
    k.setPaddleState(true, true);
    std::this_thread::sleep_for(20ms);
    k.setPaddleState(false, false);
    std::this_thread::sleep_for(500ms);
    k.stop();

    const auto evs = rec.events();
    int onCount = 0;
    for (const auto& e : evs) if (e.down) ++onCount;
    // Canonical Mode B: the dit in progress, then one extra dah from memory.
    bool ok = (onCount == 2 && evs.size() >= 4);
    if (ok) {
        ok &= nearMs(evs[1].at - evs[0].at, kTestUnitMs);       // dit
        ok &= nearMs(evs[3].at - evs[2].at, kTestUnitMs * 3);   // trailing dah
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

    // Squeeze: dit (60) + gap (60) then the dah (180) begins.  Release both
    // in one combined event ~30 ms into the dah's on-period (t = 150 ms).
    k.setPaddleState(true, true);
    std::this_thread::sleep_for(150ms);
    k.setPaddleState(false, false);
    std::this_thread::sleep_for(600ms);
    k.stop();

    const auto evs = rec.events();
    int onCount = 0;
    for (const auto& e : evs) if (e.down) ++onCount;
    // dit, dah in progress, then one extra dit from memory.
    bool ok = (onCount == 3 && evs.size() >= 6);
    if (ok) {
        ok &= nearMs(evs[3].at - evs[2].at, kTestUnitMs * 3);   // the dah
        ok &= nearMs(evs[5].at - evs[4].at, kTestUnitMs);       // trailing dit
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
    std::this_thread::sleep_for(20ms);
    k.setPaddleState(false, false);
    std::this_thread::sleep_for(500ms);
    k.stop();

    const auto evs = rec.events();
    int onCount = 0;
    for (const auto& e : evs) if (e.down) ++onCount;
    bool ok = (onCount == 1);
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
