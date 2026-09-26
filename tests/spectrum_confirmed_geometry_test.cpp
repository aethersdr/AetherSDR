// Opt-in real-widget test: no sockets, radio, USB, or synthetic firmware peer.
#include "TestSettingsProfile.h"
#include "gui/SpectrumWidget.h"
#include "gui/MainWindowHelpers.h"
#include "RtlInjectedDevice.h"
#include "core/backends/flex/FlexBackend.h"
#include "core/backends/hl2/Hl2Backend.h"
#include "core/backends/rtl/RtlSdrBackend.h"
#include "models/SliceModel.h"
#include "models/PanadapterModel.h"
#include <QImage>
#include <QPainter>

#include <QApplication>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QThread>
#include <cmath>
#include <cstdio>

using namespace AetherSDR;

namespace AetherSDR {
struct SpectrumOffscreenTestAccess {
    static QPoint marker(SpectrumWidget& widget) {
        QImage image(widget.size(), QImage::Format_ARGB32);
        QPainter painter(&image);
        widget.drawOffScreenSlices(painter, QRect(0, 0, widget.width(), widget.spectrumPixelHeight()));
        return widget.m_offScreenRects.front().center();
    }
};
}
namespace AetherSDR::rtl {
struct RtlCaptureBackendTestAccess {
    static void start(RtlSdrBackend& backend, std::unique_ptr<RtlSdrWorker::Device> device) {
        backend.m_requested.hardware = {100'000'000, 2'400'000, 0, 0, 0, 240};
        backend.m_requested.receivers = {{{0, 100'000'000, -6000, 6000, 0, 3000, 3000}, RtlCaptureTransaction::Mode::Fm}};
        backend.startCapture(std::make_unique<RtlSdrWorker>(std::move(device)));
    }
    static bool idle(const RtlSdrBackend& backend) { return !backend.m_capture.busy(); }
};
}
namespace AetherSDR::hl2 {
struct Hl2DspReadbackTestAccess {
    static void receiver(Hl2Backend& backend) {
        backend.m_rx.resize(1);
        backend.m_ids.reset(1);
        backend.m_rx[0].sliceFreqHz = 14'200'000;
        backend.m_rx[0].ncoHz = 14'000'000;
    }
    static double frequency(const Hl2Backend& backend) { return backend.m_rx[0].sliceFreqHz; }
};
}
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

    int cancelled = 0;
    int released = 0;
    QObject::connect(&widget, &SpectrumWidget::sliceDragCancelled,
        &widget, [&]() { ++cancelled; });
    QObject::connect(&widget, &SpectrumWidget::sliceDragActiveChanged,
        &widget, [&](bool active) { if (!active) { ++released; } });
    widget.observeFrequencyRange(460.3, 0.125);
    mouse(widget, QEvent::MouseButtonPress, 585, 150);
    mouse(widget, QEvent::MouseMove, 1190, 150);
    wait(100);
    widget.clearDisplay();
    const double cancelledEdgeCenter = edgeCenter;
    widget.observeFrequencyRange(144.5, 0.25);
    wait(150);
    check(cancelled == 1 && released == 0,
          "VFO cancellation releases owner hold without normal-release intent");
    check(near(edgeCenter, cancelledEdgeCenter) && near(widget.centerMhz(), 144.5),
          "cancelled VFO edge timer cannot send an old-session request");

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
    // Real marker -> the owner dispatch helper -> RadioModel -> RTL worker.
    // Routing reveal as Range must fail: that request cannot leave capture.
    {
        RadioModel model;
        check(model.rebuildBackendForTest("rtl"), "offscreen RTL model initialized");
        auto& backend = *static_cast<rtl::RtlSdrBackend*>(model.backend());
        auto usb = std::make_shared<test::DeviceState>();
        rtl::RtlCaptureBackendTestAccess::start(backend, std::make_unique<test::InjectedDevice>(usb));
        usb->releaseReadback();
        const auto settle = [&]() {
            for (int i = 0; i < 2500; ++i) {
                usb->block(); QApplication::processEvents(); QThread::msleep(2);
                if (model.slice(0) && rtl::RtlCaptureBackendTestAccess::idle(backend)) { return true; }
            }
            return false;
        };
        check(settle(), "offscreen real FM receiver adopted");
        SliceModel* slice = model.slice(0);
        PanadapterModel* pan = slice ? model.panadapter(slice->panId()) : nullptr;
        if (pan) {
            SpectrumWidget reveal;
            reveal.resize(1200, 600);
            reveal.setPanGeometryConfirmationRequired(true);
            int tunes = 0, reveals = 0;
            QObject::connect(&reveal, &SpectrumWidget::frequencyClicked, &model, [&](double) { ++tunes; });
            QObject::connect(&reveal, &SpectrumWidget::offScreenSliceCenterRequested,
                &model, [&](int id) { ++reveals; requestSlicePanCenter(model, id, model.slice(id)->frequency()); });
            QObject::connect(pan, &PanadapterModel::infoChanged, &reveal,
                [&](double center, double span) { reveal.observeFrequencyRange(center, span); });
            struct RevealCase { double center; double span; bool parked; bool doubleClick; };
            for (const RevealCase fixture : {
                    RevealCase{104.0, 2.16, true, true},
                    RevealCase{96.0, 2.16, true, true},
                    RevealCase{104.0, 0.2, true, false},
                    RevealCase{96.0, 0.2, true, true},
                    RevealCase{100.3, 0.2, false, true},
                    RevealCase{99.7, 0.2, false, false}}) {
                const double center = fixture.center;
                model.requestPanCenter(pan->panId(), pan->centerMhz(), fixture.span);
                model.requestPanCenter(pan->panId(), center, -1.0, IRadioBackend::PanCenterIntent::Drag);
                check(settle() && slice->inCapture() != fixture.parked, "offscreen fixture has expected capture membership");
                reveal.observeFrequencyRange(pan->centerMhz(), pan->bandwidthMhz());
                reveal.setSliceOverlay(0, slice->frequency(), -6000, 6000, false, true, "FM");
                reveal.setSliceOverlayInCapture(0, slice->inCapture());
                const QPoint marker = SpectrumOffscreenTestAccess::marker(reveal);
                const int before = reveals;
                mouse(reveal, QEvent::MouseButtonPress, marker.x(), marker.y());
                mouse(reveal, QEvent::MouseButtonRelease, marker.x(), marker.y());
                check(settle() && slice->inCapture() && std::abs(pan->centerMhz() - 100.0) < 0.0006,
                      "indicator press resumes parked RF and centers full view within half bin");
                // Paint after adoption, so the marker has disappeared before
                // the Qt double-click and its trailing release arrive.
                SpectrumOffscreenTestAccess::marker(reveal);
                if (fixture.doubleClick) {
                    mouse(reveal, QEvent::MouseButtonDblClick, marker.x(), marker.y());
                    mouse(reveal, QEvent::MouseButtonRelease, marker.x(), marker.y());
                }
                check(reveals == before + 1 && tunes == 0 && slice->frequency() == 100.0,
                      "full Qt double-click sequence cannot retune after marker disappears");
            }
            model.requestPanCenter(pan->panId(), 104.0, 2.16, IRadioBackend::PanCenterIntent::Drag);
            check(settle() && !slice->inCapture(), "rollback fixture parks receiver");
            const double parkedCenter = pan->centerMhz();
            {
                std::lock_guard lock(usb->mutex); usb->holdReadback = true;
            }
            usb->failWriteAt = usb->writes + 5;
            requestSlicePanCenter(model, 0, 100.0);
            wait(100);
            check(pan->centerMhz() == parkedCenter && !slice->inCapture(),
                  "pending reveal cannot publish requested geometry or membership");
            usb->releaseReadback();
            check(settle() && !slice->inCapture() && pan->centerMhz() == parkedCenter
                && slice->frequency() == 100.0,
                  "failed reveal compensates and preserves accepted RF/view/membership");
            {
                std::lock_guard lock(usb->mutex); usb->holdReadback = true;
            }
            requestSlicePanCenter(model, 0, 100.0);
            wait(50);
            model.requestPanCenter(pan->panId(), 96.0, -1.0, IRadioBackend::PanCenterIntent::Drag);
            usb->releaseReadback();
            check(settle() && !slice->inCapture() && std::abs(pan->centerMhz() - 96.0) < 0.0006,
                  "newer free pan supersedes pending reveal without publishing it");
            requestSlicePanCenter(model, 0, 100.0);
            model.requestPanCenter(pan->panId(), pan->centerMhz(), 0.2);
            check(settle() && slice->inCapture() && std::abs(pan->centerMhz() - 100.0) < 0.0006
                && pan->bandwidthMhz() < 0.201,
                  "zoom coalesces with reveal placement while preserving selected RF");
            reveal.prepareForShutdown();
        }
        backend.disconnectRadio();
    }
    // Independent pan windows keep the historical Range dispatch. Offline
    // command capture and receiver state only: no Flex/Hermes connection.
    {
        FlexBackend flex;
        QStringList commands;
        flex.setSliceCommandSink([&](const QString& command) { commands << command; });
        flex.setPanCenter("0x40000000", 14'200'000, IRadioBackend::PanCenterIntent::Range);
        check(commands == QStringList{"display pan set 0x40000000 center=14.200000"},
              "Flex Range centering dispatch retains its independent pan command");
        hl2::Hl2Backend hermes;
        hl2::Hl2DspReadbackTestAccess::receiver(hermes);
        double center = 0;
        QObject::connect(&hermes, &IRadioBackend::panCenterBandwidthChanged, &hermes,
            [&](const QString&, double mhz, double) { center = mhz; });
        hermes.setPanCenter({}, 14'200'000, IRadioBackend::PanCenterIntent::Range);
        check(center == 14.2 && hl2::Hl2DspReadbackTestAccess::frequency(hermes) == 14'200'000,
              "Hermes Range centering moves its DDC pan while preserving slice RF");
    }
    widget.prepareForShutdown();
    return failures ? 1 : 0;
}
