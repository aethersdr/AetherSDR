// Offscreen tests for the mini-pan applet.
//
// Covers the re-slice mapping that IS the feature (the mini-pan creates no
// radio objects — it cuts a narrow window out of the main pan's frames),
// MiniPanScope's render API, MiniPanApplet's feed lifecycle, and the
// Constitution Principle V single-object span persistence.
//
// What is deliberately NOT here: geometry, float/dock, always-on-top and
// close==hide. Those are ContainerWidget / FloatingContainerWindow behaviour
// now, covered by container_widget_test / container_manager_test — the mini-pan
// no longer hand-rolls any of it.
//
// Run:  QT_QPA_PLATFORM=offscreen ./build/mini_pan_widget_test

#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/MiniPanSettings.h"
#include "gui/MiniPanApplet.h"
#include "gui/MiniPanReslice.h"
#include "gui/MiniPanScope.h"

#include <QApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QImage>
#include <QLabel>
#include <QSignalSpy>
#include <QVector>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

using namespace AetherSDR;

namespace {

int g_failed = 0;

// The single AppSettings key the whole feature config lives under
// (Constitution Principle V) — mirrored from MiniPanSettings.cpp so the test
// asserts against the stored shape, not just the accessors.
const char* const kMiniPanRootKey = "MiniPan";

void report(const char* name, bool ok, const std::string& detail = {})
{
    std::printf("%s %-56s %s\n", ok ? "[ OK ]" : "[FAIL]", name, detail.c_str());
    if (!ok) ++g_failed;
}

void testScopeApi()
{
    MiniPanScope scope;
    scope.resize(300, 160);
    scope.setDbmRange(-130.0f, -40.0f);
    scope.setSpanKHz(10.0);
    scope.setPassbandHz(100, 2800);        // USB
    QVector<float> bins(256);
    for (int i = 0; i < bins.size(); ++i)
        bins[i] = -120.0f + (i % 32);
    scope.updateSpectrum(bins);
    scope.grab();                           // force a paint pass offscreen
    report("scope renders synthetic bins without crash", true);

    scope.updateSpectrum(QVector<float>{}); // empty is safe
    scope.grab();
    report("scope renders empty feed without crash", true);
}

void testAppletBasics()
{
    MiniPanApplet a;
    report("objectName addressable", a.objectName() == "miniPanApplet",
           a.objectName().toStdString());
    report("scope() present", a.scope() != nullptr);
    report("fits the 260px applet panel", a.sizeHint().width() == 260,
           std::to_string(a.sizeHint().width()));

    // It must embed as an ordinary child — the container framework supplies the
    // window when the operator floats it. Constructing it under a parent is how
    // AppletPanel uses it; a widget that forced Qt::Window on itself (as the
    // MiniPanWidget it replaced did) would stay a detached top-level here.
    QWidget host;
    auto* child = new MiniPanApplet(&host);
    report("embeds as a child widget, not a detached window",
           !child->isWindow() && child->parentWidget() == &host);

    // The readout is drawn inside the scope's top row, not a QLabel above it —
    // that row was dead space above an already-short trace.
    report("no separate readout label above the scope",
           a.findChild<QLabel*>("miniPanFreq") == nullptr);
    // The readout shows the CARRIER, not the passband centre the view is built
    // around — the operator's dial frequency is the number they need, and it is
    // the frequency the hairline marks.
    a.setVfoMhz(14.074);
    a.setPassbandHz(100, 2800);          // USB: view centre is 1.45 kHz higher
    report("frequency readout follows the carrier, not the view centre",
           a.scope()->readoutText() == "14.074000",
           a.scope()->readoutText().toStdString());
    a.setVfoMhz(0.0);
    report("no active slice → placeholder readout",
           a.scope()->readoutText() == QString::fromUtf8("—.———"));

    // The scope must own the whole tile now.
    a.resize(260, 150);
    a.show();
    report("scope fills the tile (no dead row)",
           a.scope()->height() >= a.height() - 10,
           std::to_string(a.scope()->height()) + " of "
               + std::to_string(a.height()));
    a.hide();
}

// The applet's visibility IS the feature's on/off switch — MainWindow starts and
// stops consuming pan frames on this signal, so a stuck or duplicated edge means
// either a dead scope or per-frame work for a tile nobody is looking at.
void testFeedLifecycle()
{
    MiniPanApplet a;
    QSignalSpy spy(&a, &MiniPanApplet::feedWanted);

    a.show();
    report("show() requests the feed", spy.count() == 1
               && spy.at(0).at(0).toBool() == true,
           std::to_string(spy.count()));

    a.hide();
    report("hide() releases the feed", spy.count() == 2
               && spy.at(1).at(0).toBool() == false,
           std::to_string(spy.count()));

    a.show();
    report("re-show requests it again", spy.count() == 3
               && spy.at(2).at(0).toBool() == true,
           std::to_string(spy.count()));
}

void testSpanPersistence()
{
    MiniPanSettings::setSpanKHz(MiniPanSettings::kSpanWideKHz);
    {
        MiniPanApplet a;
        report("restores ±10 kHz span (20 kHz) from settings",
               qFuzzyCompare(a.spanMhz(), 0.020),
               std::to_string(a.spanMhz()));
        a.setSpanKHz(MiniPanSettings::kSpanNarrowKHz);
        report("spanMhz() reflects setSpanKHz(10)",
               qFuzzyCompare(a.spanMhz(), 0.010));
    }
    // setSpanKHz() is the MainWindow-driven path — it must NOT persist or
    // re-emit, or a programmatic update would overwrite the operator's choice.
    report("radio-driven setSpanKHz did not overwrite the stored span",
           qFuzzyCompare(MiniPanSettings::spanKHz(),
                         MiniPanSettings::kSpanWideKHz),
           std::to_string(MiniPanSettings::spanKHz()));

    // A hand-edited out-of-range span is rejected by the settings validator, so
    // the scope can only ever be asked for a span the menu offers.
    {
        QJsonObject o;
        o["spanKHz"] = 7.0;
        AppSettings::instance().setValue(
            kMiniPanRootKey,
            QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)));
        AppSettings::instance().save();
        MiniPanApplet a;
        report("invalid persisted span falls back to ±5 kHz",
               qFuzzyCompare(a.spanMhz(), 0.010));
    }
    AppSettings::instance().remove(kMiniPanRootKey);
}

