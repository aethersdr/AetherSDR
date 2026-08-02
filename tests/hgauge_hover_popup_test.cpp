// HGauge hover readout: exactly one badge on screen, and it goes away.
//
// #3936 gave every gauge its own DragValuePopup with no coordination between
// them, and lingered it for 1000 ms on leave. Traversing the stacked TX meters
// (RF Pwr and SWR are consecutive rows two pixels apart) therefore left the
// previous gauge's badge on screen alongside the new one, overlapping it.
//
// The third case is the one that outlives even that linger: linger() only
// starts a hide timer and every showValue() cancels it, while setValue()
// re-shows the badge on each meter frame for as long as m_hovered is true. One
// dropped leaveEvent — which Qt does not guarantee, and which the hideEvent
// override in HGauge.h already exists to paper over — pins the badge on screen
// indefinitely, frozen at the anchor the pointer left it at.
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

QVector<HGauge::Tick> noTicks() { return {}; }

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
    // Bracketed rather than exact so the check is about the shipped duration
    // being DragValuePopup::kDefaultLingerMs, not about timer precision.
    {
        sendEnter(fwd, fwd->mapToGlobal(fwd->rect().center()));
        check(badgeVisible(fwd), "hovered before the linger measurement");

        sendLeave(fwd);
        spin(DragValuePopup::kDefaultLingerMs / 2);
        check(badgeVisible(fwd),
              "still up halfway through the linger — a glance still registers");

        spin(DragValuePopup::kDefaultLingerMs);
        check(!badgeVisible(fwd),
              "gone once the linger elapses, well before the old 1000 ms");
    }

    // ── a dropped leaveEvent must not pin the badge forever ──────────────
    // Live meter frames arrive at 10-30 Hz while transmitting. Each one used to
    // re-show the badge and cancel the pending hide, so the readout stayed on
    // screen for as long as the radio kept reporting.
    {
        const QPoint cursor = QCursor::pos();
        const bool pointerOutside =
            !fwd->rect().contains(fwd->mapFromGlobal(cursor));
        check(pointerOutside,
              "precondition: the real pointer is not over the gauge");

        if (pointerOutside) {
            // Hover, then drop the leave entirely — exactly what Qt does on the
            // reparent/occlude paths.
            sendEnter(fwd, fwd->mapToGlobal(fwd->rect().center()));
            check(badgeVisible(fwd), "hovered before the leave is dropped");

            // Meter frames keep arriving with genuinely changing values, so the
            // badge text changes and the redundant-update cache cannot absorb
            // them.
            for (int i = 0; i < 12; ++i) {
                fwd->setValue(20.0f + static_cast<float>(i));
                spin(30);
            }
            // Past the linger, counted from the LAST frame — so the assertion
            // is "the frames stopped re-arming it", not "the frames outran a
            // timer that was already running".
            spin(DragValuePopup::kDefaultLingerMs * 2);
            check(!badgeVisible(fwd),
                  "the badge released itself despite the meter still updating");
        }
    }

    // ── hiding the gauge still closes the badge, and re-hover works ──────
    // The existing hideEvent guard now also releases the app-wide claim; if it
    // released it without hiding, or hid without releasing, the next hover on
    // another gauge would misbehave.
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
