#include "TestSettingsProfile.h"
#include "gui/DisplaySettings.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDebug>

// Extended TNF is a global panadapter overlay preference stored beside its
// siblings in the one Display document. The risk this test guards is the
// shared document, not the boolean: extendedPassband and extendedTnf are
// independent toggles whose read-modify-write must not clobber each other.
int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("extended-tnf-settings"));
    if (!profile.isValid()) { return 1; }
    QCoreApplication app(argc, argv);
    using namespace AetherSDR;
    AppSettings& settings = AppSettings::instance();
    settings.load();
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        if (!ok) { qCritical() << label; ++failures; }
    };

    // Opt-in: an upgrade must not start painting over waterfall history.
    check(!DisplaySettings::extendedTnf(), "defaults off");

    DisplaySettings::setExtendedTnf(true);
    check(DisplaySettings::extendedTnf(), "enable retained");
    settings.load();
    check(DisplaySettings::extendedTnf(), "saved document reloads");

    // The two overlays are independent in both directions.
    DisplaySettings::setExtendedPassband(true);
    check(DisplaySettings::extendedTnf(), "passband write preserves notch overlay");
    DisplaySettings::setExtendedTnf(false);
    check(DisplaySettings::extendedPassband(), "notch write preserves passband overlay");
    check(!DisplaySettings::extendedTnf(), "disable retained");
    settings.load();
    check(!DisplaySettings::extendedTnf(), "disable survives reload");
    check(DisplaySettings::extendedPassband(), "sibling settings preserved");

    // A sibling slot-keyed setting shares the document too.
    DisplaySettings::setWaterfallTimeMarkerSeconds(0, 15);
    DisplaySettings::setExtendedTnf(true);
    check(DisplaySettings::waterfallTimeMarkerSeconds(0) == 15, "time markers preserved");
    check(DisplaySettings::extendedTnf(), "notch overlay preserved beside time markers");

    // Pin the stored spelling. Going through the accessors alone cannot catch a
    // key that was typo'd consistently in both the getter and the setter -- it
    // would round-trip perfectly here and still fail to restore a document
    // written by a correct build.
    const auto storedDisplay = [&settings]() {
        return QJsonDocument::fromJson(settings.value("Display").toString().toUtf8())
            .object();
    };
    check(storedDisplay().value("extendedTnf").toString() == QLatin1String("True"),
          "stored under the expected key, as a True/False string");

    return failures ? 1 : 0;
}