// Principle V: the feature config is ONE nested object under ONE key — a
// regression here means someone re-introduced flat keys. The window-era keys
// are listed explicitly because they existed on this branch and must not
// come back; geometry/open/always-on-top now belong to the container framework.
void testConfigIsOneObject()
{
    AppSettings::instance().remove(kMiniPanRootKey);
    MiniPanSettings::setSpanKHz(MiniPanSettings::kSpanWideKHz);

    const QJsonObject o = QJsonDocument::fromJson(
        AppSettings::instance().value(kMiniPanRootKey, QString{})
            .toString().toUtf8()).object();
    report("span lives in the one MiniPan object", o.contains("spanKHz"),
           QJsonDocument(o).toJson(QJsonDocument::Compact).toStdString());

    for (const char* legacy : {"MiniPanGeometry", "MiniPanOpen",
                               "MiniPanSpanKHz", "MiniPanAlwaysOnTop"}) {
        report((std::string("no flat key ") + legacy).c_str(),
               AppSettings::instance().value(legacy, QString{})
                   .toString().isEmpty());
    }
    AppSettings::instance().remove(kMiniPanRootKey);
}

// The scope must render in the SOURCE pan's dBm window, not one it derives
// itself: a local auto-scale would disagree with the main pan about how tall a
// signal is, and would ignore the operator's FFT Floor slider entirely.
void testDbmWindowIsAuthoritative()
{
    MiniPanScope scope;
    scope.resize(300, 160);
    scope.setSpanKHz(10.0);

    // Levels INSIDE both windows — a signal pinned at the top or bottom clamps
    // identically under either range and would prove nothing.
    QVector<float> bins(64, -100.0f);
    bins[32] = -60.0f;
    scope.setDbmRange(-120.0f, -40.0f);   // -100 sits 75% down
    scope.updateSpectrum(bins);
    const QImage wide = scope.grab().toImage();

    // Same frame, half the window: -100 now sits 50% down. The trace must move,
    // proving the range drives the vertical mapping, not the bin values.
    scope.setDbmRange(-120.0f, -80.0f);
    scope.updateSpectrum(bins);
    const QImage narrow = scope.grab().toImage();

    report("dBm range drives the vertical scale", wide != narrow);

    // Feeding the same frame twice with the same range must be stable — a
    // local auto-scale would keep drifting the floor between identical frames.
    scope.updateSpectrum(bins);
    report("identical frames render identically (no local auto-scale drift)",
           scope.grab().toImage() == narrow);
}

