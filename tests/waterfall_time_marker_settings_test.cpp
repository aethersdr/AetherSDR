#include "TestSettingsProfile.h"
#include "gui/DisplaySettings.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDebug>

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("waterfall-time-marker-settings"));
    if (!profile.isValid()) { return 1; }
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
    DisplaySettings::setWaterfallTimeMarkerSeconds(1, 900);
    check(DisplaySettings::waterfallTimeMarkerSeconds(0) == 15, "first slot retained");
    check(DisplaySettings::waterfallTimeMarkerSeconds(1) == 900, "second slot independent");
    check(DisplaySettings::extendedPassband(), "sibling settings preserved");
    settings.load();
    check(DisplaySettings::waterfallTimeMarkerSeconds(0) == 15, "saved document reloads");
    DisplaySettings::setWaterfallTimeMarkerSeconds(0, 0);
    check(DisplaySettings::waterfallTimeMarkerSeconds(0) == 0, "off persists");
    check(DisplaySettings::waterfallTimeMarkerSeconds(1) == 900, "off leaves other pan alone");
    DisplaySettings::setWaterfallTimeMarkerSeconds(-1, 30);
    check(DisplaySettings::waterfallTimeMarkerSeconds(-1) == 0, "invalid slot rejected");
    const auto storedMarkers = [&settings]() {
        return QJsonDocument::fromJson(settings.value("Display").toString().toUtf8())
            .object().value("waterfallTimeMarkers").toObject();
    };
    check(!storedMarkers().contains("-1"), "invalid slot is not stored");
    DisplaySettings::setWaterfallTimeMarkerSeconds(0, 17);
    check(DisplaySettings::waterfallTimeMarkerSeconds(0) == 0, "unknown interval defaults off");
    // 30 minutes and 1 hour were retired: they exceed the retained history and
    // could never place a visible line. A stored value from an older build
    // must fail closed to Off rather than silently persisting.
    DisplaySettings::setWaterfallTimeMarkerSeconds(0, 1800);
    check(DisplaySettings::waterfallTimeMarkerSeconds(0) == 0, "retired 30-minute interval falls back to off");
    DisplaySettings::setWaterfallTimeMarkerSeconds(0, 3600);
    check(DisplaySettings::waterfallTimeMarkerSeconds(0) == 0, "retired 1-hour interval falls back to off");
    check(storedMarkers().value("0").toInt(-1) == 0, "invalid interval is stored as off");
    return failures ? 1 : 0;
}
