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
               vuSettings.peakDecayRate == QStringLiteral("Medium"));

    const CrossNeedleMeterSettingsCodec::Snapshot powerSettings;
    report("independent PWR meter defaults to dark-room uplight",
           powerSettings.faceTheme ==
               CrossNeedleMeterSettingsCodec::kUplightTheme);
}

void testIndependentSettingsRoundTrip()
{
    VuMeterSettingsCodec::Snapshot vuBefore;
    vuBefore.txSelect = 2;
    vuBefore.rxSelect = 1;
    vuBefore.peakHoldEnabled = true;
    vuBefore.peakDecayRate = QStringLiteral("Slow");

    QString vuError;
    const VuMeterSettingsCodec::Snapshot vuAfter =
        VuMeterSettingsCodec::decode(
            VuMeterSettingsCodec::encode(vuBefore).toUtf8(), &vuError);
    report("standard meter controls round-trip independently",
           vuError.isEmpty() && vuAfter.txSelect == vuBefore.txSelect &&
               vuAfter.rxSelect == vuBefore.rxSelect &&
               vuAfter.peakHoldEnabled == vuBefore.peakHoldEnabled &&
               vuAfter.peakDecayRate == vuBefore.peakDecayRate);

    const QStringList themes{
        CrossNeedleMeterSettingsCodec::kClassicTheme,
        CrossNeedleMeterSettingsCodec::kUplightTheme,
        CrossNeedleMeterSettingsCodec::kDarkTheme};
    bool themesPreserved = true;
    for (const QString& theme : themes) {
        CrossNeedleMeterSettingsCodec::Snapshot before;
        before.faceTheme = theme;
        QString error;
        const CrossNeedleMeterSettingsCodec::Snapshot after =
            CrossNeedleMeterSettingsCodec::decode(
                CrossNeedleMeterSettingsCodec::encode(before).toUtf8(),
                &error);
        themesPreserved = themesPreserved && error.isEmpty() &&
            after.faceTheme == theme;
    }
    report("PWR applet face themes round-trip independently", themesPreserved);
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
    report("rewritten VuMeter object is version 2 and standard-only",
           rewritten.value(QStringLiteral("version")).toInt() == 2 &&
               !rewritten.contains(QStringLiteral("style")) &&
               !rewritten.contains(QStringLiteral("crossNeedle")));

    const CrossNeedleMeterSettingsCodec::Snapshot migratedPower =
        CrossNeedleMeterSettingsCodec::migrateLegacyTheme(
            legacyCrossNeedle.faceTheme);
    report("legacy face theme moves to the independent PWR object",
           migratedPower.faceTheme ==
               CrossNeedleMeterSettingsCodec::kDarkTheme);
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
               migrated.peakDecayRate == QStringLiteral("Slow"));

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
               malformed.peakDecayRate == QStringLiteral("Medium"));

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
               CrossNeedleMeterSettingsCodec::kUplightTheme);
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    testNewUserDefaults();
    testIndependentSettingsRoundTrip();
    testCombinedVersionOneMigration();
    testFlatLegacyMigration();
    testInvalidPackedSettingsFailSafe();
    return g_failed == 0 ? 0 : 1;
}
