// RTTY decoder enable state (#5353) — the operator's explicit "I want this
// pane" flag, stored in the feature's own nested object (Principle V).
//
// The bug this pins: RTTY pane visibility used to be derived entirely from
// slice mode, so the ✕ button lasted only until the next refresh — a slice
// switch, an active-pan change, or the rtty_mark echo the radio sends on a
// band change all recomputed `mode == "RTTY"` and put the pane back up.
// refreshRttyDecodeState() now gates on `isRtty && RttyDecodeSettings::
// enabled()`; the load-bearing property is that the disabled state SURVIVES
// those events, which here means it survives being re-read from the settings
// store the way each refresh re-reads it.
//
// The other assertion is the one the shared blob makes easy to get wrong:
// `enabled` and `sensitivity` live in the same object, so writing either must
// never drop the other (Principle XIV — the object is written whole).
//
// Runs in its own process: AppSettings is a process-wide singleton.

#include "TestSettingsProfile.h"

#include "core/AppSettings.h"
#include "gui/RttyDecodeSettings.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>

#include <cstdio>

using namespace AetherSDR;

static int g_failures = 0;
static void check(bool ok, const char* what)
{
    std::printf("[%s] %s\n", ok ? " OK " : "FAIL", what);
    if (!ok) {
        ++g_failures;
    }
}

int main(int argc, char** argv)
{
    // BEFORE QCoreApplication and before the first AppSettings touch — the
    // profile redirects the settings home, and Qt caches those paths.
    TestSettingsProfile profile(QStringLiteral("rtty-decode-settings-test"));
    QCoreApplication app(argc, argv);
    AppSettings::instance().load();

    // ---- default on an empty document --------------------------------------
    check(RttyDecodeSettings::enabled(),
          "defaults to enabled: a session that never closes the pane behaves "
          "exactly as it did before #5353");
    check(RttyDecodeSettings::sensitivity() == kRttySensitivityDefault,
          "sensitivity default is unchanged by the new field");

    // ---- the ✕ button, and its survival across refreshes --------------------
    RttyDecodeSettings::setEnabled(false);
    check(!RttyDecodeSettings::enabled(), "✕ records the operator's dismissal");

    // Every refreshRttyDecodeState() call re-reads the flag; a slice switch,
    // an active-pan change and an rttyMarkChanged echo are all just more of
    // those reads.  Re-reading must never resurrect the pane.
    for (int refresh = 0; refresh < 5; ++refresh) {
        if (RttyDecodeSettings::enabled()) {
            check(false, "disabled state must survive repeated refreshes");
            break;
        }
    }
    check(!RttyDecodeSettings::enabled(),
          "still disabled after repeated slice/frequency-driven refreshes");

    // Survives a full reload of the settings document too — the operator's
    // choice outlives the session, like the CW decode toggles.
    AppSettings::instance().load();
    check(!RttyDecodeSettings::enabled(), "disabled state persists across reload");

    // ---- the re-enable control ---------------------------------------------
    RttyDecodeSettings::setEnabled(true);
    check(RttyDecodeSettings::enabled(),
          "Radio Setup → Digital → RTTY Decode brings the pane back");

    // ---- neither field clobbers the other ----------------------------------
    RttyDecodeSettings::setSensitivity(38);
    RttyDecodeSettings::setEnabled(false);
    check(RttyDecodeSettings::sensitivity() == 38,
          "setEnabled() preserves the sibling sensitivity field");
    RttyDecodeSettings::setSensitivity(72);
    check(!RttyDecodeSettings::enabled(),
          "setSensitivity() preserves the sibling enabled field");
    check(RttyDecodeSettings::sensitivity() == 72, "sensitivity round-trips");

    // ---- stored shape: one nested object, not loose flat keys (Principle V) --
    {
        const QJsonObject o = QJsonDocument::fromJson(
            AppSettings::instance().value("RttyDecoder").toString().toUtf8()).object();
        check(o.value("enabled").toString() == QStringLiteral("False"),
              "enabled is a field of the RttyDecoder object");
        check(o.value("sensitivity").toInt() == 72,
              "sensitivity is a field of the same object");
        check(!AppSettings::instance().contains("RttyDecodeEnabled"),
              "no loose flat key was added alongside the owned object");
    }

    // ---- hand-edited / out-of-range values fail safe ------------------------
    RttyDecodeSettings::setSensitivity(-5);
    check(RttyDecodeSettings::sensitivity() == 0, "clamped below");
    RttyDecodeSettings::setSensitivity(250);
    check(RttyDecodeSettings::sensitivity() == 100, "clamped above");

    std::printf("%s (%d failures)\n", g_failures ? "FAIL" : "OK", g_failures);
    return g_failures ? 1 : 0;
}
