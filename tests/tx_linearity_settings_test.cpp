// TX Linearity Analyzer — owned-config-object tests (Constitution Principle V).
//
// The analyzer's settings are one nested JSON object under a single root key,
// so this proves the three properties that shape buys you and a flat namespace
// does not: it defaults as a unit when absent, it round-trips as a unit, and a
// document that has drifted — a missing field, a wrong type, a value outside
// the range its control accepts — degrades field by field instead of resetting
// the operator's whole setup or driving a control out of bounds.
//
// The range clamp is load-bearing beyond tidiness: `timeoutSec` feeds a
// transmit interlock, so a stored value that is zero, negative or larger than
// the §8 ceiling must not survive being read back.

#include "TestSettingsProfile.h"

#include "core/AppSettings.h"
#include "core/TxLinearitySettings.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>

#include <cmath>
#include <cstdio>

using namespace AetherSDR;

static int g_failures = 0;

static void check(bool cond, const char* what)
{
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

namespace {

void storeRaw(const QJsonObject& obj)
{
    AppSettings& settings = AppSettings::instance();
    settings.setValue(TxLinearitySettings::rootKey(),
                      QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact)));
    settings.save();
}

}  // namespace

int main(int argc, char** argv)
{
    // Must precede QCoreApplication and the first AppSettings touch.
    TestSettingsProfile profile(QStringLiteral("tx-linearity-settings-test"));
    QCoreApplication app(argc, argv);

    // ── Absent: the compiled-in defaults ARE the answer ─────────────────────
    {
        const TxLinearitySettings s = TxLinearitySettings::load();
        check(s.daxChannel == 1, "default DAX channel");
        check(std::abs(s.toneSpacingHz - 2000.0) < 1e-9, "default tone spacing");
        check(s.tunePowerW == 10, "default tune power");
        check(s.averages == 8, "default averages");
        check(s.timeoutSec == 10, "default transmit timeout");
        check(s.maxOrder == 7, "default highest order");
        check(s.window == QStringLiteral("blackman-harris-7"),
              "default window is the low-sidelobe one");
    }

    // ── Round-trip as a unit ────────────────────────────────────────────────
    {
        TxLinearitySettings out;
        out.daxChannel = 3;
        out.toneSpacingHz = 1500.0;
        out.tunePowerW = 25;
        out.averages = 16;
        out.timeoutSec = 4;
        out.window = QStringLiteral("blackman-harris-4");
        out.maxOrder = 5;
        out.save();

        const TxLinearitySettings back = TxLinearitySettings::load();
        check(back.daxChannel == 3, "DAX channel round-trips");
        check(std::abs(back.toneSpacingHz - 1500.0) < 1e-9, "tone spacing round-trips");
        check(back.tunePowerW == 25, "tune power round-trips");
        check(back.averages == 16, "averages round-trip");
        check(back.timeoutSec == 4, "timeout round-trips");
        check(back.window == QStringLiteral("blackman-harris-4"), "window round-trips");
        check(back.maxOrder == 5, "highest order round-trips");

        // ONE key, not seven. This is the property Principle V is actually
        // about — asserting it here stops a later change quietly re-scattering
        // the config back across the shared namespace.
        check(!AppSettings::instance()
                   .value(TxLinearitySettings::rootKey(), QString{})
                   .toString()
                   .isEmpty(),
              "the whole object lives under one root key");
        check(AppSettings::instance().value("TxLinearityAverages", QString{}).toString().isEmpty(),
              "no loose flat key is written alongside it");
    }

    // ── A partial document degrades field by field ──────────────────────────
    {
        QJsonObject partial;
        partial["averages"] = 12;   // the only field present
        storeRaw(partial);

        const TxLinearitySettings s = TxLinearitySettings::load();
        check(s.averages == 12, "a present field is read");
        check(s.timeoutSec == 10, "an absent field falls back to its default");
        check(s.window == QStringLiteral("blackman-harris-7"),
              "an absent string field falls back to its default");
    }

    // ── Wrong types fall back rather than coercing to zero ──────────────────
    //
    // A bare toInt() on a string would yield 0, and 0 averages or a 0 s
    // timeout is a meaningfully broken configuration, not a cosmetic one.
    {
        QJsonObject wrong;
        wrong["averages"] = QStringLiteral("lots");
        wrong["timeoutSec"] = QStringLiteral("forever");
        wrong["window"] = 42;
        storeRaw(wrong);

        const TxLinearitySettings s = TxLinearitySettings::load();
        check(s.averages == 8, "a string in an int field falls back, not to zero");
        check(s.timeoutSec == 10, "a string in the timeout field falls back, not to zero");
        check(s.window == QStringLiteral("blackman-harris-7"),
              "a number in the window field falls back to the default");
    }

    // ── Out-of-range values are clamped on the way in ───────────────────────
    {
        QJsonObject wild;
        wild["daxChannel"] = 99;
        wild["toneSpacingHz"] = 1.0e9;
        wild["tunePowerW"] = -5;
        wild["averages"] = 100000;
        wild["timeoutSec"] = 3600;   // an hour of continuous two-tone
        wild["maxOrder"] = 4;        // even orders are not measured here
        wild["window"] = QStringLiteral("no-such-window");
        storeRaw(wild);

        const TxLinearitySettings s = TxLinearitySettings::load();
        check(s.daxChannel >= 1 && s.daxChannel <= 4, "DAX channel is clamped to the radio's four");
        check(s.toneSpacingHz <= 10000.0, "tone spacing is clamped");
        check(s.tunePowerW >= 1, "tune power is clamped above zero");
        check(s.averages <= 64, "averages are clamped");
        // The one that matters: a stored timeout can never widen the interlock.
        check(s.timeoutSec >= 1 && s.timeoutSec <= 10,
              "a stored transmit timeout cannot exceed the safety ceiling");
        check(s.maxOrder == 7, "an even IMD order falls back to a valid odd one");
        check(s.window == QStringLiteral("blackman-harris-7"),
              "an unknown window name falls back to the default");
    }

    // ── Corrupt JSON is not fatal ───────────────────────────────────────────
    {
        AppSettings& settings = AppSettings::instance();
        settings.setValue(TxLinearitySettings::rootKey(), QStringLiteral("{not json at all"));
        settings.save();

        const TxLinearitySettings s = TxLinearitySettings::load();
        check(s.averages == 8 && s.timeoutSec == 10,
              "a corrupt document yields the defaults rather than breaking the dialog");
    }

    if (g_failures == 0) {
        std::printf("tx_linearity_settings_test: all checks passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}
