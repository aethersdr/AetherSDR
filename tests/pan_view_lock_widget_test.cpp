// Opt-in real-widget test of Lock view's gestures: no sockets, radio or
// MainWindow. A locked SpectrumWidget must refuse every operator pan and zoom,
// stop a dragged slice at the hold window's edge, and keep a click on screen;
// each locked check has an unlocked baseline that moves.

#include "TestSettingsProfile.h"
#include "gui/SpectrumWidget.h"

#include <QApplication>
#include <QMouseEvent>
#include <QPushButton>
#include <QThread>
#include <QWheelEvent>

#include <cmath>
#include <cstdio>

using namespace AetherSDR;

namespace AetherSDR {
struct SpectrumOffscreenTestAccess {
    static int xFor(const SpectrumWidget& widget, double mhz) { return widget.mhzToX(mhz); }
    static void anchoredZoom(SpectrumWidget& widget, double factor, QPointF globalPos)
    {
        widget.applyAnchoredZoom(factor, globalPos);
    }
};
}

namespace {

int failures = 0;

void check(bool ok, const char* what)
{
    std::printf("[%s] %s\n", ok ? " OK " : "FAIL", what);
    failures += !ok;
}

void mouse(SpectrumWidget& widget, QEvent::Type type, int x, int y)
{
    const QPointF local(x, y);
    QMouseEvent event(type, local, local, widget.mapToGlobal(local.toPoint()),
        type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
        type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,
        Qt::NoModifier);
    QApplication::sendEvent(&widget, &event);
}

void ctrlWheel(SpectrumWidget& widget)
{
    const QPointF point(400, 150);
    QWheelEvent wheel(point, widget.mapToGlobal(point.toPoint()), {}, QPoint(0, 120),
        Qt::NoButton, Qt::ControlModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(&widget, &wheel);
}

void wait(int ms)
{
    for (int i = 0; i < ms; i += 5) {
        QApplication::processEvents();
        QThread::msleep(5);
    }
}

bool near(double a, double b) { return std::abs(a - b) < 1e-9; }

constexpr double kCenter = 14.100;
constexpr double kSpan = 0.200;   // 14.000 .. 14.200
constexpr int kSpecY = 150;

// Everything a gesture can ask of MainWindow, counted.
struct Requests {
    int range = 0;
    int center = 0;
    int bandwidth = 0;
    double edgePanCenter = 0.0;
    double tuned = 0.0;
    double clicked = 0.0;

    explicit Requests(SpectrumWidget& widget)
    {
        QObject::connect(&widget, &SpectrumWidget::frequencyRangeChangeRequested, &widget,
                         [this](double, double) { ++range; });
        QObject::connect(&widget, &SpectrumWidget::centerChangeRequested, &widget,
                         [this](double) { ++center; });
        QObject::connect(&widget, &SpectrumWidget::bandwidthChangeRequested, &widget,
                         [this](double) { ++bandwidth; });
        QObject::connect(&widget, &SpectrumWidget::edgePanTuneRequested, &widget,
                         [this](double c, double mhz) { edgePanCenter = c; tuned = mhz; });
        QObject::connect(&widget, &SpectrumWidget::frequencyClicked, &widget,
                         [this](double mhz) { clicked = mhz; });
    }
    int rangeChanges() const { return range + center + bandwidth; }
};

void prepare(SpectrumWidget& widget)
{
    widget.resize(1200, 600);
    widget.setBandwidthLimits(0.01, 2.16);
    widget.setFrequencyRangeImmediate(kCenter, kSpan);
    widget.setStepSize(1000);
    widget.setSliceOverlay(0, kCenter, 100, 2900, false, true, QStringLiteral("USB"));
}

// Press inside the slice's passband, then drag far past the right edge.
void dragSliceRight(SpectrumWidget& widget)
{
    const int x = SpectrumOffscreenTestAccess::xFor(widget, kCenter + 0.0015);
    mouse(widget, QEvent::MouseButtonPress, x, kSpecY);
    mouse(widget, QEvent::MouseMove, x + 300, kSpecY);
    mouse(widget, QEvent::MouseMove, widget.width() + 400, kSpecY);
    wait(300);  // an unlocked drag edge-pans on a timer
    mouse(widget, QEvent::MouseButtonRelease, widget.width() + 400, kSpecY);
}

void testRangeGestures()
{
    for (const bool locked : {false, true}) {
        SpectrumWidget widget;
        prepare(widget);
        widget.setViewLocked(locked);
        Requests requests(widget);
        const QPointF mid(widget.mapToGlobal(QPoint(400, kSpecY)));

        ctrlWheel(widget);
        wait(60);
        const int afterWheel = requests.rangeChanges();
        SpectrumOffscreenTestAccess::anchoredZoom(widget, 0.5, mid);
        wait(60);
        const int afterPinch = requests.rangeChanges();
        mouse(widget, QEvent::MouseButtonPress, 200, kSpecY);
        mouse(widget, QEvent::MouseMove, 320, kSpecY);
        mouse(widget, QEvent::MouseButtonRelease, 320, kSpecY);
        wait(60);
        const int afterPanDrag = requests.rangeChanges();
        const int scaleY = (widget.spectrumPixelHeight()
            + widget.height() - widget.waterfallPixelHeight()) / 2;
        mouse(widget, QEvent::MouseButtonPress, 400, scaleY);
        mouse(widget, QEvent::MouseMove, 520, scaleY);
        mouse(widget, QEvent::MouseButtonRelease, 520, scaleY);
        wait(450);
        const int afterScaleDrag = requests.rangeChanges();

        if (locked) {
            check(afterScaleDrag == 0,
                  "locked: Ctrl+wheel, pinch, pan drag and frequency-scale drag request nothing");
            check(near(widget.centerMhz(), kCenter) && near(widget.bandwidthMhz(), kSpan),
                  "locked: the range itself is unchanged");
        } else {
            check(afterWheel > 0, "unlocked baseline: Ctrl+wheel requests a zoom");
            check(afterPinch > afterWheel, "unlocked baseline: a pinch requests a zoom");
            check(afterPanDrag > afterPinch, "unlocked baseline: a pan drag requests a centre");
            check(afterScaleDrag > afterPanDrag,
                  "unlocked baseline: a frequency-scale drag requests a span");
        }
    }
}

void testSliceDrag()
{
    {
        SpectrumWidget widget;
        prepare(widget);
        Requests requests(widget);
        dragSliceRight(widget);
        check(requests.edgePanCenter > kCenter + 1e-6,
              "unlocked baseline: a slice dragged to the edge pans the view");
    }
    {
        SpectrumWidget widget;
        prepare(widget);
        widget.setViewLocked(true);
        Requests requests(widget);
        dragSliceRight(widget);
        check(near(requests.tuned, kCenter + kSpan / 2.0),
              "locked: the dragged slice stops on the on-step right edge");
        check(near(requests.edgePanCenter, kCenter) && near(widget.centerMhz(), kCenter),
              "locked: the drag never asks for a new centre");
    }
    {
        // A radio that holds only the middle 80% (HL2): the slice stops inside it.
        SpectrumWidget widget;
        prepare(widget);
        widget.setViewHoldFraction(0.8);
        widget.setViewLocked(true);
        Requests requests(widget);
        dragSliceRight(widget);
        check(requests.tuned > kCenter && requests.tuned < kCenter + 0.4 * kSpan,
              "locked, partial hold: the slice stops strictly inside the hold window");
        check(widget.frequencyInView(requests.tuned)
                  && !widget.frequencyInView(kCenter + 0.45 * kSpan),
              "partial hold: on screen means inside the hold window");
    }
}

void testClickNearEdge()
{
    for (const bool locked : {false, true}) {
        SpectrumWidget widget;
        prepare(widget);
        widget.setStepSize(10000);
        // 14.1955 rounds to 14.200 on a 10 kHz grid; move the view so that
        // 14.200 is off screen: 14.0005 .. 14.1995.
        widget.setFrequencyRangeImmediate(kCenter, 0.199);
        widget.setViewLocked(locked);
        Requests requests(widget);
        const QPointF local(SpectrumOffscreenTestAccess::xFor(widget, 14.1955), kSpecY);
        QMouseEvent click(QEvent::MouseButtonDblClick, local, local,
                          widget.mapToGlobal(local.toPoint()), Qt::LeftButton, Qt::LeftButton,
                          Qt::NoModifier);
        QApplication::sendEvent(&widget, &click);
        if (locked) {
            check(near(requests.clicked, 14.19) && widget.frequencyInView(requests.clicked),
                  "locked: a click near the edge lands on the last on-screen step");
        } else {
            check(near(requests.clicked, 14.2) && requests.clicked > kCenter + 0.199 / 2.0,
                  "unlocked baseline: the same click rounds to an off-screen step");
        }
    }
}

void testButtons()
{
    SpectrumWidget widget;
    prepare(widget);
    auto* lock = widget.findChild<QPushButton*>(QStringLiteral("panViewLockBtn"));
    auto* zoomIn = widget.findChild<QPushButton*>(QStringLiteral("panZoomInBtn"));
    auto* zoomBand = widget.findChild<QPushButton*>(QStringLiteral("panZoomBandBtn"));
    check(lock && zoomIn && zoomBand, "found the lock and zoom buttons");
    if (!lock || !zoomIn || !zoomBand) {
        return;
    }
    widget.setBandSegmentZoomAvailable(true);
    widget.setSpanControlPlacement(true, false);
    const QString zoomInTip = zoomIn->toolTip();
    const QString zoomBandTip = zoomBand->toolTip();

    int toggled = 0;
    QObject::connect(&widget, &SpectrumWidget::viewLockToggled, &widget,
                     [&toggled](bool) { ++toggled; });
    lock->toggle();   // keyboard, accessibility and the bridge all arrive here
    check(widget.viewLocked() && toggled == 1,
          "an operator toggle locks the view and is reported once");
    check(!zoomIn->isEnabled() && !zoomBand->isEnabled()
              && zoomIn->toolTip() == zoomBand->toolTip() && !zoomIn->toolTip().isEmpty()
              && zoomIn->accessibleDescription() == zoomIn->toolTip(),
          "locked: live zoom buttons dim and say why, on the accessible channel too");
    widget.setViewLocked(false);
    check(!widget.viewLocked() && !lock->isChecked() && toggled == 1,
          "a programmatic unlock is not reported as an operator toggle");
    check(zoomIn->isEnabled() && zoomBand->isEnabled()
              && zoomIn->toolTip() == zoomInTip && zoomBand->toolTip() == zoomBandTip,
          "unlocking gives each zoom button its own state back");

    // A pair dimmed for its own reason keeps that reason while locked.
    widget.setSpanControlPlacement(true, false, /*spanFixed=*/true);
    const QString fixedTip = zoomIn->toolTip();
    widget.setViewLocked(true);
    check(!zoomIn->isEnabled() && zoomIn->toolTip() == fixedTip && !fixedTip.isEmpty(),
          "locked: a fixed-span radio's zoom buttons keep the fixed-span reason");
    widget.setViewLocked(false);

    const QString reason = QStringLiteral("cannot hold");
    widget.setViewLockAvailable(false, reason);
    check(!lock->isEnabled() && !lock->isHidden() && lock->toolTip() == reason
              && lock->accessibleDescription() == reason && lock->statusTip().isEmpty(),
          "an unsupported radio dims the lock button with its reason, never hides it");
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile("pan-view-lock-widget");
    QApplication app(argc, argv);
    testRangeGestures();
    testSliceDrag();
    testClickNearEdge();
    testButtons();
    std::printf("pan_view_lock_widget_test: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
