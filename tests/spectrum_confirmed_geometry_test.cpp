// Opt-in real-widget test: no sockets, radio, USB, or synthetic firmware peer.
#include "TestSettingsProfile.h"
#include "gui/SpectrumWidget.h"

#include <QApplication>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QThread>
#include <cmath>
#include <cstdio>

using namespace AetherSDR;

namespace {
void mouse(SpectrumWidget& widget, QEvent::Type type, int x, int y)
{
    const QPointF local(x, y);
    QMouseEvent event(type, local, local, widget.mapToGlobal(local.toPoint()),
        type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
        type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,
        Qt::NoModifier);
    QApplication::sendEvent(&widget, &event);
}
void wait(int ms)
{
    for (int i = 0; i < ms; i += 5) {
        QApplication::processEvents();
        QThread::msleep(5);
    }
}
bool near(double a, double b) { return std::abs(a - b) < 1e-10; }
}

int main(int argc, char** argv)
{
    TestSettingsProfile profile("spectrum-confirmed-geometry");
    QApplication app(argc, argv);
    int failures = 0;
    const auto check = [&](bool ok, const char* name) {
        std::printf("[%s] %s\n", ok ? "OK" : "FAIL", name);
        failures += !ok;
    };
    SpectrumWidget widget;
    widget.resize(1200, 600);
    widget.setBandwidthLimits(0.01, 2.16);
    widget.setPanGeometryConfirmationRequired(true);
    widget.observeFrequencyRange(460.3, 0.125);
    widget.setSliceOverlay(0, 460.3, -5000, 5000, false, true, "FMN");
    double requestedCenter = 0;
    QObject::connect(&widget, &SpectrumWidget::centerChangeRequested,
        &widget, [&](double center) { requestedCenter = center; });
    widget.setFrequencyRange(460.4, 0.25);
    widget.setFrequencyRangeImmediate(460.4, 0.25);
    check(near(widget.centerMhz(), 460.3) && near(widget.bandwidthMhz(), 0.125),
          "local recenter/shortcut preview cannot replace confirmed range");
    mouse(widget, QEvent::MouseButtonPress, 200, 150);
    mouse(widget, QEvent::MouseMove, 300, 150);
    check(requestedCenter < 460.3, "real pan gesture emits intent");
    const double latestIntent = requestedCenter;
    check(near(widget.centerMhz(), 460.3), "pending drag leaves accepted axis intact");
    widget.observeFrequencyRange(460.3, 0.125); // clamp/refusal, no changed geometry
    QVector<float> bins(108, -100.0f);
    bins[40] = -30.0f;
    widget.updateSpectrum(bins);
    check(near(widget.centerMhz(), 460.3)
          && widget.automationDssSnapshot().value("dssInputFftBins").toInt() == 108,
          "refused drag keeps truthful axis AND ingests live FFT");
    widget.observeFrequencyRange(460.289453125, 0.125);
    check(near(widget.centerMhz(), 460.289453125),
          "quantized acceptance applies while pointer remains held");
    mouse(widget, QEvent::MouseButtonRelease, 300, 150);
    check(near(requestedCenter, latestIntent), "release flushes intent rather than stale accepted center");
    check(near(widget.centerMhz(), 460.289453125), "release cannot move accepted axis");
    wait(400);
    check(near(widget.centerMhz(), 460.289453125), "settle timers cannot restore optimistic center");

    double zoomCenter = 0;
    double zoomSpan = 0;
    QObject::connect(&widget, &SpectrumWidget::frequencyRangeChangeRequested,
        &widget, [&](double center, double span) { zoomCenter = center; zoomSpan = span; });
    const QPointF point(400, 150);
    QWheelEvent wheel(point, widget.mapToGlobal(point.toPoint()), {}, QPoint(0, 120),
        Qt::NoButton, Qt::ControlModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(&widget, &wheel);
    check(zoomSpan > 0 && zoomSpan < 0.125 && near(widget.bandwidthMhz(), 0.125),
          "Ctrl-wheel requests zoom without claiming unaccepted span");
    widget.observeFrequencyRange(460.29, 0.083203125);
    check(near(widget.centerMhz(), 460.29) && near(widget.bandwidthMhz(), 0.083203125),
          "accepted zoom is not rejected by settling-gesture guard");
    const double requestedZoomCenter = zoomCenter;
    const double requestedZoomSpan = zoomSpan;
    wait(400);
    check(near(zoomCenter, requestedZoomCenter) && near(zoomSpan, requestedZoomSpan),
          "zoom settle flushes requested pair without rewriting accepted pair");
    check(near(widget.centerMhz(), 460.29) && near(widget.bandwidthMhz(), 0.083203125),
          "zoom settle keeps confirmed geometry");

    // Drag the actual frequency scale. Release must retain the last request
    // even when a different quantized range arrives before the mouse-up.
    const int scaleY = (widget.spectrumPixelHeight()
        + widget.height() - widget.waterfallPixelHeight()) / 2;
    mouse(widget, QEvent::MouseButtonPress, 400, scaleY);
    mouse(widget, QEvent::MouseMove, 500, scaleY);
    const double scaleCenter = zoomCenter;
    const double scaleSpan = zoomSpan;
    check(!near(scaleSpan, 0.083203125) && near(widget.bandwidthMhz(), 0.083203125),
          "frequency-scale drag requests span while holding accepted geometry");
    widget.observeFrequencyRange(460.291, 0.1);
    mouse(widget, QEvent::MouseButtonRelease, 500, scaleY);
    check(near(zoomCenter, scaleCenter) && near(zoomSpan, scaleSpan),
          "frequency-scale release preserves final requested pair");
    check(near(widget.centerMhz(), 460.291) && near(widget.bandwidthMhz(), 0.1),
          "frequency-scale release preserves accepted geometry");

    // A queued zoom from the old session must not escape after clear/reconnect.
    wait(60);
    QApplication::sendEvent(&widget, &wheel);
    const double beforeClearRequest = zoomSpan;
    widget.clearDisplay();
    widget.observeFrequencyRange(144.5, 0.25);
    wait(400);
    check(near(zoomSpan, beforeClearRequest) && near(widget.centerMhz(), 144.5),
          "disconnect clears pending gesture intents before a new session");

    widget.observeFrequencyRange(460.3, 0.125);
    double edgeCenter = 0;
    QObject::connect(&widget, &SpectrumWidget::edgePanTuneRequested,
        &widget, [&](double center, double) { edgeCenter = center; });
    mouse(widget, QEvent::MouseButtonPress, 585, 150);
    mouse(widget, QEvent::MouseMove, 1190, 150);
    wait(150);
    check(edgeCenter > 460.3 && near(widget.centerMhz(), 460.3),
          "VFO edge pan requests capture movement without moving unaccepted axis");
    widget.observeFrequencyRange(460.301171875, 0.125);
    check(near(widget.centerMhz(), 460.301171875),
          "VFO drag accepts confirmed geometry despite echo hold");
    mouse(widget, QEvent::MouseButtonRelease, 1190, 150);

    // Same real widget with the policy disabled pins the existing Flex path.
    widget.setPanGeometryConfirmationRequired(false);
    widget.setFrequencyRangeImmediate(460.3, 0.125);
    mouse(widget, QEvent::MouseButtonPress, 200, 150);
    mouse(widget, QEvent::MouseMove, 300, 150);
    const double optimistic = widget.centerMhz();
    check(optimistic < 460.3, "Flex pan retains immediate optimistic preview");
    widget.observeFrequencyRange(460.3, 0.125);
    check(near(widget.centerMhz(), optimistic), "Flex retains stale-echo hold during drag");
    mouse(widget, QEvent::MouseButtonRelease, 300, 150);
    widget.prepareForShutdown();
    return failures ? 1 : 0;
}
