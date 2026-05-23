#include "core/AppSettings.h"
#include "core/SpotCommandPolicy.h"

#include <QCoreApplication>
#include <QString>
#include <cstdio>

using namespace MasterSDR;

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
           SpotCommandPolicy::shouldSendSpotAddCommands());

    settings.setValue(SpotCommandPolicy::kPassiveSpotsModeKey, "True");
    report("passive mode suppresses spot add commands",
           !SpotCommandPolicy::shouldSendSpotAddCommands());

    settings.setValue(SpotCommandPolicy::kPassiveSpotsModeKey, "False");
    report("disabling passive mode resumes spot add commands",
           SpotCommandPolicy::shouldSendSpotAddCommands());
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    testSettingParsing();
    testSendPolicyUsesAppSettings();

    return g_failed == 0 ? 0 : 1;
}
