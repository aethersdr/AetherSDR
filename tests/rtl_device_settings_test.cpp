#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/RtlDeviceSettings.h"

#include <QCoreApplication>
#include <cstdio>

using namespace AetherSDR;
namespace {
int failures = 0;
void check(bool value, const char* message)
{
    if (!value) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); }
}
}

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("rtl-device-settings"));
    if (!profile.isValid()) { return 1; }
    QCoreApplication app(argc, argv);
    AppSettings::instance().load();
    using Settings = RtlDeviceSettings;
    const RadioSettingsScope first(QStringLiteral("rtl"), QStringLiteral("receiver-A"));
    const RadioSettingsScope second(QStringLiteral("rtl"), QStringLiteral("receiver-B"));
    const RadioSettingsScope family(QStringLiteral("rtl"), {});
    QString reason;
    check(Settings(first).load().status == Settings::ReadStatus::Missing, "absent device has no guessed calibration");
    check(family.setFeature(Settings::featureName(), 1, {{"ppm", 39}, {"dcSuppression", true}}), "seed unrelated family defaults");
    check(Settings(first).load().status == Settings::ReadStatus::Missing, "calibration never falls back to another device or family");
    check(Settings(first).saveAccepted({17, false}, reason), "accepted integer PPM saved for reported serial");
    check(Settings(first).load().values == Settings::Values{17, false}, "same serial restores exact values");
    check(Settings(second).load().status == Settings::ReadStatus::Missing, "different serial remains uncalibrated");
    check(!Settings(RadioSettingsScope::anonymousRadio(QStringLiteral("rtl"))).saveAccepted({-8, true}, reason), "anonymous/index identity remains session-only");
    check(family.featureExact(Settings::featureName()).value("ppm").toInt() == 39, "anonymous calibration cannot overwrite family defaults");

    QJsonObject stored = first.featureExact(Settings::featureName());
    stored.insert(QStringLiteral("futureField"), QJsonObject{{"keep", 123}});
    check(first.setFeature(Settings::featureName(), 1, stored), "seed unknown member");
    check(Settings(first).saveAccepted({-12, true}, reason), "update accepted controls");
    check(first.featureExact(Settings::featureName()).value("futureField").toObject().value("keep").toInt() == 123, "unknown members survive read-modify-write");
    check(Settings(first).load().values == Settings::Values{-12, true}, "DC and PPM roundtrip independently");
    const QJsonObject future{{"privateFutureState", 42}};
    check(first.setFeature(Settings::featureName(), 99, future), "seed future schema");
    check(Settings(first).load().status == Settings::ReadStatus::Refused, "future document is not reinterpreted as current controls");
    check(!Settings(first).saveAccepted({0, false}, reason) && first.featureExact(Settings::featureName()) == future, "accepted defaults cannot destroy future document");
    const QJsonObject malformed{{"ppm", 17.38}, {"dcSuppression", false}};
    check(first.setFeature(Settings::featureName(), 1, malformed), "seed unsupported fractional PPM");
    check(Settings(first).load().status == Settings::ReadStatus::Refused
        && !Settings(first).saveAccepted({0, false}, reason)
        && first.featureExact(Settings::featureName()) == malformed, "malformed document refused intact");
    for (const QJsonValue& invalid : {QJsonValue(-1001), QJsonValue(1001), QJsonValue("17"), QJsonValue(true), QJsonValue()}) {
        Settings::Values values{33, true};
        check(!Settings::decode({{"ppm", invalid}, {"dcSuppression", false}}, values)
            && values == Settings::Values{33, true}, "invalid PPM cannot publish partially parsed state");
    }
    Settings::Values values;
    check(!Settings::decode({{"ppm", 0}, {"dcSuppression", 1}}, values), "DC setting requires a real boolean");
    check(Settings::decode({}, values) && values == Settings::Values{}, "deliberately empty document disables both corrections");
    check(!Settings(RadioSettingsScope(QStringLiteral("flex"), QStringLiteral("receiver-A"))).saveAccepted({}, reason), "foreign family cannot write RTL calibration");
    return failures ? 1 : 0;
}
