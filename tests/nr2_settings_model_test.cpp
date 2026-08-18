#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "models/Nr2SettingsModel.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSignalSpy>

#include <array>
#include <cmath>
#include <cstdio>

using namespace AetherSDR;

namespace {

int g_failures = 0;

void expect(bool condition, const char* description)
{
    std::printf("%s %s\n", condition ? "[ OK ]" : "[FAIL]", description);
    if (!condition) {
        ++g_failures;
    }
}

bool nearlyEqual(float lhs, float rhs)
{
    return std::abs(lhs - rhs) < 1.0e-5f;
}

bool runChildProfile(const QString& argument)
{
    constexpr int kChildTimeoutMs = 5000;
    QProcess process;
    process.start(QCoreApplication::applicationFilePath(), {argument});
    if (!process.waitForStarted(kChildTimeoutMs)) {
        std::fprintf(stderr, "Child %s did not start: %s\n",
                     argument.toUtf8().constData(),
                     process.errorString().toUtf8().constData());
        return false;
    }
    if (!process.waitForFinished(kChildTimeoutMs)) {
        process.kill();
        process.waitForFinished(kChildTimeoutMs);
        std::fprintf(stderr, "Child %s timed out: %s\n",
                     argument.toUtf8().constData(),
                     process.errorString().toUtf8().constData());
        return false;
    }
    if (process.exitStatus() != QProcess::NormalExit
        || process.exitCode() != 0) {
        std::fprintf(stderr,
                     "Child %s failed: status=%d code=%d error=%s\n",
                     argument.toUtf8().constData(),
                     static_cast<int>(process.exitStatus()),
                     process.exitCode(),
                     process.errorString().toUtf8().constData());
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char* argv[])
{
    TestSettingsProfile profile(QStringLiteral("aether-nr2-settings-model-test"));
    QCoreApplication app(argc, argv);
    if (!profile.isValid()) {
        std::fprintf(stderr, "Unable to create isolated settings profile\n");
        return 1;
    }

    if (app.arguments().contains(QStringLiteral("--clean-profile"))) {
        AppSettings& cleanSettings = AppSettings::instance();
        cleanSettings.load();
        const Nr2SettingsModel::Config clean =
            Nr2SettingsModel::instance().config();
        expect(clean.version == Nr2SettingsModel::kConfigVersion,
               "a clean profile starts at the current config version");
        return g_failures == 0 ? 0 : 1;
    }

    if (app.arguments().contains(QStringLiteral("--v1-profile"))) {
        AppSettings& v1Settings = AppSettings::instance();
        v1Settings.load();
        const QJsonObject v1Object{
            {QStringLiteral("version"), 1},
            {QStringLiteral("enabled"), true},
            {QStringLiteral("gainMethod"), 1},
            {QStringLiteral("gainFloor"), 0.12},
        };
        v1Settings.setValue(
            QStringLiteral("NR2"),
            QString::fromUtf8(QJsonDocument(v1Object).toJson(
                QJsonDocument::Compact)));
        v1Settings.save();

        const Nr2SettingsModel::Config migrated =
            Nr2SettingsModel::instance().config();
        expect(migrated.version == Nr2SettingsModel::kConfigVersion,
               "a v1 JSON object migrates to the current schema");
        expect(migrated.enabled && migrated.gainMethod == 1
                   && nearlyEqual(migrated.gainFloor, 0.12f),
               "v1 JSON migration preserves recognized settings");
        const QJsonDocument persisted = QJsonDocument::fromJson(
            v1Settings.value(QStringLiteral("NR2")).toString().toUtf8());
        expect(persisted.object().value(QStringLiteral("version")).toInt()
                   == Nr2SettingsModel::kConfigVersion,
               "v1 JSON migration rewrites the persisted schema version");
        return g_failures == 0 ? 0 : 1;
    }

    expect(runChildProfile(QStringLiteral("--clean-profile")),
           "a fresh settings profile reports the current schema version");
    expect(runChildProfile(QStringLiteral("--v1-profile")),
           "a version-1 JSON profile migrates without losing settings");

    AppSettings& settings = AppSettings::instance();
    settings.load();
    settings.setValue(QStringLiteral("ClientNr2Enabled"), QStringLiteral("True"));
    settings.setValue(QStringLiteral("NR2GainMethod"), QStringLiteral("1"));
    settings.setValue(QStringLiteral("NR2NpeMethod"), QStringLiteral("2"));
    settings.setValue(QStringLiteral("NR2AeFilter"), QStringLiteral("False"));
    settings.setValue(QStringLiteral("NR2GainMax"), QStringLiteral("1.25"));
    settings.setValue(QStringLiteral("NR2GainFloor"), QStringLiteral("0.10"));
    settings.setValue(QStringLiteral("NR2GainSmooth"), QStringLiteral("0.90"));
    settings.setValue(QStringLiteral("NR2Qspp"), QStringLiteral("0.35"));
    settings.setValue(QStringLiteral("NR2UseOriginalGeometry"),
                      QStringLiteral("True"));
    settings.save();

    Nr2SettingsModel& model = Nr2SettingsModel::instance();
    const Nr2SettingsModel::Config migrated = model.config();
    expect(migrated.version == 2, "migration writes config version 2");
    expect(migrated.enabled, "migration preserves enabled state");
    expect(migrated.gainMethod == 1, "migration preserves gain method");
    expect(migrated.npeMethod == 2, "migration preserves NPE method");
    expect(!migrated.aeFilter, "migration preserves AE filter state");
    expect(nearlyEqual(migrated.gainMax, 1.25f),
           "migration preserves reduction value");
    expect(nearlyEqual(migrated.gainFloor, 0.10f),
           "migration preserves naturalness value");
    expect(nearlyEqual(migrated.gainSmooth, 0.90f),
           "migration preserves smoothing value");
    expect(nearlyEqual(migrated.qspp, 0.35f),
           "migration preserves voice threshold");

    const std::array<QString, 9> legacyKeys = {
        QStringLiteral("ClientNr2Enabled"),
        QStringLiteral("NR2GainMethod"),
        QStringLiteral("NR2NpeMethod"),
        QStringLiteral("NR2AeFilter"),
        QStringLiteral("NR2GainMax"),
        QStringLiteral("NR2GainFloor"),
        QStringLiteral("NR2GainSmooth"),
        QStringLiteral("NR2Qspp"),
        QStringLiteral("NR2UseOriginalGeometry"),
    };
    bool removedLegacyKeys = true;
    for (const QString& key : legacyKeys) {
        removedLegacyKeys = removedLegacyKeys && !settings.contains(key);
    }
    expect(removedLegacyKeys,
           "migration removes every legacy flat NR2 key");

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(
        settings.value(QStringLiteral("NR2")).toString().toUtf8(),
        &parseError);
    expect(parseError.error == QJsonParseError::NoError
               && document.isObject(),
           "NR2 persists as one JSON settings object");
    expect(document.object().value(QStringLiteral("version")).toInt() == 2,
           "persisted NR2 object is versioned");
    expect(!document.object().contains(
               QStringLiteral("legacyGeometryAndGainMapping")),
           "retired original-geometry setting is not persisted");

    QSignalSpy changed(&model, &Nr2SettingsModel::configChanged);
    model.setGainFloor(0.15f);
    expect(changed.count() == 1,
           "a real setting change broadcasts one refresh signal");
    model.setGainFloor(0.15f);
    expect(changed.count() == 1,
           "an unchanged value does not broadcast a duplicate refresh");

    const QJsonDocument updated = QJsonDocument::fromJson(
        settings.value(QStringLiteral("NR2")).toString().toUtf8());
    expect(nearlyEqual(
               static_cast<float>(updated.object()
                                      .value(QStringLiteral("gainFloor"))
                                      .toDouble()),
               0.15f),
           "setting changes update the authoritative NR2 object");
    expect(nearlyEqual(Nr2SettingsModel::defaults().gainFloor, 0.0f),
           "Naturalness defaults to 0.00");

    std::printf("\n%d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
