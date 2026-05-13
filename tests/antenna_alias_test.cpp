#include "core/AppSettings.h"
#include "models/AntennaAliasStore.h"
#include "models/SliceModel.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <iostream>

using namespace AetherSDR;

namespace {

bool expect(bool condition, const char* label)
{
    std::cout << (condition ? "[ OK ] " : "[FAIL] ") << label << '\n';
    return condition;
}

} // namespace

int main(int argc, char** argv)
{
    QTemporaryDir fakeHome(QDir::tempPath() + "/aether-antenna-alias-test-XXXXXX");
    if (!fakeHome.isValid()) {
        std::cerr << "[FAIL] create temporary home\n";
        return 1;
    }
    qputenv("HOME", fakeHome.path().toUtf8());
    qputenv("CFFIXED_USER_HOME", fakeHome.path().toUtf8());
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication app(argc, argv);

    const QString configRoot =
        QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
    QDir(configRoot + "/AetherSDR").removeRecursively();

    auto& settings = AppSettings::instance();
    settings.reset();

    const QString radioKey = QStringLiteral("serial-123");
    QMap<QString, QString> aliases;
    aliases.insert(QStringLiteral("ANT1"), QStringLiteral("YAGI"));
    aliases.insert(QStringLiteral("ANT2"), QStringLiteral("YAGI"));
    aliases.insert(QStringLiteral("RX_A"), QStringLiteral("BVRG"));
    AntennaAliasStore::save(radioKey, aliases);

    bool ok = true;
    QFile saved(settings.filePath());
    ok &= expect(saved.open(QIODevice::ReadOnly | QIODevice::Text),
                 "alias settings file is written");
    const QString xml = ok ? QString::fromUtf8(saved.readAll()) : QString();
    saved.close();
    ok &= expect(xml.contains(QStringLiteral("AntennaAliases_")),
                 "aliases persist under an XML-safe per-radio key");

    settings.reset();
    settings.load();
    const auto restored = AntennaAliasStore::load(radioKey);
    ok &= expect(restored.value(QStringLiteral("ANT1")) == QStringLiteral("YAGI"),
                 "ANT1 alias round-trips");
    ok &= expect(AntennaAliasStore::displayName(restored, QStringLiteral("RX_A"))
                     == QStringLiteral("BVRG"),
                 "alias display name replaces canonical token");
    ok &= expect(AntennaAliasStore::displayName(restored, QStringLiteral("RX_B"))
                     == QStringLiteral("RX_B"),
                 "empty alias falls back to canonical token");
    ok &= expect(AntennaAliasStore::displayName(restored, QStringLiteral("ANT1"), true)
                     == QStringLiteral("YAGI (ANT1)"),
                 "duplicate alias can be disambiguated with canonical token");

    SliceModel slice(3);
    QStringList commands;
    QObject::connect(&slice, &SliceModel::commandReady,
                     [&commands](const QString& cmd) { commands.append(cmd); });
    slice.applyStatus({{QStringLiteral("tx_ant_list"), QStringLiteral("ANT1,ANT2,XVTR")}});
    ok &= expect(slice.txAntennaList() == QStringList({QStringLiteral("ANT1"),
                                                       QStringLiteral("ANT2"),
                                                       QStringLiteral("XVTR")}),
                 "slice parses tx_ant_list");

    slice.setTxAntenna(QStringLiteral("XVTR"));
    ok &= expect(commands == QStringList({QStringLiteral("slice set 3 txant=XVTR")}),
                 "slice TX antenna command keeps canonical token");

    QDir(configRoot + "/AetherSDR").removeRecursively();
    return ok ? 0 : 1;
}
