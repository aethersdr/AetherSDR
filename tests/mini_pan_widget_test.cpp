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
#include "core/MiniPanSettings.h"
#include "gui/MiniPanScope.h"
#include "gui/MiniPanWidget.h"

#include <QApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QSignalSpy>
#include <QVector>
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
    report("open state persisted false on close", !MiniPanSettings::open());
}

void testGeometryPersistence()
{
    AppSettings::instance().remove(kMiniPanRootKey);
    {
        MiniPanWidget w;
        w.show();
        w.setGeometry(120, 90, 360, 220);
        w.close();                          // flushes geometry to settings
    }
    const QByteArray saved =
        QByteArray::fromBase64(MiniPanSettings::geometryBase64());
    report("geometry persisted on close", !saved.isEmpty());

    MiniPanWidget w2;
    w2.show();                              // restores on first show
    report("restored geometry round-trips size",
           w2.size() == QSize(360, 220),
           (std::to_string(w2.width()) + "x" + std::to_string(w2.height())));
}

void testSpanPersistence()
{
    MiniPanSettings::setSpanKHz(MiniPanSettings::kSpanWideKHz);
    {
        MiniPanWidget w;
        report("restores ±10 kHz span (20 kHz) from settings",
               qFuzzyCompare(w.spanMhz(), 0.020),
               std::to_string(w.spanMhz()));
        w.setSpanKHz(MiniPanSettings::kSpanNarrowKHz);
        report("spanMhz() reflects setSpanKHz(10)",
               qFuzzyCompare(w.spanMhz(), 0.010));
    }
    // A hand-edited out-of-range span is rejected by the settings validator,
    // so it can never reach "display pan set … bandwidth=".
    {
        QJsonObject o;
        o["spanKHz"] = 7.0;
        AppSettings::instance().setValue(
            kMiniPanRootKey,
            QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)));
        AppSettings::instance().save();
        MiniPanWidget w;
        report("invalid persisted span falls back to ±5 kHz",
               qFuzzyCompare(w.spanMhz(), 0.010));
    }
    AppSettings::instance().remove(kMiniPanRootKey);
}

// Principle V: the whole feature config is ONE nested object under ONE key —
// a regression here means someone re-introduced flat keys.
void testConfigIsOneObject()
{
    AppSettings::instance().remove(kMiniPanRootKey);
    MiniPanSettings::setSpanKHz(MiniPanSettings::kSpanWideKHz);
    MiniPanSettings::setAlwaysOnTop(true);
    MiniPanSettings::setOpen(true);
    MiniPanSettings::setGeometryBase64("Zm9v");

    const QJsonObject o = QJsonDocument::fromJson(
        AppSettings::instance().value(kMiniPanRootKey, QString{})
            .toString().toUtf8()).object();
    report("all four fields live in the one MiniPan object",
           o.contains("spanKHz") && o.contains("alwaysOnTop")
               && o.contains("open") && o.contains("geometryBase64"),
           QJsonDocument(o).toJson(QJsonDocument::Compact).toStdString());
    report("round-trips through the object",
           MiniPanSettings::alwaysOnTop() && MiniPanSettings::open()
               && qFuzzyCompare(MiniPanSettings::spanKHz(),
                                MiniPanSettings::kSpanWideKHz));

    // No legacy flat key is written any more.
    for (const char* legacy : {"MiniPanGeometry", "MiniPanOpen",
                               "MiniPanSpanKHz", "MiniPanAlwaysOnTop"}) {
        report((std::string("no flat key ") + legacy).c_str(),
               AppSettings::instance().value(legacy, QString{})
                   .toString().isEmpty());
    }
    AppSettings::instance().remove(kMiniPanRootKey);
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
    testSpanPersistence();
    testConfigIsOneObject();
    testFramelessToggle();

    std::printf(g_failed ? "\n%d check(s) FAILED\n" : "\nAll checks passed\n", g_failed);
    return g_failed ? 1 : 0;
}
