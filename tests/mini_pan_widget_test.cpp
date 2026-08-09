// Offscreen tests for the mini-pan applet.
//
// Covers MiniPanScope's render API, MiniPanApplet's feed lifecycle (show/hide
// is what creates and frees the radio-side pan, so it has to be exact), and the
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
#include "gui/MiniPanScope.h"

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

    auto* label = a.findChild<QLabel*>("miniPanFreq");
    a.setCenterMhz(14.074);
    report("frequency readout follows setCenterMhz",
           label && label->text() == "14.074000",
           label ? label->text().toStdString() : "no label");
    a.setCenterMhz(0.0);
    report("no active slice → placeholder readout",
           label && label->text() == QString::fromUtf8("—.———"));
}

// The applet's visibility IS the feature's on/off switch — MainWindow creates
// and frees the dedicated radio pan on this signal, so a stuck or duplicated
// edge means either a leaked pan slot or a dead scope.
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
    // setSpanKHz() is the MainWindow-driven path (radio echo) — it must NOT
    // persist or re-emit, or the radio's own clamp would overwrite the
    // operator's choice in the store.
    report("radio-driven setSpanKHz did not overwrite the stored span",
           qFuzzyCompare(MiniPanSettings::spanKHz(),
                         MiniPanSettings::kSpanWideKHz),
           std::to_string(MiniPanSettings::spanKHz()));

    // A hand-edited out-of-range span is rejected by the settings validator,
    // so it can never reach "display pan set … bandwidth=".
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

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile("mini_pan_widget_test");
    QApplication app(argc, argv);

    testScopeApi();
    testAppletBasics();
    testFeedLifecycle();
    testSpanPersistence();
    testConfigIsOneObject();

    std::printf(g_failed ? "\n%d check(s) FAILED\n" : "\nAll checks passed\n", g_failed);
    return g_failed ? 1 : 0;
}
