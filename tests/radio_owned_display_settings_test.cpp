#include "TestSettingsProfile.h"
#include "gui/DisplaySettings.h"

#include <QCoreApplication>
#include <QDebug>
#include <QStringList>

using namespace AetherSDR;

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("radio-owned-display-settings"));
    if (!profile.isValid()) {
        return 1;
    }
    QCoreApplication app(argc, argv);
    AppSettings& settings = AppSettings::instance();
    settings.load();
    int failures = 0;
    const auto check = [&failures](bool ok, const char* label) {
        if (!ok) {
            qCritical() << label;
            ++failures;
        }
    };
    const QStringList retired = {
        "DisplayWnbEnabled", "DisplayWnbLevel", "DisplayFftAverage",
        "DisplayFftFps", "DisplayFftWeightedAvg", "DisplayWfLineDuration",
    };
    for (const QString& key : retired) {
        settings.setValue(key, "73");
        settings.setValue(key + "_1", "91");
    }
    settings.setValue("DisplayShowGrid", "False");
    settings.setValue("DisplayFftLineWidth_1", "2.5");
    settings.setValue("DisplayRfGain", "12");
    settings.setValue("DisplayWnbEnabled_7", "True");
    settings.save();

    DisplaySettings::retireRadioOwnedPanSettings(0);
    settings.load();
    for (const QString& key : retired) {
        check(!settings.contains(key), "primary retired key removed on disk");
        check(settings.value(key + "_1").toString() == "91", "other pan preserved");
    }
    DisplaySettings::retireRadioOwnedPanSettings(1);
    DisplaySettings::retireRadioOwnedPanSettings(1);
    DisplaySettings::retireRadioOwnedPanSettings(0);
    settings.load();
    for (const QString& key : retired) {
        check(!settings.contains(key), "primary keys stay absent after repeated cleanup");
        check(!settings.contains(key + "_1"), "secondary retired key removed on disk");
    }
    check(settings.value("DisplayShowGrid").toString() == "False", "client grid preserved");
    check(settings.value("DisplayFftLineWidth_1").toString() == "2.5", "client line width preserved");
    check(settings.value("DisplayRfGain").toString() == "12", "RF gain preserved");
    // Retirement follows SpectrumWidget's key format even for larger layouts.
    DisplaySettings::retireRadioOwnedPanSettings(7);
    settings.load();
    check(!settings.contains("DisplayWnbEnabled_7"), "higher pan slot retired");
    return failures ? 1 : 0;
}
