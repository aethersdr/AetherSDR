#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "gui/AmpApplet.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QByteArray>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QPushButton>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <cstdio>

using namespace AetherSDR;

namespace {

int g_failed = 0;

void report(const char* name, bool ok, const QString& detail = QString())
{
    std::printf("%s %-52s %s\n",
                ok ? "[ OK ]" : "[FAIL]",
                name,
                qPrintable(detail));
    if (!ok) ++g_failed;
}

QPushButton* tempButton(AmpApplet& applet)
{
    return applet.findChild<QPushButton*>(QStringLiteral("ampTempUnitButton"));
}

QComboBox* fanCombo(AmpApplet& applet)
{
    return applet.findChild<QComboBox*>(QStringLiteral("ampFanModeCombo"));
}

void resetSettings()
{
    auto& settings = AppSettings::instance();
    const QString path = settings.filePath();
    settings.reset();
    QFile::remove(path);
    QFile::remove(path + QStringLiteral(".bak"));
    QFile::remove(path + QStringLiteral(".tmp"));
    settings.load();
}

void testDefaultPlaceholder()
{
    resetSettings();

    AmpApplet applet;
    auto* button = tempButton(applet);
    report("temperature button exists", button != nullptr);
    if (!button) return;

    report("default placeholder uses Celsius",
           button->text() == QStringLiteral("\u2014 C"),
           button->text());
}

void testSingleSensorToggle()
{
    resetSettings();

    AmpApplet applet;
    auto* button = tempButton(applet);
    report("single sensor button exists", button != nullptr);
    if (!button) return;

    applet.setTemp(34.7f);
    report("single sensor displays Celsius",
           button->text() == QStringLiteral("34.7 C"),
           button->text());

    button->click();
    report("single sensor toggles to Fahrenheit",
           button->text() == QStringLiteral("94.5 F"),
           button->text());

    button->click();
    report("single sensor toggles back to Celsius",
           button->text() == QStringLiteral("34.7 C"),
           button->text());
}

void testDualSensorToggle()
{
    resetSettings();

    AmpApplet applet;
    auto* button = tempButton(applet);
    report("dual sensor button exists", button != nullptr);
    if (!button) return;

    applet.setTemp(34.7f);
    applet.setTempB(28.4f);
    report("dual sensor displays Celsius pair",
           button->text() == QStringLiteral("34.7/28.4 C"),
           button->text());

    button->click();
    report("dual sensor toggles to Fahrenheit pair",
           button->text() == QStringLiteral("94.5/83.1 F"),
           button->text());
}

void testPreferenceReload()
{
    resetSettings();

    {
        AmpApplet applet;
        auto* button = tempButton(applet);
        report("preference button exists", button != nullptr);
        if (!button) return;
        button->click();
    }

    auto& settings = AppSettings::instance();
    settings.reset();
    settings.load();

    AmpApplet restored;
    auto* button = tempButton(restored);
    report("reloaded button exists", button != nullptr);
    if (!button) return;

    report("reloaded placeholder uses Fahrenheit",
           button->text() == QStringLiteral("\u2014 F"),
           button->text());

    restored.setTemp(0.0f);
    report("reloaded value displays Fahrenheit",
           button->text() == QStringLiteral("32.0 F"),
           button->text());
}

void testFanModePulldown()
{
    resetSettings();

    AmpApplet applet;
    auto* combo = fanCombo(applet);
    report("fan combo exists", combo != nullptr);
    if (!combo) return;

    report("fan combo has three modes", combo->count() == 3, QString::number(combo->count()));
    // isVisibleTo(&applet), not isVisible(): the applet is never shown as a
    // top-level window in this offscreen harness, so isVisible() would be
    // false regardless of the combo's own shown/hidden state.
    report("fan combo starts hidden", !combo->isVisibleTo(&applet));

    QSignalSpy spy(&applet, &AmpApplet::fanModeChanged);

    // Reflecting an incoming PGXL status must select the right item, show
    // the combo, and NOT emit fanModeChanged (#3905) — that would echo a
    // redundant "setup fanmode=" command straight back to the amp.
    applet.setFanMode("contest");
    report("setFanMode selects the matching item",
           combo->currentData().toString() == QStringLiteral("CONTEST"),
           combo->currentData().toString());
    report("setFanMode shows the combo", combo->isVisibleTo(&applet));
    report("setFanMode does not emit fanModeChanged", spy.isEmpty());

    // A user-driven selection must emit the uppercase mode.
    combo->setCurrentIndex(combo->findData(QStringLiteral("BROADCAST")));
    report("user selection emits fanModeChanged", spy.count() == 1, QString::number(spy.count()));
    if (!spy.isEmpty()) {
        report("emitted mode is uppercase BROADCAST",
               spy.takeFirst().at(0).toString() == QStringLiteral("BROADCAST"));
    }

    // An unrecognized mode from the radio must not crash or desync the
    // combo's selection.
    const QString before = combo->currentData().toString();
    applet.setFanMode("bogus");
    report("unknown fanmode leaves combo selection unchanged",
           combo->currentData().toString() == before,
           combo->currentData().toString());

    // #4731: on a large-enough default UI font, the popup's fixed pixel
    // width (sized off the combo's own hardcoded 10px stylesheet font)
    // couldn't fit "Fan: Contest" — the longest item — so Qt's default
    // ElideMiddle silently mangled it. Widths/fonts aren't trustworthy to
    // assert on directly in this offscreen, unlaid-out harness (the combo
    // is never shown, so its geometry never reflects a real style pass),
    // so guard the three properties the fix actually sets instead: let the
    // widest item drive the combo's width rather than pinning it, and fail
    // any future overflow visibly (clipped) instead of mid-eliding it.
    report("fan combo sizes to its widest item, not a pinned width",
           combo->sizeAdjustPolicy() == QComboBox::AdjustToMinimumContentsLengthWithIcon);
    report("fan combo reserves room for \"Fan: Contest\"",
           combo->minimumContentsLength() >= static_cast<int>(QStringLiteral("Fan: Contest").length()),
           QString::number(combo->minimumContentsLength()));
    report("fan combo popup does not silently mid-elide overflow",
           combo->view()->textElideMode() == Qt::ElideNone);
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile settingsProfile(QStringLiteral("aether-amp-applet-test"));
    if (!settingsProfile.isValid()) {
        std::printf("[FAIL] create temporary home\n");
        return 1;
    }
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }

    QApplication app(argc, argv);

    std::printf("AmpApplet temperature unit test harness\n\n");

    testDefaultPlaceholder();
    testSingleSensorToggle();
    testDualSensorToggle();
    testPreferenceReload();
    testFanModePulldown();

    std::printf("\n%s\n",
                g_failed == 0
                    ? "All tests passed."
                    : qPrintable(QStringLiteral("%1 test(s) failed.").arg(g_failed)));
    return g_failed == 0 ? 0 : 1;
}
