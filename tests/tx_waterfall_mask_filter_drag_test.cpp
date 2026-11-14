// #5204 — the TX waterfall mask must follow the TX slice's filter even when the
// radio's filterChanged echo lands while the operator is dragging a filter edge.
//
// MainWindow_Wiring.cpp's filterChanged handler returns early while
// SpectrumWidget::isDraggingFilter() is true. That guard exists for #764: the
// radio's status echo would otherwise fight the mouse and snap the dragged edge
// back. It was written for the OVERLAY. The TX-mask sync
// (syncTxWaterfallSliceToSpectrums) was added later inside the same lambda,
// BELOW the early return, so it inherited a guard that was never reasoning
// about it.
//
// Consequence: for a slice with tx=1, a filter change that arrives mid-drag
// never reaches SpectrumWidget::m_txWaterfallFilterLow/HighHz. The model holds
// the new width, the widget keeps the old one, and txWaterfallMaskRange() paints
// the TX block at a stale width. Same settings, different painted width —
// which is what "Inconsistant CW signal on waterfall" describes.
//
// Socket-free: the real MainWindow on a backend with no transport, driven by
// SliceDelta injection. Nothing keys; this is a rendering-state test.

#include "TestSettingsProfile.h"
#include "gui/MainWindow.h"
#include "gui/SpectrumWidget.h"
#include "core/AppSettings.h"
#include "core/backends/flex/FlexBackend.h"
#include "core/backends/SliceDelta.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QApplication>
#include <QCoreApplication>
#include <QEvent>
#include <QString>

#include <cstdio>
#include <cstdlib>
#include <memory>

using namespace AetherSDR;

namespace {
int failures = 0;
void check(bool ok, const char* what)
{
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) { ++failures; }
}
}

namespace AetherSDR {

// Reads the private TX-mask state the bug corrupts. txWaterfallMaskRange() is
// the function that actually decides the painted block, so the test asserts on
// it rather than on a field that merely feeds it.
struct TxMaskTestAccess {
    static bool maskRange(const SpectrumWidget& w, double& lowMhz, double& highMhz)
    {
        return w.txWaterfallMaskRange(lowMhz, highMhz);
    }
    static int maskLowHz(const SpectrumWidget& w) { return w.m_txWaterfallFilterLowHz; }
    static int maskHighHz(const SpectrumWidget& w) { return w.m_txWaterfallFilterHighHz; }
    // Puts the widget in the state a held mouse button on a filter edge creates,
    // without synthesising the whole gesture: the guard reads exactly this.
    static void beginFilterDrag(SpectrumWidget& w)
    {
        w.m_draggingFilter = SpectrumWidget::FilterEdge::High;
    }
    static void endFilterDrag(SpectrumWidget& w)
    {
        w.m_draggingFilter = SpectrumWidget::FilterEdge::None;
    }
};

// No transport. Filter writes are echoed straight back, which is what the real
// radio does and what drives SliceModel::filterChanged.
class NoRadioBackend final : public IRadioBackend {
public:
    RadioCapabilities capabilities() const override { return FlexBackend().capabilities(); }
    bool isConnected() const override { return true; }
    ReceiveControlPolicy receiveControlPolicy() const override { return ReceiveControlPolicy::Confirmed; }
    void connectRadio(const RadioConnectRequest&) override {}
    void disconnectRadio() override {}
    void setSliceFrequency(int, double) override {}
    void setSliceMode(int, const QString&) override {}
    void setSliceFilter(int id, int low, int high) override
    {
        SliceDelta delta;
        delta.filterLow = low;
        delta.filterHigh = high;
        emit sliceChanged(id, delta);
    }
    void setSliceAgc(int, const QString&, int) override {}
    void setPanCenter(const QString&, double, PanCenterIntent) override {}
    void setKeying(bool, const TxCoordinator::Operation&, const TxCoordinator::Completion&) override {}
    void invokeExtension(const QString&, const QString&, quint64, const QVariant&) override {}
};

}  // namespace AetherSDR

