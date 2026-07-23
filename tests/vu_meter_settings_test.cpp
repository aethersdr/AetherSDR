#include "gui/CrossNeedleMeterSettings.h"
#include "gui/VuMeterSettings.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>

#include <cstdio>

using namespace AetherSDR;

namespace {

int g_failed = 0;

void report(const char* name, bool passed)
{
    std::printf("[%s] %s\n", passed ? " OK " : "FAIL", name);
    if (!passed) {
        ++g_failed;
    }
}

void testNewUserDefaults()
{
    const VuMeterSettingsCodec::Snapshot vuSettings;
    report("standard meter retains its established new-user defaults",
           vuSettings.txSelect == 0 && vuSettings.rxSelect == 0 &&
               !vuSettings.peakHoldEnabled &&
               vuSettings.peakDecayRate == QStringLiteral("Medium") &&
               vuSettings.faceTheme == VuMeterSettingsCodec::kAetherTheme);

    const CrossNeedleMeterSettingsCodec::Snapshot powerSettings;
    report("independent PWR meter defaults to uplight with Range visible",
           powerSettings.faceTheme ==
               CrossNeedleMeterSettingsCodec::kUplightTheme &&
               powerSettings.showRange);
}

void testIndependentSettingsRoundTrip()
{
    VuMeterSettingsCodec::Snapshot vuBefore;
    vuBefore.txSelect = 2;
    vuBefore.rxSelect = 1;
    vuBefore.peakHoldEnabled = true;
    vuBefore.peakDecayRate = QStringLiteral("Slow");
    vuBefore.faceTheme = VuMeterSettingsCodec::kDarkTheme;

    QString vuError;
    const VuMeterSettingsCodec::Snapshot vuAfter =
        VuMeterSettingsCodec::decode(
            VuMeterSettingsCodec::encode(vuBefore).toUtf8(), &vuError);
    report("standard meter controls round-trip independently",
           vuError.isEmpty() && vuAfter.txSelect == vuBefore.txSelect &&
               vuAfter.rxSelect == vuBefore.rxSelect &&
               vuAfter.peakHoldEnabled == vuBefore.peakHoldEnabled &&
               vuAfter.peakDecayRate == vuBefore.peakDecayRate &&
               vuAfter.faceTheme == vuBefore.faceTheme);

    bool standardThemesPreserved = true;
    for (const QString& theme : VuMeterSettingsCodec::faceThemeItems()) {
        VuMeterSettingsCodec::Snapshot before;
        before.faceTheme = theme;
        QString error;
        const VuMeterSettingsCodec::Snapshot after =
            VuMeterSettingsCodec::decode(
                VuMeterSettingsCodec::encode(before).toUtf8(), &error);
        standardThemesPreserved = standardThemesPreserved
            && error.isEmpty() && after.faceTheme == theme;
    }
    report("standard S-meter face themes round-trip", standardThemesPreserved);

    const QStringList themes{
        CrossNeedleMeterSettingsCodec::kClassicTheme,
        CrossNeedleMeterSettingsCodec::kUplightTheme,
        CrossNeedleMeterSettingsCodec::kDarkTheme};
    bool themesPreserved = true;
    bool currentEncodingComplete = true;
    for (const QString& theme : themes) {
        CrossNeedleMeterSettingsCodec::Snapshot before;
        before.faceTheme = theme;
        before.showRange = theme != CrossNeedleMeterSettingsCodec::kDarkTheme;
        const QString encoded =
            CrossNeedleMeterSettingsCodec::encode(before);
        const QJsonObject encodedRoot =
            QJsonDocument::fromJson(encoded.toUtf8()).object();
        currentEncodingComplete = currentEncodingComplete &&
            encodedRoot.value(QStringLiteral("version")).toInt() ==
                CrossNeedleMeterSettingsCodec::kVersion &&
            encodedRoot.value(QStringLiteral("showRange")).toBool() ==
                before.showRange;
        QString error;
        const CrossNeedleMeterSettingsCodec::Snapshot after =
            CrossNeedleMeterSettingsCodec::decode(
                encoded.toUtf8(), &error);
        themesPreserved = themesPreserved && error.isEmpty() &&
            after.faceTheme == theme && after.showRange == before.showRange;
    }
    report("PWR applet theme and Show Range round-trip independently",
           themesPreserved);
    report("PWR settings encode the current version and Show Range field",
           currentEncodingComplete);

    QString legacyError;
    const CrossNeedleMeterSettingsCodec::Snapshot versionOne =
        CrossNeedleMeterSettingsCodec::decode(
            QByteArrayLiteral(
                "{\"version\":1,\"faceTheme\":\"graphite-dark\"}"),
            &legacyError);
    report("version-1 PWR settings migrate with Range visible",
           legacyError.isEmpty() &&
               versionOne.faceTheme ==
                   CrossNeedleMeterSettingsCodec::kDarkTheme &&
               versionOne.showRange);
}

void testCombinedVersionOneMigration()
{
    const QByteArray versionOne = QByteArrayLiteral(
        "{\"version\":1,\"style\":\"cross-needle\","
        "\"standard\":{\"txSelect\":2,\"rxSelect\":1,"
        "\"peakHoldEnabled\":true,\"peakDecayRate\":\"Fast\"},"
        "\"crossNeedle\":{\"faceTheme\":\"graphite-dark\"}}");

    QString error;
    VuMeterSettingsCodec::LegacyCrossNeedle legacyCrossNeedle;
    const VuMeterSettingsCodec::Snapshot standard =
        VuMeterSettingsCodec::decode(
            versionOne, &error, &legacyCrossNeedle);
    report("version-1 combined object retains standard meter controls",
           error.isEmpty() && standard.txSelect == 2 &&
               standard.rxSelect == 1 && standard.peakHoldEnabled &&
               standard.peakDecayRate == QStringLiteral("Fast"));
    report("version-1 combined object exposes its PWR theme for migration",
           legacyCrossNeedle.present && legacyCrossNeedle.selected &&
               legacyCrossNeedle.faceTheme == QStringLiteral("graphite-dark"));

    const QJsonObject rewritten = QJsonDocument::fromJson(
        VuMeterSettingsCodec::encode(standard).toUtf8()).object();
    report("rewritten VuMeter object is version 3 and standard-only",
           rewritten.value(QStringLiteral("version")).toInt() == 3 &&
               !rewritten.contains(QStringLiteral("style")) &&
               !rewritten.contains(QStringLiteral("crossNeedle")) &&
               rewritten.value(QStringLiteral("standard")).toObject()
                       .value(QStringLiteral("faceTheme")).toString()
                   == VuMeterSettingsCodec::kAetherTheme);

    const CrossNeedleMeterSettingsCodec::Snapshot migratedPower =
        CrossNeedleMeterSettingsCodec::migrateLegacyTheme(
            legacyCrossNeedle.faceTheme);
    report("legacy face theme moves to the independent PWR object",
           migratedPower.faceTheme ==
               CrossNeedleMeterSettingsCodec::kDarkTheme &&
               migratedPower.showRange);
}

void testVersionTwoMigration()
{
    const QByteArray versionTwo = QByteArrayLiteral(
        "{\"version\":2,\"standard\":{\"txSelect\":3,\"rxSelect\":1,"
        "\"peakHoldEnabled\":true,\"peakDecayRate\":\"Slow\"}}");
    QString error;
    const VuMeterSettingsCodec::Snapshot migrated =
        VuMeterSettingsCodec::decode(versionTwo, &error);
    report("version-2 standard settings migrate without changing controls",
           error.isEmpty() && migrated.txSelect == 3 && migrated.rxSelect == 1
               && migrated.peakHoldEnabled
               && migrated.peakDecayRate == QStringLiteral("Slow"));
    report("version-2 settings adopt the established Aether face",
           migrated.faceTheme == VuMeterSettingsCodec::kAetherTheme);

    const QByteArray invalidTheme = QByteArrayLiteral(
        "{\"version\":3,\"standard\":{\"faceTheme\":\"not-a-theme\"}}");
    const VuMeterSettingsCodec::Snapshot bounded =
        VuMeterSettingsCodec::decode(invalidTheme, &error);
    report("unknown standard face themes fall back safely",
           error.isEmpty()
               && bounded.faceTheme == VuMeterSettingsCodec::kAetherTheme);
}

void testFlatLegacyMigration()
{
    const VuMeterSettingsCodec::Snapshot migrated =
        VuMeterSettingsCodec::migrateLegacy(
            99, -4, true, QStringLiteral("Slow"));
    report("flat legacy settings are bounded and retain valid choices",
           migrated.txSelect ==
                   VuMeterSettingsCodec::txMeterItems().size() - 1 &&
               migrated.rxSelect == 0 && migrated.peakHoldEnabled &&
               migrated.peakDecayRate == QStringLiteral("Slow") &&
               migrated.faceTheme == VuMeterSettingsCodec::kAetherTheme);

    const VuMeterSettingsCodec::Snapshot invalidDecay =
        VuMeterSettingsCodec::migrateLegacy(
            0, 0, false, QStringLiteral("Unknown"));
    report("invalid legacy decay falls back to Medium",
           invalidDecay.peakDecayRate == QStringLiteral("Medium"));
}

void testInvalidPackedSettingsFailSafe()
{
    QString malformedError;
    const VuMeterSettingsCodec::Snapshot malformed =
        VuMeterSettingsCodec::decode(
            QByteArrayLiteral("{"), &malformedError);
    report("malformed VuMeter settings return safe defaults",
           !malformedError.isEmpty() && malformed.txSelect == 0 &&
               malformed.rxSelect == 0 &&
               malformed.peakDecayRate == QStringLiteral("Medium") &&
               malformed.faceTheme == VuMeterSettingsCodec::kAetherTheme);

    QString versionError;
    const VuMeterSettingsCodec::Snapshot unsupported =
        VuMeterSettingsCodec::decode(
            QByteArrayLiteral("{\"version\":99}"), &versionError);
    report("unsupported VuMeter versions return safe defaults",
           !versionError.isEmpty() && unsupported.txSelect == 0 &&
               unsupported.rxSelect == 0);

    QString powerError;
    const CrossNeedleMeterSettingsCodec::Snapshot badPower =
        CrossNeedleMeterSettingsCodec::decode(
            QByteArrayLiteral("{\"version\":99}"), &powerError);
    report("invalid PWR settings return the uplight default",
           !powerError.isEmpty() && badPower.faceTheme ==
               CrossNeedleMeterSettingsCodec::kUplightTheme &&
               badPower.showRange);
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    testNewUserDefaults();
    testIndependentSettingsRoundTrip();
    testCombinedVersionOneMigration();
    testVersionTwoMigration();
    testFlatLegacyMigration();
    testInvalidPackedSettingsFailSafe();
    return g_failed == 0 ? 0 : 1;
}