// The trace mirrors the main pan's FFT Line / FFT Fill; the themed chrome does
// not. An invalid colour means "nothing mirrored yet" and must fall back.
void testTraceAppearanceMirrors()
{
    MiniPanScope scope;
    scope.resize(300, 160);
    scope.setDbmRange(-120.0f, -40.0f);
    QVector<float> bins(64, -100.0f);
    bins[32] = -50.0f;
    scope.updateSpectrum(bins);
    const QImage themed = scope.grab().toImage();

    scope.setTraceAppearance(QColor(255, 0, 0), QColor(255, 0, 0), 0.7f, 2.0f);
    const QImage mirrored = scope.grab().toImage();
    report("mirrored FFT Line/Fill colours change the trace",
           themed != mirrored);

    scope.setTraceAppearance(QColor(), QColor(), 0.7f, 1.2f);
    report("invalid colours fall back to the theme token",
           scope.grab().toImage() == themed);
}

// Heat Map and Grid mirror the main pan's toggles.
void testHeatMapAndGrid()
{
    MiniPanScope scope;
    scope.resize(300, 160);
    scope.setDbmRange(-120.0f, -40.0f);
    QVector<float> bins(64, -100.0f);
    bins[32] = -55.0f;
    scope.updateSpectrum(bins);

    scope.setHeatMap(false);
    scope.setShowGrid(true);
    const QImage solidGrid = scope.grab().toImage();

    scope.setHeatMap(true);
    const QImage heat = scope.grab().toImage();
    report("Heat Map changes the trace rendering", solidGrid != heat);

    scope.setHeatMap(false);
    report("Heat Map off restores the solid trace",
           scope.grab().toImage() == solidGrid);

    scope.setShowGrid(false);
    const QImage noGrid = scope.grab().toImage();
    report("Grid off changes the render", noGrid != solidGrid);
    scope.setShowGrid(true);
    report("Grid on restores it", scope.grab().toImage() == solidGrid);

    // The rules must sit on dB values, not fixed fractions of the height —
    // that is what makes them mean the same thing as the main pan's. Changing
    // the dBm window by a non-integer number of steps has to move them.
    scope.setDbmRange(-117.0f, -37.0f);   // same 80 dB span, shifted by 3 dB
    scope.updateSpectrum(bins);
    const QImage shifted = scope.grab().toImage();
    scope.setShowGrid(false);
    scope.updateSpectrum(bins);
    const QImage shiftedNoGrid = scope.grab().toImage();
    report("grid lines are dB-aligned, not fixed fractions",
           shifted != shiftedNoGrid);
    scope.setShowGrid(true);
}

// Which frequency ends up in the middle. On SSB the carrier sits at the edge of
// the filter, so centring on it spent half of an already narrow view on the
// sideband the filter throws away — the window centres on the passband instead.
// MainWindow cuts the re-slice around this offset and MiniPanScope places the
// hairline and the wash with it, so a regression here desynchronises the trace
// from the chrome as well as mis-centring both.
void testPassbandCentreOffset()
{
    using AetherSDR::MiniPan::passbandCenterOffsetHz;

    report("USB 100..2800 centres 1450 Hz above the carrier",
           qFuzzyCompare(passbandCenterOffsetHz(100, 2800), 1450.0),
           std::to_string(passbandCenterOffsetHz(100, 2800)));
    report("LSB -2800..-100 centres 1450 Hz below the carrier",
           qFuzzyCompare(passbandCenterOffsetHz(-2800, -100), -1450.0),
           std::to_string(passbandCenterOffsetHz(-2800, -100)));

    // A filter that straddles the carrier (AM/FM, and CW filters centred on the
    // pitch) is unaffected — the view stays exactly where it was before.
    report("symmetric passband still centres on the carrier",
           qFuzzyIsNull(passbandCenterOffsetHz(-3000, 3000)));
    // No slice, or filter edges not known yet: fall back to the carrier rather
    // than inventing an offset from nonsense.
    report("hidden passband falls back to the carrier",
           qFuzzyIsNull(passbandCenterOffsetHz(0, 0))
               && qFuzzyIsNull(passbandCenterOffsetHz(2800, 100)));
}

