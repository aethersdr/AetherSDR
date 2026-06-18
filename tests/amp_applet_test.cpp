#include "core/AppSettings.h"
#include "gui/AmpApplet.h"

#include <QApplication>
#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QPushButton>
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

void resetSettings()
{
    auto& settings = AppSettings::instance();
    const QString path = settings.filePath();
    settings.reset();
    QFile::remove(path);
    QFile::remove(path + QStringLiteral(".bak"));
    QFile::remove(path + QStringLiteral(".tmp"));
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

} // namespace

int main(int argc, char** argv)
{
    QTemporaryDir fakeHome(QDir::tempPath() + "/aether-amp-applet-test-XXXXXX");
    if (!fakeHome.isValid()) {
        std::printf("[FAIL] create temporary home\n");
        return 1;
    }

    const QByteArray fakeHomePath = fakeHome.path().toUtf8();
    qputenv("HOME", fakeHomePath);
    qputenv("CFFIXED_USER_HOME", fakeHomePath);
    qputenv("LOCALAPPDATA", fakeHomePath);
    qputenv("XDG_CONFIG_HOME", fakeHomePath);
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QStandardPaths::setTestModeEnabled(true);

    QApplication app(argc, argv);

    const QString configRoot =
        QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    QDir(configRoot + QStringLiteral("/AetherSDR")).removeRecursively();

    std::printf("AmpApplet temperature unit test harness\n\n");

    testDefaultPlaceholder();
    testSingleSensorToggle();
    testDualSensorToggle();
    testPreferenceReload();

    std::printf("\n%s\n",
                g_failed == 0
                    ? "All tests passed."
                    : qPrintable(QStringLiteral("%1 test(s) failed.").arg(g_failed)));
    return g_failed == 0 ? 0 : 1;
}
