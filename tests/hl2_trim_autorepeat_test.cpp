// Trim auto-repeat: applied live while held, persisted exactly once on release.
//
// WHY THIS TEST EXISTS. The Calibration page's Trim buttons auto-repeat so the
// beat note moves under the operator's hand while they null it, but every
// persisting step is an AppSettings::save() — a full non-atomic settings write
// (#4273) — plus an NCO re-push. The split between "apply live" and "persist"
// therefore has to hold, and it rests on one non-obvious Qt property:
//
//   QAbstractButtonPrivate::repeatTimerTimeout() emits released(), clicked()
//   and pressed() on EVERY auto-repeat tick and leaves the button DOWN.
//   Only the real release path (mouseReleaseEvent -> QAbstractButtonPrivate::
//   click()) clears `down` BEFORE emitting released() and clicked().
//
// Two consequences the production code depends on, both pinned below:
//
//   1. isDown() inside the handler is the only thing separating "still held"
//      from "let go". A commit that is not gated on it runs ~8x/second.
//   2. The gate has to live in clicked(), not released(). released() is emitted
//      FIRST, so a commit there would store the value from one step ago and the
//      operator's final trim would never reach the settings store.
//
// This mirrors RadioSetupDialog::buildCalibrationTab()'s wiring exactly —
// including setAutoRepeatDelay(400)/setAutoRepeatInterval(120) — rather than
// instantiating the dialog, which would drag the whole GUI into a unit test for
// a four-line contract.

#include <QApplication>
#include <QPushButton>
#include <QTest>

#include <cstdio>
#include <vector>

