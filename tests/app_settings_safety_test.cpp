#include "TestSettingsProfile.h"
#include "core/AppSettings.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QStandardPaths>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>

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

QString settingsPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
           + QStringLiteral("/AetherSDR/AetherSDR.settings");
}

QByteArray settingsDocument(int count, const QString& valuePrefix)
{
    QByteArray data;
    QBuffer buffer(&data);
    buffer.open(QIODevice::WriteOnly);
    QXmlStreamWriter xml(&buffer);
    xml.setAutoFormatting(true);
    xml.writeStartDocument();
    xml.writeStartElement(QStringLiteral("Settings"));
    for (int index = 0; index < count; ++index) {
        xml.writeTextElement(
            QStringLiteral("Key%1").arg(index, 3, 10, QLatin1Char('0')),
            QStringLiteral("%1-%2").arg(valuePrefix).arg(index));
    }
    xml.writeEndElement();
    xml.writeEndDocument();
    buffer.close();
    return data;
}

bool writeFile(const QString& path, const QByteArray& data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    return file.write(data) == data.size();
}

QByteArray readFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}

QMap<QString, QString> parseSettings(const QByteArray& data, bool* valid)
{
    QMap<QString, QString> values;
    QXmlStreamReader xml(data);
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement() && xml.name() != QStringLiteral("Settings")) {
            const QString key = xml.name().toString();
            values.insert(key, xml.readElementText());
        }
    }
    *valid = !xml.hasError();
    return values;
}

void testSaveBeforeLoad()
{
    const QString path = settingsPath();
    const QByteArray live = settingsDocument(50, QStringLiteral("live"));
    const QByteArray backup = settingsDocument(40, QStringLiteral("backup"));
    expect(writeFile(path, live), "production-like live fixture is written");
    expect(writeFile(path + QStringLiteral(".bak"), backup),
           "production-like backup fixture is written");

    AppSettings& settings = AppSettings::instance();
    settings.setValue(QStringLiteral("OnlyNewKey"), QStringLiteral("new-value"));
    settings.save();

    expect(readFile(path) == live,
           "save-before-load leaves the existing live file byte-for-byte unchanged");
    expect(readFile(path + QStringLiteral(".bak")) == backup,
           "save-before-load does not rotate or replace the backup");
    expect(!QFile::exists(path + QStringLiteral(".tmp")),
           "save-before-load creates no temporary settings file");
}

void testNormalLoadThenSave()
{
    const QString path = settingsPath();
    const QByteArray original = settingsDocument(50, QStringLiteral("original"));
    expect(writeFile(path, original), "normal-load fixture is written");

    AppSettings& settings = AppSettings::instance();
    settings.load();
    expect(settings.value(QStringLiteral("Key049")).toString()
               == QStringLiteral("original-49"),
           "normal load reads the complete existing profile");
    settings.setValue(QStringLiteral("AddedKey"), QStringLiteral("preserved"));
    settings.save();

    bool valid = false;
    const QMap<QString, QString> saved = parseSettings(readFile(path), &valid);
    expect(valid, "normal load-then-save produces valid XML");
    expect(saved.size() == 51 && saved.value(QStringLiteral("Key000"))
                                     == QStringLiteral("original-0")
               && saved.value(QStringLiteral("Key049"))
                      == QStringLiteral("original-49")
               && saved.value(QStringLiteral("AddedKey"))
                      == QStringLiteral("preserved"),
           "normal load-then-save preserves every old key and the new key");
    expect(readFile(path + QStringLiteral(".bak")) == original,
           "normal save retains the prior live profile as its backup");
}

void testFirstRunInitialization()
{
    const QString path = settingsPath();
    AppSettings& settings = AppSettings::instance();
    settings.load();

    bool valid = false;
    const QMap<QString, QString> saved = parseSettings(readFile(path), &valid);
    expect(valid && QFile::exists(path),
           "first-run load creates a valid settings document");
    expect(saved.size() == 13
               && saved.value(QStringLiteral("AutoConnect")) == QStringLiteral("True")
               && saved.value(QStringLiteral("AutoConnectToLastRadio"))
                      == QStringLiteral("True")
               && saved.contains(QStringLiteral("GUIClientID")),
           "first-run initialization writes only the intentional default profile");
}