// The render side of the same rule: the wash must land symmetrically about the
// middle of the widget, and the hairline must leave the middle to stay on the
// carrier. Both are drawn from carrier-relative offsets, so an unshifted one
// would put the band off-centre or the hairline on the wrong frequency.
void testPassbandCentredRender()
{
    MiniPanScope scope;
    scope.resize(300, 160);
    scope.setDbmRange(-120.0f, -40.0f);
    scope.setSpanKHz(10.0);              // ±5 kHz across 300 px → 30 px per kHz
    scope.setShowGrid(false);            // keep the field plain to sample against
    scope.setPassbandHz(100, 2800);      // USB: 2.7 kHz wide, centred 1450 Hz up
    const QImage usb = scope.grab().toImage();

    // Band half-width 1350 Hz → x 109.5..190.5 about the centre at 150. Sample
    // well inside and well outside, clear of both edges and of the carrier
    // hairline (1450 Hz below centre → x ≈ 106).
    const int y = scope.height() - 5;    // below the top label row
    const QRgb field = usb.pixel(30, y);
    report("passband wash is centred on the widget",
           usb.pixel(130, y) != field && usb.pixel(130, y) == usb.pixel(170, y));
    report("outside the passband is plain field",
           usb.pixel(70, y) == field && usb.pixel(230, y) == field);

    // Same filter WIDTH, straddling the carrier: the wash is identical geometry,
    // so the only thing that can differ is where the hairline is — which is the
    // assertion. If the hairline were still pinned to the middle these would
    // compare equal.
    scope.setPassbandHz(-1350, 1350);
    report("carrier hairline moves off centre on a sideband filter",
           scope.grab().toImage() != usb);
}

// The whole mini-pan data path: cut a narrow window out of a main-pan frame.
// No radio object is created, so this mapping is the feature.
void testReslice()
{
    using AetherSDR::MiniPan::resliceWindow;

    // Main pan: 200 kHz wide centred on 14.100, 2800 bins (a real xpixels).
    const double panBw = 0.200, panLo = 14.100 - panBw / 2.0;
    const int    n = 2800;
    QVector<float> bins(n, -120.0f);
    // A single spike on one source bin. Derive that bin's exact frequency
    // rather than assuming a round number — the bin grid is quantised, so a
    // spike "at 14.100" actually sits a fraction of a bin off it, and an
    // assertion against the round number would be testing the quantisation
    // rather than the mapping.
    const int    spikeIdx = n / 2;
    const double spikeMhz = panLo + (panBw * spikeIdx) / (n - 1);
    bins[spikeIdx] = -40.0f;

    // ±5 kHz window centred on the spike: it must come back dead centre.
    const double span = 0.010;
    constexpr int kOut = 512;
    auto out = resliceWindow(bins, panLo, panBw, spikeMhz - span / 2.0, span,
                             kOut, -140.0f);
    report("reslice returns the requested width", out.size() == kOut,
           std::to_string(out.size()));

    // An off-by-one here puts a signal at a frequency it is not on, which is
    // the one thing a tuning aid must never do. The centre falls between the
    // two middle samples, so either is correct.
    const int peak = static_cast<int>(
        std::max_element(out.cbegin(), out.cend()) - out.cbegin());
    const int mid = (kOut - 1) / 2;
    report("signal at the VFO lands at the centre of the window",
           peak == mid || peak == mid + 1, std::to_string(peak));

    // A window sitting entirely outside the pan is floor, not smeared edge data.
    auto off = resliceWindow(bins, panLo, panBw, 21.000, span, 512, -140.0f);
    const bool allFloor = std::all_of(off.cbegin(), off.cend(),
                                      [](float v) { return v == -140.0f; });
    report("window outside the pan reads as floor, not clamped edge", allFloor);

    // Half-overlapping window: the half inside carries data, the half outside
    // is floor — proving we pad rather than stretch.
    auto edge = resliceWindow(bins, panLo, panBw, panLo - span / 2.0, span,
                              512, -140.0f);
    report("window straddling the pan edge pads the outside half",
           edge.first() == -140.0f && edge.last() > -140.0f);

    // Degenerate inputs must not crash or return garbage.
    report("empty source returns empty",
           resliceWindow(QVector<float>{}, panLo, panBw, 14.1, span, 512,
                         -140.0f).isEmpty());
    report("zero-bandwidth pan returns empty",
           resliceWindow(bins, panLo, 0.0, 14.1, span, 512, -140.0f).isEmpty());
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile("mini_pan_widget_test");
    QApplication app(argc, argv);

    testScopeApi();
    testDbmWindowIsAuthoritative();
    testTraceAppearanceMirrors();
    testHeatMapAndGrid();
    testPassbandCentreOffset();
    testPassbandCentredRender();
    testReslice();
    testAppletBasics();
    testFeedLifecycle();
    testSpanPersistence();
    testConfigIsOneObject();

    std::printf(g_failed ? "\n%d check(s) FAILED\n" : "\nAll checks passed\n", g_failed);
    return g_failed ? 1 : 0;
}
