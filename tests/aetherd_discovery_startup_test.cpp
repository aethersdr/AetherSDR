#include "TestSettingsProfile.h"
#include "aetherd/DiscoveryStartup.h"
#include "core/SettingsPaths.h"
#include "core/backends/anan/AnanDiscovery.h"
#include "core/backends/hl2/Hl2Discovery.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QProcess>

#include <cstdio>

using namespace AetherSDR;

namespace {

const QString kSerial = QStringLiteral("01:02:03:04:05:06");
const QString kNickname = QStringLiteral("Saved discovery nickname");

int readback()
{
    // Construction runs the same startup policy as aetherd, but discovery is
    // never started: no UDP socket, USB enumeration or firmware peer is used.
    const std::unique_ptr<RadioDiscoverySource> source =
        aetherd::makeDiscoverySource({true, false});
    if (!source || !source->enabledSources().contains(QStringLiteral("hl2"))
        || !source->enabledSources().contains(QStringLiteral("anan"))) {
        std::fprintf(stderr, "local discovery sources missing\n");
        return 1;
    }
    if (hl2::Hl2Discovery::effectiveNickname(QStringLiteral("hl2"), kSerial,
            QStringLiteral("Hermes-Lite 2")) != kNickname
        || anan::AnanDiscovery::effectiveNickname(QStringLiteral("anan"), kSerial,
            QStringLiteral("ANAN-G2")) != kNickname) {
        std::fprintf(stderr, "daemon startup did not load persisted native nicknames\n");
        return 1;
    }
    return 0;
}

} // namespace

int main(int argc, char* argv[])
{
    TestSettingsProfile profile(QStringLiteral("aetherd-discovery-startup"));
    if (!profile.isValid()) {
        return 1;
    }
    // A readback process gets its own legacy-settings isolation but reads the
    // parent's explicitly supplied SQLite store, never the operator's profile.
    const bool child = argc == 3 && QByteArray(argv[1]) == "--readback";
    if (child) {
        qputenv("AETHER_SETTINGS_DIR", QByteArray(argv[2]));
    }
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("aetherd"));
    QCoreApplication::setApplicationVersion(QStringLiteral(AETHERSDR_VERSION));
    if (child) {
        return readback();
    }

    for (const bool simulator : {false, true}) {
        const std::unique_ptr<RadioDiscoverySource> source =
            aetherd::makeDiscoverySource({false, simulator});
        source->start(); // Passive or demo metadata only; no physical discovery.
        source->stop();
        if (QFileInfo::exists(SettingsPaths::databasePath())) {
            std::fprintf(stderr, "passive/simulator startup opened the settings store\n");
            return 1;
        }
    }

    AppSettings& settings = AppSettings::instance();
    settings.load();
    for (const QString& family : {QStringLiteral("hl2"), QStringLiteral("anan")}) {
        if (!settings.setRadioFeature(family, kSerial, QStringLiteral("Identity"), 1,
                {{QStringLiteral("nickname"), kNickname}})) {
            std::fprintf(stderr, "could not seed isolated nickname document\n");
            return 1;
        }
    }

    QProcess process;
    process.start(QCoreApplication::applicationFilePath(),
        {QStringLiteral("--readback"), SettingsPaths::configDir()});
    if (!process.waitForFinished(15000)) {
        process.kill();
        process.waitForFinished(1000);
        std::fprintf(stderr, "nickname readback process failed or timed out\n");
        return 1;
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        std::fputs(process.readAllStandardError().constData(), stderr);
        return 1;
    }
    return 0;
}
