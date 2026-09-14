#include "TestSettingsProfile.h"
#include "gui/DisplaySettings.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDebug>

// Waterfall scrollback retention is a global length, not a toggle, and it is
// the one Display setting whose stored value is an int rather than a
// True/False string. Two things are worth pinning beyond the round trip:
//
//   * the DEFAULT. Every sibling in this document defaults off, because they
//     are opt-in overlays. This one has shipped behaviour behind it, so its
//     default is the full 20 minutes and an upgrade must not read as an
//     operator losing their scrollback.
//   * the FALLBACK DIRECTION. An unknown value resolves to the default, never
//     to 0. Falling back to Off would turn a corrupt setting into silent data
//     loss, which is the opposite of the conservative choice.
int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("waterfall-history-retention"));
    if (!profile.isValid()) { return 1; }
    QCoreApplication app(argc, argv);
    using namespace AetherSDR;
    AppSettings& settings = AppSettings::instance();
    settings.load();
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        if (!ok) { qCritical() << label; ++failures; }
    };

    // Preserves what shipped: a fresh profile retains the full window.
    check(DisplaySettings::waterfallHistoryMinutes()
              == kDefaultWaterfallHistoryMinutes,
          "defaults to the shipped 20-minute window");
    check(kDefaultWaterfallHistoryMinutes == 20, "shipped default is 20 minutes");

    // Every offered length round-trips, Off included.
    for (const int minutes : kWaterfallHistoryMinutes) {
        DisplaySettings::setWaterfallHistoryMinutes(minutes);
        check(DisplaySettings::waterfallHistoryMinutes() == minutes,
              "offered length retained");
        settings.load();
        check(DisplaySettings::waterfallHistoryMinutes() == minutes,
              "offered length survives reload");
    }

    // Off is a real, storable choice — not the absence of a setting.
    DisplaySettings::setWaterfallHistoryMinutes(0);
    check(DisplaySettings::waterfallHistoryMinutes() == 0, "Off is retained");
    settings.load();
    check(DisplaySettings::waterfallHistoryMinutes() == 0, "Off survives reload");

    // Unknown values resolve to the default, NOT to 0. A hand-edited or
    // corrupt document must not disable scrollback behind the operator's back.
    check(validWaterfallHistoryMinutes(7) == kDefaultWaterfallHistoryMinutes,
          "unlisted length falls back to the default");
    check(validWaterfallHistoryMinutes(-5) == kDefaultWaterfallHistoryMinutes,
          "negative length falls back to the default, not to Off");
    check(validWaterfallHistoryMinutes(99999) == kDefaultWaterfallHistoryMinutes,
          "oversized length falls back to the default");
    // ...and the setter refuses to persist one either, so a bad value cannot
    // enter the document through the front door and be read back verbatim.
    DisplaySettings::setWaterfallHistoryMinutes(7);
    check(DisplaySettings::waterfallHistoryMinutes()
              == kDefaultWaterfallHistoryMinutes,
          "setter validates before storing");

    // Shares the one Display document with its siblings in both directions.
    DisplaySettings::setWaterfallHistoryMinutes(5);
    DisplaySettings::setExtendedTnf(true);
    DisplaySettings::setWaterfallTimeMarkerSeconds(0, 15);
    check(DisplaySettings::waterfallHistoryMinutes() == 5,
          "retention preserved across sibling writes");
    check(DisplaySettings::extendedTnf(),
          "sibling overlay preserved across a retention write");
    check(DisplaySettings::waterfallTimeMarkerSeconds(0) == 15,
          "slot-keyed sibling preserved across a retention write");
    DisplaySettings::setWaterfallHistoryMinutes(10);
    check(DisplaySettings::extendedTnf() && DisplaySettings::waterfallTimeMarkerSeconds(0) == 15,
          "retention write preserves both sibling shapes");

    // Pin the stored spelling and TYPE. Going through the accessors alone
    // cannot catch a key typo'd consistently in both, and this setting is the
    // only one here stored as a number — writing it as a string would
    // round-trip through toInt() as 0, i.e. silently as Off.
    const auto storedDisplay = [&settings]() {
        return QJsonDocument::fromJson(settings.value("Display").toString().toUtf8())
            .object();
    };
    const QJsonValue stored = storedDisplay().value("waterfallHistoryMinutes");
    check(stored.isDouble(), "stored as a JSON number, not a string");
    check(stored.toInt() == 10, "stored under the expected key with the set value");

    return failures ? 1 : 0;
}
