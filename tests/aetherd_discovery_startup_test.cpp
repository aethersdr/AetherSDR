#include "TestSettingsProfile.h"
#include "aetherd/DiscoveryStartup.h"
#include "core/SettingsPaths.h"
#include "core/backends/anan/AnanDiscovery.h"
#include "core/backends/hl2/Hl2Discovery.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

#include <cstdio>

using namespace AetherSDR;

namespace {

const QString kSerial = QStringLiteral("01:02:03:04:05:06");
const QString kNickname = QStringLiteral("Saved discovery nickname");

bool rejectsBeforeSettings(const QString& executable, const QStringList& options,
                           const QString& settingsPath, const QByteArray& diagnostic)
{
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("AETHER_SETTINGS_DIR"), settingsPath);
    // The invalid endpoint also prevents a bind if argument validation regresses.
    // No test case can reach discovery, even when the validator is removed.
    QStringList arguments{QStringLiteral("--socket"), QStringLiteral("invalid/name")};
    arguments.append(options);
    QProcess rejected;
    rejected.setProcessEnvironment(environment);
    rejected.start(executable, arguments);
    if (!rejected.waitForFinished(15000)) {
        rejected.kill();
        rejected.waitForFinished(1000);
        std::fprintf(stderr, "rejected daemon startup failed or timed out\n");
        return false;
    }
    const QByteArray error = rejected.readAllStandardError();
    if (rejected.exitStatus() != QProcess::NormalExit || rejected.exitCode() != 1
        || !error.contains(diagnostic) || QFileInfo::exists(settingsPath)) {
        std::fprintf(stderr, "startup did not refuse before settings: %s\n", error.constData());
        return false;
    }
    return true;
}

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

    if (argc != 2) {
        std::fprintf(stderr, "expected daemon executable path\n");
        return 1;
    }
    for (const bool control : {false, true}) {
        // Invalid logical names fail before endpoint resolution: no socket is
        // bound. Simulator-only mode also keeps a regressed startup free of
        // native discovery or settings-load side effects. RadioModel's settings
        // singleton construction would still create this private directory.
        const QString rejectedSettings = profile.path()
            + (control ? QStringLiteral("/rejected-control") : QStringLiteral("/rejected-observe"));
        QStringList options{QStringLiteral("--discover-sim")};
        if (control) {
            options.append(QStringLiteral("--allow-local-control"));
        }
        if (!rejectsBeforeSettings(QString::fromLocal8Bit(argv[1]), options, rejectedSettings,
                QByteArrayLiteral("cannot listen on local socket"))) {
            return 1;
        }
    }

    const QStringList invalidAddresses{
        QStringLiteral("not-an-address"), QStringLiteral("192.168.1.300"),
        QStringLiteral("192.168.1.5:1024"), QStringLiteral("::1"),
        QStringLiteral("0.0.0.0"), QStringLiteral("255.255.255.255"),
        QStringLiteral("224.0.0.1")};
    for (qsizetype index = 0; index < invalidAddresses.size(); ++index) {
        const QStringList options{QStringLiteral("--discover-local"),
                                  QStringLiteral("--hl2-address"), QStringLiteral("192.168.50.7"),
                                  QStringLiteral("--hl2-address"), invalidAddresses[index]};
        const QByteArray diagnostic = "--hl2-address '" + invalidAddresses[index].toUtf8()
            + "' must be a unicast IPv4 literal";
        if (!rejectsBeforeSettings(QString::fromLocal8Bit(argv[1]), options,
                profile.path() + QStringLiteral("/rejected-address-%1").arg(index), diagnostic)) {
            return 1;
        }
    }
    const QStringList repeatedAddresses{QStringLiteral("--hl2-address"), QStringLiteral("192.168.50.7"),
                                        QStringLiteral("--hl2-address"), QStringLiteral("192.168.50.7"),
                                        QStringLiteral("--hl2-address"), QStringLiteral("10.1.2.3")};
    QStringList localOptions{QStringLiteral("--discover-local")};
    localOptions.append(repeatedAddresses);
    if (!rejectsBeforeSettings(QString::fromLocal8Bit(argv[1]), localOptions,
            profile.path() + QStringLiteral("/accepted-addresses"),
            QByteArrayLiteral("cannot listen on local socket"))) {
        return 1;
    }
    if (!rejectsBeforeSettings(QString::fromLocal8Bit(argv[1]), repeatedAddresses,
            profile.path() + QStringLiteral("/inactive-addresses"),
            QByteArrayLiteral("--hl2-address has no effect without --discover-local"))) {
        return 1;
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
