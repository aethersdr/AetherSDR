#include "TestSettingsProfile.h"
#include "aetherd/DiscoveryStartup.h"
#include "core/SettingsPaths.h"
#include "models/ConnectionWorkspace.h"
#include "models/SmartLinkAccountModel.h"
#include "core/backends/anan/AnanDiscovery.h"
#include "core/backends/hl2/Hl2Discovery.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QFileInfo>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

#include <cstdio>

using namespace AetherSDR;

namespace {

const QString kSerial = QStringLiteral("01:02:03:04:05:06");
const QString kNickname = QStringLiteral("Saved discovery nickname");
const QString kFingerprint(64, QLatin1Char('a'));

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

int readback(bool smartLink)
{
    // Construction runs the same startup policy as aetherd, but discovery is
    // never started: no UDP socket, USB enumeration or firmware peer is used.
    const std::unique_ptr<RadioDiscoverySource> source =
        aetherd::makeDiscoverySource({!smartLink, false}, smartLink);
    if (smartLink) {
        const QVector<PinnedCertInfo> pins = SmartLinkAccountModel::pinnedCertificates();
        if (!source || pins.size() != 1 || pins.first().host != QStringLiteral("192.0.2.1")
            || pins.first().fingerprintHex != kFingerprint) {
            std::fprintf(stderr, "SmartLink-only startup did not load saved certificate pins\n");
            return 1;
        }
        return 0;
    }
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
    const bool smartLinkChild = argc == 3 && QByteArray(argv[1]) == "--smartlink-readback";
    const bool failedSettingsChild = argc == 3 && QByteArray(argv[1]) == "--unavailable-smartlink-settings";
    const bool child = smartLinkChild || failedSettingsChild || (argc == 3 && QByteArray(argv[1]) == "--readback");
    if (child) {
        qputenv("AETHER_SETTINGS_DIR", QByteArray(argv[2]));
    }
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("aetherd"));
    QCoreApplication::setApplicationVersion(QStringLiteral(AETHERSDR_VERSION));
    if (failedSettingsChild) {
        return aetherd::makeDiscoverySource({false, false}, true) ? 1 : 0;
    }
    if (child) {
        return readback(smartLinkChild);
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

    settings.setValue("SmartLinkCertFingerprintCache", QJsonDocument(QJsonObject{
        {"192.0.2.1", QJsonObject{{"fp", kFingerprint}, {"pinnedAt", "2026-01-01T00:00:00Z"}}}})
        .toJson(QJsonDocument::Compact));
    settings.save();
    QJsonObject workspace = ConnectionWorkspace::load();
    const QString accountId = ConnectionWorkspace::accountId(QStringLiteral("fixture@example.test"));
    workspace["accounts"] = QJsonArray{QJsonObject{{"id", accountId}, {"name", "Fixture"}, {"email", "fixture@example.test"}}};
    workspace["selectedAccount"] = accountId;
    if (!ConnectionWorkspace::save(workspace)) { return 1; }

    for (const QString& mode : {QStringLiteral("--readback"), QStringLiteral("--smartlink-readback")}) {
        QProcess process;
        process.start(QCoreApplication::applicationFilePath(), {mode, SettingsPaths::configDir()});
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
    }
    QProcess listing;
    listing.start(QString::fromLocal8Bit(argv[1]), {QStringLiteral("--list-smartlink-accounts")});
    if (!listing.waitForFinished(15000) || listing.exitStatus() != QProcess::NormalExit
        || listing.exitCode() != 0) {
        std::fprintf(stderr, "saved SmartLink account listing failed\n");
        return 1;
    }
    const QJsonArray listed = QJsonDocument::fromJson(listing.readAllStandardOutput()).array();
    if (listed != QJsonArray{QJsonObject{{"id", accountId}, {"name", "Fixture"}}}) {
        std::fprintf(stderr, "saved SmartLink account listing missing metadata\n");
        return 1;
    }
    QFile unavailable(profile.path() + QStringLiteral("/not-a-directory"));
    if (!unavailable.open(QIODevice::WriteOnly)) { return 1; }
    unavailable.close();
    QProcess failedLoad;
    failedLoad.start(QCoreApplication::applicationFilePath(),
        {QStringLiteral("--unavailable-smartlink-settings"), unavailable.fileName()});
    if (!failedLoad.waitForFinished(15000) || failedLoad.exitStatus() != QProcess::NormalExit
        || failedLoad.exitCode() != 0) {
        std::fprintf(stderr, "unavailable SmartLink pin store did not refuse startup\n");
        return 1;
    }
    return 0;
}