int main(int argc, char** argv)
{
    // Unbuffered, so the PASS/FAIL lines reach the log even if teardown dies
    // after them (see the quick_exit note at the end). Buffered, a fail-fast
    // discards the whole result and a correct run reads as a silent crash.
    setvbuf(stdout, nullptr, _IONBF, 0);
    TestSettingsProfile profile("tx-waterfall-mask-filter-drag");
    QApplication app(argc, argv);
    AppSettings::instance().setValue("AutoConnectToLastRadio", "False");

    MainWindow window;
    auto backend = std::make_unique<NoRadioBackend>();
    NoRadioBackend* radio = backend.get();
    window.radioModel().setBackendForTest(std::move(backend), QStringLiteral("flex"));
    emit window.radioModel().capabilitiesChanged(true, radio->capabilities());
    emit radio->panCenterBandwidthChanged(QStringLiteral("0x40000000"), 14.050, 0.05);

    // One CW slice that is also the TX slice — the #5204 configuration.
    SliceDelta initial;
    initial.panId = QStringLiteral("0x40000000");
    initial.letter = QStringLiteral("A");
    initial.frequency = 14.050;
    initial.mode = QStringLiteral("CWU");
    initial.modeList = QStringList{"USB", "LSB", "CW", "CWU", "CWL"};
    initial.filterLow = 50;
    initial.filterHigh = 450;   // a 400 Hz CW filter, as reported on #5204
    initial.active = true;
    initial.txSlice = true;
    emit radio->sliceChanged(0, initial);
    QCoreApplication::processEvents();

    auto* spectrum = window.findChild<SpectrumWidget*>();
    check(spectrum != nullptr, "the window has a SpectrumWidget");
    if (!spectrum) { return 1; }

    SliceModel* slice = nullptr;
    for (auto* s : window.radioModel().slices()) {
        if (s && s->isTxSlice()) { slice = s; break; }
    }
    check(slice != nullptr, "a TX slice reached the model");
    if (!slice) { return 1; }

    // Baseline: with no drag in progress the mask tracks the model.
    check(TxMaskTestAccess::maskHighHz(*spectrum) == 450,
          "baseline: the widget's TX mask holds the slice's 450 Hz filter edge");

    double lowMhz = 0.0, highMhz = 0.0;
    const bool haveBaseline = TxMaskTestAccess::maskRange(*spectrum, lowMhz, highMhz);
    check(haveBaseline, "baseline: txWaterfallMaskRange() reports a range");
    const double baselineWidthHz = (highMhz - lowMhz) * 1.0e6;
    check(baselineWidthHz > 390.0 && baselineWidthHz < 410.0,
          "baseline: the painted mask is ~400 Hz wide");

    // The operator grabs the filter edge and widens it. The radio echoes the
    // new width back DURING the drag, which is the ordinary case: the echo is
    // asynchronous and the mouse is still down.
    TxMaskTestAccess::beginFilterDrag(*spectrum);
    slice->setFilterWidth(50, 2050);        // widen to 2 kHz
    QCoreApplication::processEvents();

    check(slice->filterHigh() == 2050,
          "the model took the widened filter while the drag was in progress");

    // THE BUG: the sync below the #764 early return never ran, so the widget
    // still masks at the pre-drag width while the model says 2 kHz.
    check(TxMaskTestAccess::maskHighHz(*spectrum) == 2050,
          "#5204: the TX mask follows the filter even when the echo lands mid-drag");

    double dragLowMhz = 0.0, dragHighMhz = 0.0;
    TxMaskTestAccess::maskRange(*spectrum, dragLowMhz, dragHighMhz);
    const double dragWidthHz = (dragHighMhz - dragLowMhz) * 1.0e6;
    check(dragWidthHz > 1990.0 && dragWidthHz < 2010.0,
          "#5204: the painted TX mask is ~2 kHz, not the stale ~400 Hz");

    // Releasing must not leave the stale width behind either: nothing re-syncs
    // on drag end today, so a later unrelated echo is what eventually fixes it.
    TxMaskTestAccess::endFilterDrag(*spectrum);
    QCoreApplication::processEvents();
    check(TxMaskTestAccess::maskHighHz(*spectrum) == 2050,
          "#5204: the mask is correct once the drag ends");

    std::printf(failures == 0 ? "PASSED\n" : "FAILED (%d failures)\n", failures);

    // MainWindow's destructor expects the state app.exec() leaves behind
    // (MainWindow.cpp:4220 — "~MainWindow runs only after app.exec()"). A test
    // that never enters the event loop tears down subsystems mid-flight and
    // fail-fasts with 0xC0000409 AFTER the result is printed, which would make
    // a correct run look like a crash. Drain the queued work, then leave the
    // window alive and end the process on the result: nothing here owns a file
    // or a socket that needs closing.
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    std::fflush(stdout);
    std::quick_exit(failures == 0 ? 0 : 1);
}
