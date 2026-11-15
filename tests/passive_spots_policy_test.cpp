#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/SpotCommandPolicy.h"

#include <QCoreApplication>
#include <QFile>
#include <QString>
#include <cstdio>

using namespace AetherSDR;

namespace {

int g_failed = 0;

void report(const char* name, bool ok)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", name);
    if (!ok) ++g_failed;
}

void testSettingParsing()
{
    using namespace SpotCommandPolicy;

    report("PassiveSpotsMode True enables passive mode",
           passiveModeFromSetting(QStringLiteral("True")));
    report("PassiveSpotsMode False disables passive mode",
           !passiveModeFromSetting(QStringLiteral("False")));
    report("missing PassiveSpotsMode defaults inactive",
           !passiveModeFromSetting({}));
}

void testSendPolicyUsesAppSettings()
{
    auto& settings = AppSettings::instance();
    settings.reset();

    report("default policy sends spot add commands",
           SpotCommandPolicy::shouldSendSpotAddCommands(false));

    report("backend policy forces client-side spots",
           !SpotCommandPolicy::shouldSendSpotAddCommands(true));

    settings.setValue(SpotCommandPolicy::kPassiveSpotsModeKey, "True");
    report("passive mode suppresses spot add commands",
           !SpotCommandPolicy::shouldSendSpotAddCommands(false));

    report("passive mode remains effective for a client-side backend",
           !SpotCommandPolicy::shouldSendSpotAddCommands(true));

    settings.setValue(SpotCommandPolicy::kPassiveSpotsModeKey, "False");
    report("disabling passive mode resumes spot add commands",
           SpotCommandPolicy::shouldSendSpotAddCommands(false));

    report("disabling passive mode does not override backend policy",
           !SpotCommandPolicy::shouldSendSpotAddCommands(true));
}

void testShippingPublicationCallSites()
{
    QFile spotSubsystem(
        QStringLiteral(AETHER_SOURCE_DIR "/src/gui/MainWindow_Spots.cpp"));
    report("the policy test can inspect the shipping SpotHub wiring",
           spotSubsystem.open(QIODevice::ReadOnly));
    const QByteArray spotWiring = spotSubsystem.readAll();

    report("SpotHub sources gate publication on the backend capability",
           spotWiring.count(
               "m_radioModel.backendCapabilities().alwaysUseClientSideSpots") == 3);
    report("SpotHub sources retain the passive-local SpotModel route",
           spotWiring.contains(
               "addPassiveSpotToModel(spot, source, spotColor, lifetimeSec);"));
    report("WSJT-X retains the passive-local SpotModel route",
           spotWiring.contains(
               "addPassiveSpotToModel(colored, \"WSJT-X\", colored.color,"));

    QFile panWiring(
        QStringLiteral(AETHER_SOURCE_DIR "/src/gui/MainWindow_Wiring.cpp"));
    report("the policy test can inspect the shipping manual-spot wiring",
           panWiring.open(QIODevice::ReadOnly));
    const QByteArray manualWiring = panWiring.readAll();

    report("manual spots gate publication on the backend capability",
           manualWiring.contains(
               "m_radioModel.backendCapabilities().alwaysUseClientSideSpots"));
    report("manual spots retain the passive-local SpotModel route",
           manualWiring.contains(
               "m_radioModel.spotModel().applySpotStatus(spotId, kvs);"));
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile settingsProfile(QStringLiteral("aether-passive-spots-policy-test"));
    if (!settingsProfile.isValid()) {
        return 1;
    }
    QCoreApplication app(argc, argv);
    AppSettings::instance().load();

    testSettingParsing();
    testSendPolicyUsesAppSettings();
    testShippingPublicationCallSites();

    return g_failed == 0 ? 0 : 1;
}
