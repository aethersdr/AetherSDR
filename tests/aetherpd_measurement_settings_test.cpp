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
    check(Settings(a).save({txlin::WindowKind::Hann, 2, 3, 8000}, reason), "save exact radio preferences");
    check(Settings(a).load().values == Settings::Values{txlin::WindowKind::Hann, 2, 3, 8000}, "roundtrip all analysis fields");
    check(Settings(b).load().values == Settings::Values{}, "separate radio has independent defaults");
    QJsonObject legacy = a.featureExact(Settings::featureName());
    legacy.remove(QStringLiteral("timeoutMs"));
    check(a.setFeature(Settings::featureName(), 1, legacy)
        && Settings(a).load().usable && Settings(a).load().values.timeoutMs == 10000,
        "legacy preferences without timeout retain ten-second ceiling");
    for (int timeout : {0, 2000, 3500, 10001, 14000}) {
        check(!Settings(a).save({txlin::WindowKind::Hann, 2, 3, timeout}, reason), "unsupported timeout cannot persist");
    }
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
    int savedSchema = 0;
    const QJsonObject good = a.featureExact(Settings::featureName(), &savedSchema);
    check(savedSchema == 2 && good.value("timeoutMs").toInt() == 10000,
          "explicit save migrates legacy schema so old readers cannot ignore timeout");
    QJsonObject missingTimeout = good; missingTimeout.remove(QStringLiteral("timeoutMs"));
    check(a.setFeature(Settings::featureName(), 2, missingTimeout) && !Settings(a).load().usable
        && !Settings(a).save({}, reason) && a.featureExact(Settings::featureName()) == missingTimeout,
        "current schema cannot silently default a missing timeout");
    for (const QJsonObject& bad : {QJsonObject{}, QJsonObject{{"window", "unknown"}, {"averages", 2}, {"maxOrder", 3}},
                                  QJsonObject{{"window", "hann"}, {"averages", 1.5}, {"maxOrder", 3}},
                                  QJsonObject{{"window", "hann"}, {"averages", 2}, {"maxOrder", "3"}}}) {
        check(a.setFeature(Settings::featureName(), 1, bad), "seed malformed document");
        check(!Settings(a).load().usable && !Settings(a).save({}, reason)
            && a.featureExact(Settings::featureName()) == bad, "malformed preferences preserved without guessed configuration");
    }
    for (const QJsonValue& timeout : {QJsonValue(), QJsonValue("8000"), QJsonValue(true), QJsonValue(8000.5), QJsonValue(14000)}) {
        QJsonObject bad = good; bad["timeoutMs"] = timeout;
        check(a.setFeature(Settings::featureName(), 1, bad), "seed invalid timeout");
        check(!Settings(a).load().usable && !Settings(a).save({}, reason)
            && a.featureExact(Settings::featureName()) == bad, "invalid timeout cannot become default or be overwritten");
    }
    check(a.setFeature(Settings::featureName(), 3, good), "seed future schema");
    check(!Settings(a).load().usable && !Settings(a).save({}, reason)
        && a.featureExact(Settings::featureName()) == good, "future schema cannot be overwritten");
    check(!good.contains("rfPower") && !good.contains("daxChannel") && !good.contains("dummyLoadConfirmed")
        && good.size() == 5, "document stores analysis only, never drive, stream ownership or authorization");
    return failures ? 1 : 0;
}
