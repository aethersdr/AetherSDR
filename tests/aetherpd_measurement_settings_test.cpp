#include "TestSettingsProfile.h"
#include "core/AetherPDMeasurementSettings.h"
#include <QCoreApplication>
#include <QJsonObject>
#include <cstdio>

using namespace AetherSDR;
int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("aetherpd-measurement-settings"));
    if (!profile.isValid()) { return 1; }
    QCoreApplication app(argc, argv);
    AppSettings::instance().load();
    using Settings = AetherPDMeasurementSettings;
    const RadioSettingsScope a(QStringLiteral("flex"), QStringLiteral("radio-A"));
    const RadioSettingsScope b(QStringLiteral("flex"), QStringLiteral("radio-B"));
    const RadioSettingsScope family(QStringLiteral("flex"), {});
    int failures = 0;
    const auto check = [&](bool value, const char* label) {
        if (!value) { ++failures; std::fprintf(stderr, "FAIL: %s\n", label); }
    };
    QString reason;
    check(Settings(a).load().usable && Settings(a).load().values == Settings::Values{}, "missing settings preserve defaults");
    check(!Settings(family).save({}, reason), "unidentified radio cannot write family state");
    check(Settings(a).save({txlin::WindowKind::Hann, 2, 3}, reason), "save exact radio preferences");
    check(Settings(a).load().values == Settings::Values{txlin::WindowKind::Hann, 2, 3}, "roundtrip all analysis fields");
    check(Settings(b).load().values == Settings::Values{}, "separate radio has independent defaults");
    QJsonObject stored = a.featureExact(Settings::featureName());
    stored.insert(QStringLiteral("futureField"), QStringLiteral("retain"));
    check(a.setFeature(Settings::featureName(), 1, stored), "seed unknown same-schema member");
    check(Settings(a).save({txlin::WindowKind::FlatTop, 1, 5}, reason)
        && a.featureExact(Settings::featureName()).value("futureField") == "retain", "preserve unknown document members");
    for (int averages : {0, 5, 64}) {
        check(!Settings(a).save({txlin::WindowKind::Hann, averages, 3}, reason), "reject unavailable averaging count");
    }
    check(!Settings(a).save({txlin::WindowKind::Hann, 2, 4}, reason), "reject even IMD order");
    check(!Settings(a).save({static_cast<txlin::WindowKind>(99), 2, 3}, reason), "reject unknown window");
    const QJsonObject good = a.featureExact(Settings::featureName());
    for (const QJsonObject& bad : {QJsonObject{}, QJsonObject{{"window", "unknown"}, {"averages", 2}, {"maxOrder", 3}},
                                  QJsonObject{{"window", "hann"}, {"averages", 1.5}, {"maxOrder", 3}},
                                  QJsonObject{{"window", "hann"}, {"averages", 2}, {"maxOrder", "3"}}}) {
        check(a.setFeature(Settings::featureName(), 1, bad), "seed malformed document");
        check(!Settings(a).load().usable && !Settings(a).save({}, reason)
            && a.featureExact(Settings::featureName()) == bad, "malformed preferences preserved without guessed configuration");
    }
    check(a.setFeature(Settings::featureName(), 2, good), "seed future schema");
    check(!Settings(a).load().usable && !Settings(a).save({}, reason)
        && a.featureExact(Settings::featureName()) == good, "future schema cannot be overwritten");
    check(!good.contains("rfPower") && !good.contains("daxChannel") && !good.contains("dummyLoadConfirmed")
        && good.size() == 4, "document stores analysis only, never drive, stream ownership or authorization");
    return failures ? 1 : 0;
}
