// HGauge hover readout: exactly one badge on screen, and it goes away.
//
// #3936 gave every gauge its own DragValuePopup with no coordination between
// them, and lingered it for 1000 ms on leave. Traversing the stacked TX meters
// (RF Pwr and SWR are consecutive rows two pixels apart) therefore left the
// previous gauge's badge on screen alongside the new one, overlapping it.
//
// The third case outlives even that linger: linger() only starts a hide timer
// and every showValue() cancels it, while setValue() re-shows the badge on each
// meter frame for as long as m_hovered is true. One dropped leaveEvent — which
// Qt does not guarantee, and which the hideEvent override in HGauge.h already
// exists to paper over — pins the badge on screen indefinitely, frozen at the
// anchor the pointer left it at. Recovery is a watchdog rather than a check on
// the value path, because setValue() early-returns on an unchanged reading and
// a settled meter would never reach it.
//
// The fourth case is the converse, and it is why the watchdog is gated on the
// physical cursor: the automation bridge injects hovers WITHOUT moving the
// cursor, so a per-frame cursor check would tear a driver's badge down under it.
//
// Asserts popup VISIBILITY rather than the hover flags, because the flags are
// internal and the two-badges-at-once symptom is precisely a case where each
// gauge's own state reads correct in isolation.

#include "DragValuePopup.h"
#include "HGauge.h"

#include <QApplication>
#include <QCursor>
#include <QElapsedTimer>
#include <QEnterEvent>
#include <QEvent>
#include <QPointF>
#include <QVBoxLayout>
#include <QWidget>

#include <cstdio>

using AetherSDR::DragValuePopup;
using AetherSDR::HGauge;

namespace {

int failures = 0;

void check(bool ok, const char* what)
{
    std::printf("%s  %s\n", ok ? "[ OK ]" : "[FAIL]", what);
    if (!ok) ++failures;
}

// An unmet ENVIRONMENT precondition, not a product defect. Printed rather than
// silently passed — a skipped case that reads as a passing case is worse than
// a noisy one.
void skip(const char* what, const char* why)
{
    std::printf("[SKIP]  %s  (%s)\n", what, why);
}

QVector<HGauge::Tick> noTicks() { return {}; }

// Mirrors HGauge's own constant rather than re-stating the number, so the
// waits below cannot drift away from the interval they are waiting on.
constexpr int kWatchdogMs = HGauge::kHoverWatchdogMs;

// #3936's bespoke hover linger, superseded by DragValuePopup::kDefaultLingerMs.
// Deliberately a literal: it is the value this test exists to rule OUT, so it
// must not follow the shipped constant if that ever changes again.
constexpr int kSupersededLingerMs = 1000;

// How far past the linger to wait before asserting the badge is gone. Has to
// leave room for a loaded runner to service the timer, while staying under
// kSupersededLingerMs or the assertion stops discriminating.
constexpr int kLingerSlackMs = 250;
static_assert(DragValuePopup::kDefaultLingerMs + kLingerSlackMs
                  < kSupersededLingerMs,
              "the gone-by check must land between the two linger values");

// The badge is parented to the gauge that owns it, so it is findable without
// widening HGauge's interface just for the test. Looked up by object name
// rather than by type: DragValuePopup has no Q_OBJECT, so findChild<T*>() is
// not available for it.
QWidget* badgeOf(HGauge* gauge)
{
    return gauge->findChild<QWidget*>(QStringLiteral("DragValuePopup"));
}

bool badgeVisible(HGauge* gauge)
{
    QWidget* badge = badgeOf(gauge);
    return badge && badge->isVisible();
}

void sendEnter(HGauge* gauge, const QPoint& global)
{
    const QPointF local = gauge->mapFromGlobal(global);
    QEnterEvent ev(local, local, QPointF(global));
    QApplication::sendEvent(gauge, &ev);
}

void sendLeave(HGauge* gauge)
{
    QEvent ev(QEvent::Leave);
    QApplication::sendEvent(gauge, &ev);
}

// Pump the event loop for a wall-clock interval so the linger timer can fire.
void spin(int msec)
{
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < msec)
        QApplication::processEvents(QEventLoop::AllEvents, 10);
}

// Poll for the badge to go away rather than sleeping a fixed span and asserting
// once. Every "gone by now" case here is bounded by a timer the code itself
// owns, so a badge still up at a generous deadline is a real defect and a slow
// CI runner just costs wall time. The linger-DURATION check is the deliberate
// exception — it has to discriminate 450 ms from 1000 ms, so it cannot wait
// longer and measures elapsed time instead.
bool badgeGoneWithin(HGauge* gauge, int deadlineMs)
{
    QElapsedTimer clock;
    clock.start();
    while (badgeVisible(gauge) && clock.elapsed() < deadlineMs)
        spin(20);
    return !badgeVisible(gauge);
}