void testSettingCountGuard()
{
    const QString path = settingsPath();
    const QByteArray original = settingsDocument(50, QStringLiteral("guarded"));
    expect(writeFile(path, original), "setting-count guard fixture is written");

    AppSettings& settings = AppSettings::instance();
    settings.load();
    for (int index = 0; index < 40; ++index) {
        settings.remove(
            QStringLiteral("Key%1").arg(index, 3, 10, QLatin1Char('0')));
    }
    settings.save();

    expect(readFile(path) == original,
           "the existing setting-count guard still rejects a truncated save");
    expect(!QFile::exists(path + QStringLiteral(".bak")),
           "a setting-count rejection does not rotate the live profile");
}

void testBackupRecovery()
{
    const QString path = settingsPath();
    const QByteArray corruptLive("<Settings><Broken>partial");
    const QByteArray goodBackup = settingsDocument(50, QStringLiteral("backup"));
    expect(writeFile(path, corruptLive), "corrupt live fixture is written");
    expect(writeFile(path + QStringLiteral(".bak"), goodBackup),
           "valid recovery fixture is written");

    AppSettings& settings = AppSettings::instance();
    settings.load();
    expect(settings.value(QStringLiteral("Key049")).toString()
               == QStringLiteral("backup-49"),
           "a corrupt live profile recovers all values from a valid backup");
    settings.setValue(QStringLiteral("RecoveredKey"), QStringLiteral("yes"));
    settings.save();

    bool valid = false;
    const QMap<QString, QString> saved = parseSettings(readFile(path), &valid);
    expect(valid && saved.size() == 51
               && saved.value(QStringLiteral("Key000")) == QStringLiteral("backup-0")
               && saved.value(QStringLiteral("RecoveredKey")) == QStringLiteral("yes"),
           "a recovered profile may be safely saved without losing backup keys");
    expect(readFile(path + QStringLiteral(".bak")) == goodBackup,
           "saving a recovered profile preserves the known-good backup");
}

void testMissingLivePromotesValidTemp()
{
    const QString path = settingsPath();
    const QByteArray pending = settingsDocument(50, QStringLiteral("pending"));
    const QByteArray backup = settingsDocument(40, QStringLiteral("backup"));
    expect(writeFile(path + QStringLiteral(".tmp"), pending),
           "interrupted-commit temporary fixture is written");
    expect(writeFile(path + QStringLiteral(".bak"), backup),
           "interrupted-commit backup fixture is written");

    AppSettings& settings = AppSettings::instance();
    settings.load();

    expect(settings.value(QStringLiteral("Key049")).toString()
               == QStringLiteral("pending-49"),
           "a valid pending commit wins when the live profile is missing");
    expect(readFile(path) == pending,
           "a valid pending commit is promoted to the live settings path");
    expect(!QFile::exists(path + QStringLiteral(".tmp")),
           "promoting a pending commit consumes the temporary file");
    expect(readFile(path + QStringLiteral(".bak")) == backup,
           "promoting a pending commit preserves the prior known-good backup");

    settings.setValue(QStringLiteral("RecoveredKey"), QStringLiteral("yes"));
    settings.save();

    bool valid = false;
    const QMap<QString, QString> saved = parseSettings(readFile(path), &valid);
    expect(valid && saved.size() == 51
               && saved.value(QStringLiteral("Key000"))
                      == QStringLiteral("pending-0")
               && saved.value(QStringLiteral("RecoveredKey"))
                      == QStringLiteral("yes"),
           "saving a promoted pending commit preserves its complete state");
    expect(readFile(path + QStringLiteral(".bak")) == backup,
           "the first save after promotion preserves the known-good recovery backup");
}

void testMissingLiveRecoversBackup()
{
    const QString path = settingsPath();
    const QByteArray corruptTemp("<Settings><Pending>partial");
    const QByteArray backup = settingsDocument(50, QStringLiteral("backup-only"));
    expect(writeFile(path + QStringLiteral(".tmp"), corruptTemp),
           "invalid newer temporary recovery fixture is written");
    expect(writeFile(path + QStringLiteral(".bak"), backup),
           "missing-live backup fixture is written");

    AppSettings& settings = AppSettings::instance();
    settings.load();
    expect(settings.value(QStringLiteral("Key049")).toString()
               == QStringLiteral("backup-only-49"),
           "a missing live profile falls back from an invalid temp to its valid backup");

    settings.setValue(QStringLiteral("RecoveredKey"), QStringLiteral("yes"));
    settings.save();

    bool valid = false;
    const QMap<QString, QString> saved = parseSettings(readFile(path), &valid);
    expect(valid && saved.size() == 51
               && saved.value(QStringLiteral("Key000"))
                      == QStringLiteral("backup-only-0")
               && saved.value(QStringLiteral("RecoveredKey"))
                      == QStringLiteral("yes"),
           "saving a missing-live recovery restores a complete live profile");
    expect(readFile(path + QStringLiteral(".bak")) == backup,
           "saving a missing-live recovery preserves the known-good backup");
}

