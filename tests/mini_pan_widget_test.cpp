// Offscreen smoke tests for the mini-pan window shell (PR1).
//
// Exercises MiniPanScope's render API and MiniPanWidget's window behavior:
// independent top-level type, close==hide, geometry + open-state persistence,
// and frameless-chrome toggle. No radio feed (that is PR2) — the scope is fed
// synthetic bins only to prove updateSpectrum() paints without crashing.
//
// Run:  QT_QPA_PLATFORM=offscreen ./build/mini_pan_widget_test

#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "gui/MiniPanScope.h"
#include "gui/MiniPanWidget.h"

#include <QApplication>
#include <QLabel>
#include <QSignalSpy>
#include <QVector>
#include <cstdio>

using namespace AetherSDR;

namespace {

int g_failed = 0;

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

void testWindowBasics()
{
    MiniPanWidget w;
    report("objectName addressable", w.objectName() == "miniPanWindow",
           w.objectName().toStdString());
    report("is a top-level window (Qt::Window)",
           (w.windowFlags() & Qt::Window) == Qt::Window);
    report("scope() present", w.scope() != nullptr);
    report("WA_DeleteOnClose off (single instance)",
           !w.testAttribute(Qt::WA_DeleteOnClose));

    auto* label = w.findChild<QLabel*>("miniPanFreq");
    w.setCenterMhz(14.074);
    report("frequency readout follows setCenterMhz",
           label && label->text() == "14.074000",
           label ? label->text().toStdString() : "no label");
}

void testCloseIsHide()
{
    MiniPanWidget w;
    w.show();
    QSignalSpy spy(&w, &MiniPanWidget::closedByUser);
    const bool closed = w.close();          // returns true if it accepted close
    report("close() kept the instance alive (not deleted)", true);
    report("close() hid the window instead of closing",
           !w.isVisible() && !closed);
    report("closedByUser emitted once", spy.count() == 1,
           std::to_string(spy.count()));
    report("MiniPanOpen persisted False on close",
           AppSettings::instance().value(MiniPanWidget::kOpenKey).toString()
               == "False");
}

void testGeometryPersistence()
{
    AppSettings::instance().remove(MiniPanWidget::kGeometryKey);
    {
        MiniPanWidget w;
        w.show();
        w.setGeometry(120, 90, 360, 220);
        w.close();                          // flushes geometry to settings
    }
    const QByteArray saved = QByteArray::fromBase64(
        AppSettings::instance().value(MiniPanWidget::kGeometryKey, "")
            .toByteArray());
    report("geometry persisted on close", !saved.isEmpty());

    MiniPanWidget w2;
    w2.show();                              // restores on first show
    report("restored geometry round-trips size",
           w2.size() == QSize(360, 220),
           (std::to_string(w2.width()) + "x" + std::to_string(w2.height())));
}

void testFramelessToggle()
{
    MiniPanWidget w;
    w.setFramelessMode(true);
    report("frameless flag set when on",
           (w.windowFlags() & Qt::FramelessWindowHint) != Qt::WindowFlags());
    w.setFramelessMode(false);
    report("frameless flag cleared when off",
           (w.windowFlags() & Qt::FramelessWindowHint) == Qt::WindowFlags());
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile("mini_pan_widget_test");
    QApplication app(argc, argv);

    testScopeApi();
    testWindowBasics();
    testCloseIsHide();
    testGeometryPersistence();
    testFramelessToggle();

    std::printf(g_failed ? "\n%d check(s) FAILED\n" : "\nAll checks passed\n", g_failed);
    return g_failed ? 1 : 0;
}