HGauge* addGauge(QWidget* host, const char* label)
{
    auto* gauge = new HGauge(0.0f, 100.0f, 90.0f, QString::fromLatin1(label),
                             "W", noTicks(), host);
    gauge->setHoverValuePopupEnabled(true);
    host->layout()->addWidget(gauge);
    return gauge;
}

}  // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);

    // Two gauges stacked like TxApplet's RF Pwr / SWR pair.
    QWidget host;
    auto* vbox = new QVBoxLayout(&host);
    vbox->setSpacing(2);
    HGauge* fwd = addGauge(&host, "RF Pwr");
    HGauge* swr = addGauge(&host, "SWR");
    host.resize(240, 60);
    host.move(400, 400);
    host.show();
    QApplication::processEvents();

    fwd->setValueImmediate(42.0f);
    swr->setValueImmediate(11.0f);

    // ── the reported case: leave one meter, enter the next ───────────────
    // The linger is what keeps the first badge alive long enough to overlap,
    // so the hand-off has to happen on entering the second gauge, not on
    // leaving the first.
    {
        sendEnter(fwd, fwd->mapToGlobal(fwd->rect().center()));
        check(badgeVisible(fwd), "hovering the first meter shows its badge");

        sendLeave(fwd);
        check(badgeVisible(fwd), "the badge lingers after the pointer leaves");

        sendEnter(swr, swr->mapToGlobal(swr->rect().center()));
        check(badgeVisible(swr), "hovering the second meter shows its badge");
        check(!badgeVisible(fwd),
              "the first meter's badge is gone — never two at once");

        sendLeave(swr);
        spin(50);
    }

    // ── the same traverse with no leaveEvent in between ──────────────────
    // Belt and braces: the hand-off must not depend on the leave arriving,
    // since a dropped leave is the whole reason the third case exists.
    {
        sendEnter(fwd, fwd->mapToGlobal(fwd->rect().center()));
        check(badgeVisible(fwd), "first meter hovered again");

        sendEnter(swr, swr->mapToGlobal(swr->rect().center()));
        check(!badgeVisible(fwd),
              "entering the second meter closes the first's badge without a leave");
        check(badgeVisible(swr), "the second meter's badge is the only one up");

        sendLeave(swr);
        spin(50);
    }

    // ── the linger is the app-wide 450 ms, not a bespoke 1000 ms ─────────
    // The load-bearing half is the second one: under the old bespoke 1000 ms
    // the badge would still be up at 450 ms. The first half only shows the
    // badge does not vanish instantly, and it asserts an upper bound on
    // elapsed time that spin() cannot guarantee — so it measures and declines
    // to assert if a loaded runner overshot the linger.
    {
        sendEnter(fwd, fwd->mapToGlobal(fwd->rect().center()));
        check(badgeVisible(fwd), "hovered before the linger measurement");

        sendLeave(fwd);
        QElapsedTimer sinceLeave;
        sinceLeave.start();
        spin(DragValuePopup::kDefaultLingerMs / 4);
        if (sinceLeave.elapsed() < DragValuePopup::kDefaultLingerMs)
            check(badgeVisible(fwd),
                  "still up early in the linger — a glance still registers");
        else
            skip("still up early in the linger",
                 "runner stalled past the linger; nothing to assert");

        // The discriminating half. It has to land in the gap BETWEEN the two
        // durations: waiting "several multiples of 450" would sail past 1000
        // too, and then #3936's bespoke value would pass this just as happily.
        // So aim a little past the linger and, if a stalled runner overshoots
        // the superseded value anyway, say so rather than claim a pass that
        // proves nothing.
        while (sinceLeave.elapsed()
               < DragValuePopup::kDefaultLingerMs + kLingerSlackMs)
            spin(20);
        if (sinceLeave.elapsed() < kSupersededLingerMs)
            check(!badgeVisible(fwd),
                  "gone once the 450 ms linger elapses — at the old 1000 ms it "
                  "would still be up");
        else
            skip("gone once the 450 ms linger elapses",
                 "runner stalled past the superseded 1000 ms; cannot discriminate");
    }

    // ── a dropped physical leaveEvent must not pin the badge forever ─────
    // The recovery watchdog is armed only while the REAL pointer is over the
    // bar, so this drives it by moving the window out from under the cursor
    // rather than by faking events: park the gauge on the cursor, hover, then
    // move the host away and drop the leave entirely — what Qt does on the
    // reparent/occlude paths.
    //
    // Deliberately no setValue() calls: the point of moving recovery off the
    // value path is that a SETTLED meter recovers too. setValue() early-returns
    // on an unchanged reading, so a meter holding 0 W / 1.0 SWR — or one that
    // stopped reporting on unkey — never reached the old check.
    {
        const QPoint cursor = QCursor::pos();
        host.move(cursor.x() - 40, cursor.y() - fwd->y() - 12);
        QApplication::processEvents();

        if (!fwd->rect().contains(fwd->mapFromGlobal(cursor))) {
            skip("dropped-leave recovery",
                 "could not park the gauge under the pointer");
        } else {
            sendEnter(fwd, cursor);
            check(badgeVisible(fwd), "hovered with the pointer really inside");

            // A resting pointer must not be torn down by the watchdog.
            spin(kWatchdogMs * 3);
            check(badgeVisible(fwd),
                  "a resting pointer keeps the badge up indefinitely");

            // The pointer effectively leaves — and Qt tells us nothing.
            host.move(cursor.x() + 600, cursor.y() + 400);
            QApplication::processEvents();
            check(!fwd->rect().contains(fwd->mapFromGlobal(QCursor::pos())),
                  "the gauge really did move out from under the pointer");

            // Recovery is bounded by kHoverWatchdogMs + the linger. Poll to a
            // multiple of that: with the watchdog disarmed the badge never
            // goes away at all, so this still fails the mutant, it just does
            // not race a loaded runner to get there.
            check(badgeGoneWithin(
                      fwd, (kWatchdogMs + DragValuePopup::kDefaultLingerMs) * 4),
                  "released itself with no leaveEvent and no new value");
        }
        host.move(400, 400);
        QApplication::processEvents();
        spin(50);
    }

    // ── an INJECTED hover survives live meter frames ─────────────────────
    // The automation bridge's `hover` verb sends a QEnterEvent and a no-button
    // QMouseMove at the widget centre without moving the physical cursor, then
    // expects the badge up until it sends an explicit leave. Validating hover
    // against QCursor::pos() on every frame would tear the badge down under the
    // driver on the first changing value; this pins that it does not.
    {
        if (fwd->rect().contains(fwd->mapFromGlobal(QCursor::pos()))) {
            skip("injected hover survives meter frames",
                 "the real pointer is over the gauge, so this hover is not injected");
        } else {
            sendEnter(fwd, fwd->mapToGlobal(fwd->rect().center()));
            check(badgeVisible(fwd), "injected hover raises the badge");

            // The loop has to outlast the linger, or a teardown on the FIRST
            // frame is still on screen when the check runs and this passes for
            // the wrong reason.
            const int frames = 12;
            const int perFrameMs = 60;
            static_assert(frames * perFrameMs
                              > DragValuePopup::kDefaultLingerMs,
                          "frame burst must outlast the linger to be a test");
            for (int i = 0; i < frames; ++i) {
                fwd->setValue(30.0f + static_cast<float>(i));
                spin(perFrameMs);
            }
            check(badgeVisible(fwd),
                  "still up after changing frames — no cursor check tore it down");

            for (int i = 0; i < 5; ++i) {
                fwd->setValue(39.0f);          // repeated identical frames
                spin(40);
            }
            check(badgeVisible(fwd), "still up across repeated identical frames");

            sendLeave(fwd);
            check(badgeGoneWithin(fwd, DragValuePopup::kDefaultLingerMs * 4),
                  "an explicit leave still takes it down");
        }
    }

    // ── hiding the gauge still closes the badge, and re-hover works ──────
    // hideEvent() closes the badge AND releases the app-wide claim, but only
    // the close is observable — a claim left dangling at a hidden gauge is
    // harmless, because the next gauge's showHoverPopup() calls
    // hideHoverPopupNow() on the stale holder on its way past, and ~HGauge
    // catches whatever is left. The release is belt-and-braces, so what is
    // asserted here is the part that can actually go wrong: the badge goes
    // down, and a later gauge can still raise its own afterwards.
    {
        sendEnter(fwd, fwd->mapToGlobal(fwd->rect().center()));
        check(badgeVisible(fwd), "hovered before the gauge is hidden");

        fwd->hide();
        check(!badgeVisible(fwd), "hiding the gauge closes its badge");

        sendEnter(swr, swr->mapToGlobal(swr->rect().center()));
        check(badgeVisible(swr), "another gauge can still claim the badge after");
        check(!badgeVisible(fwd), "and the hidden gauge's badge stays down");

        sendLeave(swr);
        fwd->show();
        spin(50);
    }

    // ── disabling the readout closes it immediately ──────────────────────
    {
        sendEnter(swr, swr->mapToGlobal(swr->rect().center()));
        check(badgeVisible(swr), "hovered before the readout is disabled");

        swr->setHoverValuePopupEnabled(false);
        check(!badgeVisible(swr), "disabling the readout closes the badge");

        sendEnter(fwd, fwd->mapToGlobal(fwd->rect().center()));
        check(badgeVisible(fwd),
              "the claim was released, so the next gauge still shows");
    }

    std::printf("\n%s\n", failures == 0
                              ? "hgauge_hover_popup_test: all checks passed"
                              : "hgauge_hover_popup_test: FAILURES ABOVE");
    return failures == 0 ? 0 : 1;
}