namespace {

int g_failures = 0;

void check(bool cond, const char* what)
{
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

// The page's Trim button, configured the way the page configures it.
QPushButton* makeTrimButton()
{
    auto* b = new QPushButton(QStringLiteral("+"));
    b->setAutoRepeat(true);
    b->setAutoRepeatDelay(400);
    b->setAutoRepeatInterval(120);
    b->resize(44, 24);
    return b;
}

struct Applied {
    int value;
    bool persist;
};

// The production handler, verbatim in shape: one connection on clicked(), the
// persist flag derived from isDown().
std::vector<Applied> runTrim(int holdMs)
{
    QPushButton* b = makeTrimButton();
    b->show();
    if (!QTest::qWaitForWindowExposed(b)) {
        std::fprintf(stderr, "FAIL: trim button never exposed\n");
        ++g_failures;
    }

    std::vector<Applied> applied;
    int value = 0;
    QObject::connect(b, &QPushButton::clicked, b, [&] {
        value += 100;                       // one step, as the page does
        applied.push_back({value, !b->isDown()});
    });

    QTest::mousePress(b, Qt::LeftButton, {}, QPoint(20, 12));
    if (holdMs > 0)
        QTest::qWait(holdMs);
    QTest::mouseRelease(b, Qt::LeftButton, {}, QPoint(20, 12));
    QTest::qWait(20);

    delete b;
    return applied;
}

int persistCount(const std::vector<Applied>& v)
{
    int n = 0;
    for (const Applied& a : v)
        n += a.persist ? 1 : 0;
    return n;
}

// A single click — no hold, no repeat — must persist, or an operator who taps
// Trim once loses the step on the next connect.
void testSingleClickPersistsOnce()
{
    const std::vector<Applied> applied = runTrim(0);
    check(applied.size() == 1, "a single click applies exactly once");
    check(persistCount(applied) == 1, "a single click persists exactly once");
    if (!applied.empty())
        check(applied.back().value == 100, "the persisted value is the stepped value");
}

// Holding past the auto-repeat delay must apply repeatedly and persist ONCE,
// with the final value — this is the case the released()-based wiring got wrong
// in both directions (persisted every tick, and one step stale).
void testHoldAppliesLiveAndPersistsOnceAtTheEnd()
{
    // 400 ms delay + 120 ms interval: ~1000 ms of hold is several repeats.
    const std::vector<Applied> applied = runTrim(1000);

    check(applied.size() >= 4,
          "holding Trim auto-repeats (the beat note moves under the operator's hand)");
    check(persistCount(applied) == 1,
          "holding Trim writes the settings store exactly once, not once per tick");

    if (!applied.empty()) {
        check(applied.back().persist,
              "the persisting apply is the LAST one, so the stored value is final");
        for (std::size_t i = 0; i + 1 < applied.size(); ++i) {
            check(!applied[i].persist,
                  "every apply before the release is live-only");
        }
    }
}

// The shape this replaced, asserted as a tripwire rather than left as prose:
// commit-on-released() — no isDown() gate — persists on every auto-repeat tick,
// and does it with the PRE-step value because released() is emitted before the
// clicked() that applies the step. If Qt ever stops emitting released() during
// auto-repeat this fails, which is the signal that the discriminator in
// RadioSetupDialog::buildCalibrationTab() is no longer load-bearing.
void testCommitOnReleasedWouldPersistEveryTick()
{
    QPushButton* b = makeTrimButton();
    b->show();
    if (!QTest::qWaitForWindowExposed(b)) {
        std::fprintf(stderr, "FAIL: trim button never exposed\n");
        ++g_failures;
    }

    int value = 0;
    std::vector<int> persisted;
    QObject::connect(b, &QPushButton::clicked, b, [&] { value += 100; });
    QObject::connect(b, &QPushButton::released, b, [&] { persisted.push_back(value); });

    QTest::mousePress(b, Qt::LeftButton, {}, QPoint(20, 12));
    QTest::qWait(1000);
    QTest::mouseRelease(b, Qt::LeftButton, {}, QPoint(20, 12));
    QTest::qWait(20);

    check(persisted.size() > 1,
          "commit-on-released() fires on every repeat tick — the bug the isDown() "
          "gate in the Calibration page exists to prevent");
    check(!persisted.empty() && persisted.back() != value,
          "commit-on-released() also stores the PRE-step value: released() is "
          "emitted before the clicked() that applies the step");

    delete b;
}

// The property the whole split rests on, asserted directly so a Qt change is
// reported as itself rather than as a mysterious extra settings write.
void testAutoRepeatLeavesTheButtonDown()
{
    QPushButton* b = makeTrimButton();
    b->show();
    if (!QTest::qWaitForWindowExposed(b)) {
        std::fprintf(stderr, "FAIL: trim button never exposed\n");
        ++g_failures;
    }

    std::vector<bool> downAtRelease;
    std::vector<bool> downAtClick;
    QObject::connect(b, &QPushButton::released, b,
                     [&] { downAtRelease.push_back(b->isDown()); });
    QObject::connect(b, &QPushButton::clicked, b,
                     [&] { downAtClick.push_back(b->isDown()); });

    QTest::mousePress(b, Qt::LeftButton, {}, QPoint(20, 12));
    QTest::qWait(1000);
    const std::size_t repeats = downAtClick.size();
    check(repeats >= 3, "auto-repeat ticked while the button was held");
    for (std::size_t i = 0; i < repeats; ++i) {
        check(downAtClick[i], "auto-repeat emits clicked() with the button still down");
        check(i < downAtRelease.size() && downAtRelease[i],
              "auto-repeat emits released() with the button still down");
    }

    QTest::mouseRelease(b, Qt::LeftButton, {}, QPoint(20, 12));
    QTest::qWait(20);
    check(downAtClick.size() == repeats + 1, "the real release emits one more clicked()");
    check(!downAtClick.empty() && !downAtClick.back(),
          "the real release emits clicked() with the button already up");
    check(!downAtRelease.empty() && !downAtRelease.back(),
          "the real release emits released() with the button already up");
    // released() precedes clicked() on the real release, which is why the commit
    // is gated in clicked(): a commit in released() would store the pre-step value.
    check(downAtRelease.size() == downAtClick.size(),
          "released() and clicked() are emitted in lockstep, released() first");

    delete b;
}

}  // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);

    testSingleClickPersistsOnce();
    testHoldAppliesLiveAndPersistsOnceAtTheEnd();
    testCommitOnReleasedWouldPersistEveryTick();
    testAutoRepeatLeavesTheButtonDown();

    if (g_failures == 0)
        std::printf("hl2_trim_autorepeat_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
