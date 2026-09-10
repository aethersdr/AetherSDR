#include "gui/DisplaySettings.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QDebug>

int main(int argc, char** argv)
{
    QTemporaryDir settingsDirectory;
    if (!settingsDirectory.isValid()) { return 1; }
    qputenv("AETHER_SETTINGS_DIR", settingsDirectory.path().toUtf8());
    QCoreApplication app(argc, argv);
    using namespace AetherSDR;
    AppSettings& settings = AppSettings::instance();
    settings.load();
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        if (!ok) { qCritical() << label; ++failures; }
    };
    check(DisplaySettings::waterfallTimeMarkerSeconds(0) == 0, "defaults off");
    DisplaySettings::setExtendedPassband(true);
    DisplaySettings::setWaterfallTimeMarkerSeconds(0, 15);
    DisplaySettings::setWaterfallTimeMarkerSeconds(1, 3600);
    check(DisplaySettings::waterfallTimeMarkerSeconds(0) == 15, "first slot retained");
    check(DisplaySettings::waterfallTimeMarkerSeconds(1) == 3600, "second slot independent");
    check(DisplaySettings::extendedPassband(), "sibling settings preserved");
    settings.load();
    check(DisplaySettings::waterfallTimeMarkerSeconds(0) == 15, "saved document reloads");
    DisplaySettings::setWaterfallTimeMarkerSeconds(0, 0);
    check(DisplaySettings::waterfallTimeMarkerSeconds(0) == 0, "off persists");
    check(DisplaySettings::waterfallTimeMarkerSeconds(1) == 3600, "off leaves other pan alone");
    DisplaySettings::setWaterfallTimeMarkerSeconds(-1, 30);
    check(DisplaySettings::waterfallTimeMarkerSeconds(-1) == 0, "invalid slot rejected");
    DisplaySettings::setWaterfallTimeMarkerSeconds(0, 17);
    check(DisplaySettings::waterfallTimeMarkerSeconds(0) == 0, "unknown interval defaults off");
    return failures ? 1 : 0;
}
