// Global VFO marker / filter-edge defaults (#5570).
//
// Pins three things the PR review found broken in earlier revisions:
//   1. the fresh-profile marker width is 3 px, not 1 px — changing an existing
//      default silently is a user-visible regression on every untouched profile;
//   2. the defaults are ONE object under ONE root key, never loose flat keys
//      (Constitution Principle V);
//   3. writing one property preserves the other, so picking a marker size can
//      never clear a stored filter-edge default.
//
// Socket-free and headless: touches AppSettings through an isolated store and
// nothing else.

#include "TestSettingsProfile.h"

#include "gui/VfoDisplayDefaults.h"
#include "core/AppSettings.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>

#include <cstdio>
#include <cstdlib>

using namespace AetherSDR;

static int g_failures = 0;

#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);\
            ++g_failures;                                                     \
        }                                                                     \
    } while (0)

#define CHECK_EQ(actual, expected, msg)                                       \
    do {                                                                      \
        const auto a_ = (actual);                                             \
        const auto e_ = (expected);                                           \
        if (!(a_ == e_)) {                                                    \
            std::fprintf(stderr, "FAIL %s:%d: %s (actual %d, expected %d)\n", \
                         __FILE__, __LINE__, msg, int(a_), int(e_));          \
            ++g_failures;                                                     \
        }                                                                     \
    } while (0)

namespace {

QJsonObject storedDefaults()
{
    const QByteArray raw = AppSettings::instance()
        .value(QStringLiteral("VfoDisplayDefaults"), QStringLiteral("{}"))
        .toString().toUtf8();
    return QJsonDocument::fromJson(raw).object();
}

// A fresh profile must report the historical appearance, not a new one.
void testFreshProfileDefaults()
{
    CHECK_EQ(VfoDisplayDefaults::markerWidth(), 3,
             "fresh profile marker width must stay at the historical 3 px");
    CHECK(!VfoDisplayDefaults::filterEdgesHidden(),
          "fresh profile must show filter edges");
}

// Only 0 / 1 / 3 are renderable states; anything else snaps.
void testNormalization()
{
    CHECK_EQ(VfoDisplayDefaults::normalizeMarkerWidth(0), 0, "0 stays off");
    CHECK_EQ(VfoDisplayDefaults::normalizeMarkerWidth(-5), 0, "negative snaps to off");
    CHECK_EQ(VfoDisplayDefaults::normalizeMarkerWidth(1), 1, "1 stays 1");
    CHECK_EQ(VfoDisplayDefaults::normalizeMarkerWidth(2), 3, "2 snaps up to 3");
    CHECK_EQ(VfoDisplayDefaults::normalizeMarkerWidth(99), 3, "large snaps to 3");

    VfoDisplayDefaults::setMarkerWidth(2);
    CHECK_EQ(VfoDisplayDefaults::markerWidth(), 3, "stored width is normalized");
    VfoDisplayDefaults::setMarkerWidth(0);
    CHECK_EQ(VfoDisplayDefaults::markerWidth(), 0, "off round-trips");
}

// Principle V: one self-contained object under one root key, and no loose
// siblings scattered across the shared AppSettings namespace.
void testStorageShape()
{
    VfoDisplayDefaults::setMarkerWidth(1);
    VfoDisplayDefaults::setFilterEdgesHidden(true);

    const QJsonObject object = storedDefaults();
    CHECK(!object.isEmpty(), "defaults are stored as a JSON object");
    CHECK_EQ(object.value(QStringLiteral("markerWidth")).toInt(-1), 1,
             "markerWidth lives inside the object");
    CHECK(object.value(QStringLiteral("filterEdgesHidden")).toBool(false),
          "filterEdgesHidden lives inside the object");

    auto& s = AppSettings::instance();
    CHECK(!s.contains(QStringLiteral("DisplayVfoMarkerWidth")),
          "no loose flat marker-width key (Principle V)");
    CHECK(!s.contains(QStringLiteral("DisplayVfoFilterEdgesHidden")),
          "no loose flat filter-edge key (Principle V)");
    CHECK(!s.contains(QStringLiteral("VfoDefaultMarkerWidth")),
          "no loose flat marker-width key (Principle V)");
}

// Writing one property must not disturb the other — the whole point of storing
// the feature's config as a single read-modify-write document.
void testPropertiesAreIndependent()
{
    VfoDisplayDefaults::setMarkerWidth(3);
    VfoDisplayDefaults::setFilterEdgesHidden(true);
    CHECK_EQ(VfoDisplayDefaults::markerWidth(), 3, "precondition: width 3");
    CHECK(VfoDisplayDefaults::filterEdgesHidden(), "precondition: edges hidden");

    VfoDisplayDefaults::setMarkerWidth(0);
    CHECK(VfoDisplayDefaults::filterEdgesHidden(),
          "changing marker width must not clear the filter-edge default");

    VfoDisplayDefaults::setFilterEdgesHidden(false);
    CHECK_EQ(VfoDisplayDefaults::markerWidth(), 0,
             "changing filter edges must not clear the marker-width default");
}

// A corrupt or non-object value must fall back to the defaults rather than
// wedging the menu at 0 px.
void testMalformedDocumentFallsBack()
{
    auto& s = AppSettings::instance();
    s.setValue(QStringLiteral("VfoDisplayDefaults"), QStringLiteral("not json"));
    s.save();
    CHECK_EQ(VfoDisplayDefaults::markerWidth(), 3, "malformed doc falls back to 3 px");
    CHECK(!VfoDisplayDefaults::filterEdgesHidden(), "malformed doc shows edges");

    s.setValue(QStringLiteral("VfoDisplayDefaults"), QStringLiteral("[1,2,3]"));
    s.save();
    CHECK_EQ(VfoDisplayDefaults::markerWidth(), 3, "non-object doc falls back to 3 px");
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("vfo-display-defaults-test"));
    QCoreApplication app(argc, argv);

    testFreshProfileDefaults();
    testNormalization();
    testStorageShape();
    testPropertiesAreIndependent();
    testMalformedDocumentFallsBack();

    if (g_failures == 0) {
        std::printf("vfo_display_defaults_test: all assertions passed\n");
        return 0;
    }
    std::fprintf(stderr, "vfo_display_defaults_test: %d failure(s)\n", g_failures);
    return 1;
}