void testMissingLiveCorruptRecoveryFailsClosed()
{
    const QString path = settingsPath();
    const QByteArray corruptTemp("<Settings><Pending>partial");
    const QByteArray corruptBackup("<Settings><Backup>partial");
    expect(writeFile(path + QStringLiteral(".tmp"), corruptTemp),
           "corrupt missing-live temporary fixture is written");
    expect(writeFile(path + QStringLiteral(".bak"), corruptBackup),
           "corrupt missing-live backup fixture is written");

    AppSettings& settings = AppSettings::instance();
    settings.load();
    settings.setValue(QStringLiteral("Replacement"), QStringLiteral("must-not-save"));
    settings.save();

    expect(!QFile::exists(path),
           "invalid missing-live recovery artifacts do not create a default profile");
    expect(readFile(path + QStringLiteral(".tmp")) == corruptTemp,
           "failed missing-live recovery leaves the temporary artifact unchanged");
    expect(readFile(path + QStringLiteral(".bak")) == corruptBackup,
           "failed missing-live recovery leaves the backup artifact unchanged");
}

void testFailedLoadCannotSave()
{
    const QString path = settingsPath();
    QByteArray corruptLive("<Settings>");
    for (int index = 0; index < 25; ++index) {
        corruptLive += "<Key" + QByteArray::number(index) + ">value</Key"
                       + QByteArray::number(index) + ">";
    }
    corruptLive += "<Broken>";
    const QByteArray corruptBackup("<Settings><AlsoBroken>");
    expect(writeFile(path, corruptLive), "partially populated corrupt live fixture is written");
    expect(writeFile(path + QStringLiteral(".bak"), corruptBackup),
           "corrupt backup fixture is written");

    AppSettings& settings = AppSettings::instance();
    settings.load();
    settings.setValue(QStringLiteral("Replacement"), QStringLiteral("must-not-save"));
    settings.save();

    expect(readFile(path) == corruptLive,
           "failed main and backup loads leave the live file unchanged");
    expect(readFile(path + QStringLiteral(".bak")) == corruptBackup,
           "failed main and backup loads leave the backup unchanged");
    expect(!QFile::exists(path + QStringLiteral(".tmp")),
           "failed loads cannot create a replacement temporary file");
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("aether-app-settings-safety-test"));
    if (!profile.isValid()) {
        std::fprintf(stderr, "[FAIL] could not create temporary settings profile\n");
        return 1;
    }

    QCoreApplication app(argc, argv);
    if (argc != 2) {
        std::fprintf(stderr, "usage: app_settings_safety_test <scenario>\n");
        return 2;
    }

    const QString scenario = QString::fromLocal8Bit(argv[1]);
    if (scenario == QStringLiteral("save-before-load")) {
        testSaveBeforeLoad();
    } else if (scenario == QStringLiteral("load-then-save")) {
        testNormalLoadThenSave();
    } else if (scenario == QStringLiteral("first-run")) {
        testFirstRunInitialization();
    } else if (scenario == QStringLiteral("count-guard")) {
        testSettingCountGuard();
    } else if (scenario == QStringLiteral("backup-recovery")) {
        testBackupRecovery();
    } else if (scenario == QStringLiteral("missing-live-temp")) {
        testMissingLivePromotesValidTemp();
    } else if (scenario == QStringLiteral("missing-live-backup")) {
        testMissingLiveRecoversBackup();
    } else if (scenario == QStringLiteral("missing-live-corrupt")) {
        testMissingLiveCorruptRecoveryFailsClosed();
    } else if (scenario == QStringLiteral("failed-load")) {
        testFailedLoadCannotSave();
    } else {
        std::fprintf(stderr, "unknown scenario: %s\n", argv[1]);
        return 2;
    }

    return g_failures == 0 ? 0 : 1;
}
