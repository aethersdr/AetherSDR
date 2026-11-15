// The AetherTX profile library: the file store, the import reader and the
// name collision rule. The dialog around it drives QFileDialog and
// QInputDialog, which no headless run can click, so everything that decides
// what lands on disk lives here where it can be checked.

#include "TestSettingsProfile.h"
#include "core/AetherTxProfiles.h"
#include "core/ChannelStripPresets.h"
#include "core/AudioEngine.h"
#include "core/ClientComp.h"
#include "core/AppSettings.h"
#include "core/SettingsPaths.h"
#include "core/ClientGate.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

using namespace AetherSDR;

namespace {
// A minimal profile of the shape captureRxJson() produces.
QJsonObject sampleProfile(double threshold = -40.0)
{
    return QJsonObject{
        { "chain", QJsonArray{ "Gate", "Eq", "DeEss", "Comp", "Tube", "Enh", "Reverb" } },
        { "gate",  QJsonObject{ { "enabled", true }, { "thresholdDb", threshold } } },
        { "eq",    QJsonObject{ { "enabled", false } } },
    };
}
} // namespace

class AetherTxProfilesTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void storesAndListsByName();
    void namesSortCaseInsensitively();
    void exportWritesAStandaloneFileThatReadsBack();
    void readFileTakesTheFirstProfileOutOfALibrary();
    void readFileRejectsSomethingThatIsNotAProfile();
    void readFileRejectsBrokenJson();
    void uniqueNameStepsPastCollisions();
    void deleteRemovesIt();
    void theStoredCopyCarriesNoNameOfItsOwn();
    void aRefusedWriteKeepsTheLibraryAndReportsFailure();
    void roundTripsTheLiveTransmitChain();
    void exportsAndRestoresCompressorDriveAndPhase();
    void legacyCompressorProfilesKeepCurrentDriveAndPhase();
    void importedCompressorValuesAreClamped();
    void aReceiveExportIsRefusedByKind();
    void legacyPresetsMigrateOnFirstOpen();
    void migrationDoesNotUndoADeliberateDelete();

private:
    QTemporaryDir m_home;
};

void AetherTxProfilesTest::initTestCase()
{
    QVERIFY(m_home.isValid());
    const QString isolatedDir = qEnvironmentVariable("AETHER_SETTINGS_DIR");
    QVERIFY(!isolatedDir.isEmpty());
    QCOMPARE(SettingsPaths::configDir(), isolatedDir);
    QCOMPARE(SettingsPaths::databasePath(), isolatedDir + "/AetherSDR.db");
    QVERIFY(QStandardPaths::isTestModeEnabled());
}

void AetherTxProfilesTest::storesAndListsByName()
{
    AetherTxProfiles lib(nullptr);
    QVERIFY(lib.addProfile("Contest", sampleProfile()));
    // Check the production path, not just a second instance using the same
    // potentially incorrect location. No native settings path is writable
    // by the test: main() installs the sandbox before Qt starts.
    QFile stored(SettingsPaths::configDir() + "/AetherTxProfiles.json");
    QVERIFY(stored.open(QIODevice::ReadOnly));
    QVERIFY(QJsonDocument::fromJson(stored.readAll()).object()
                .value("profiles").toObject().contains("Contest"));
    QVERIFY(lib.hasProfile("Contest"));
    QVERIFY(lib.profileNames().contains("Contest"));

    // A second library over the same file sees it — it went to disk, not
    // just into memory.
    AetherTxProfiles reopened(nullptr);
    QVERIFY(reopened.hasProfile("Contest"));
}

void AetherTxProfilesTest::namesSortCaseInsensitively()
{
    AetherTxProfiles lib(nullptr);
    lib.addProfile("zulu", sampleProfile());
    lib.addProfile("Alpha", sampleProfile());
    const QStringList names = lib.profileNames();
    QVERIFY(names.indexOf("Alpha") < names.indexOf("zulu"));
}

void AetherTxProfilesTest::exportWritesAStandaloneFileThatReadsBack()
{
    AetherTxProfiles lib(nullptr);
    QVERIFY(lib.addProfile("DX Weak Signal", sampleProfile(-52.5)));

    const QString path = m_home.filePath("exported.json");
    QVERIFY(lib.exportToFile("DX Weak Signal", path));

    QFile f(path);
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    // The file names itself, so the importer has something to suggest.
    QCOMPARE(o.value("name").toString(), QStringLiteral("DX Weak Signal"));
    QCOMPARE(o.value("kind").toString(), QStringLiteral("AetherTX profile"));
    QCOMPARE(o.value("gate").toObject().value("thresholdDb").toDouble(), -52.5);

    QString suggested, error;
    const QJsonObject back = AetherTxProfiles::readFile(path, &suggested, &error);
    QVERIFY2(!back.isEmpty(), qPrintable(error));
    QCOMPARE(suggested, QStringLiteral("DX Weak Signal"));
    QCOMPARE(back.value("gate").toObject().value("thresholdDb").toDouble(), -52.5);
}

void AetherTxProfilesTest::readFileTakesTheFirstProfileOutOfALibrary()
{
    const QJsonObject library{
        { "version",  1 },
        { "profiles", QJsonObject{ { "Only One", sampleProfile(-33.0) } } },
    };
    const QString path = m_home.filePath("library.json");
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(QJsonDocument(library).toJson());
    f.close();

    QString suggested, error;
    const QJsonObject got = AetherTxProfiles::readFile(path, &suggested, &error);
    QVERIFY2(!got.isEmpty(), qPrintable(error));
    QCOMPARE(suggested, QStringLiteral("Only One"));
    QCOMPARE(got.value("gate").toObject().value("thresholdDb").toDouble(), -33.0);
}

void AetherTxProfilesTest::readFileRejectsSomethingThatIsNotAProfile()
{
    const QString path = m_home.filePath("wrong.json");
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("{\"radio\":\"FLEX-8600\",\"slices\":3}");
    f.close();

    QString suggested, error;
    QVERIFY(AetherTxProfiles::readFile(path, &suggested, &error).isEmpty());
    QVERIFY(!error.isEmpty());
}

void AetherTxProfilesTest::readFileRejectsBrokenJson()
{
    const QString path = m_home.filePath("broken.json");
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("{ this is not json");
    f.close();

    QString suggested, error;
    QVERIFY(AetherTxProfiles::readFile(path, &suggested, &error).isEmpty());
    QVERIFY(error.contains("JSON"));
}

void AetherTxProfilesTest::uniqueNameStepsPastCollisions()
{
    AetherTxProfiles lib(nullptr);
    QCOMPARE(lib.uniqueName("Fresh"), QStringLiteral("Fresh"));

    lib.addProfile("Rain Static", sampleProfile());
    QCOMPARE(lib.uniqueName("Rain Static"), QStringLiteral("Rain Static (2)"));
    lib.addProfile("Rain Static (2)", sampleProfile());
    QCOMPARE(lib.uniqueName("Rain Static"), QStringLiteral("Rain Static (3)"));

    // An import whose file offered nothing still gets a name to land on.
    QVERIFY(!lib.uniqueName(QString()).isEmpty());
}

void AetherTxProfilesTest::deleteRemovesIt()
{
    AetherTxProfiles lib(nullptr);
    lib.addProfile("Scratch", sampleProfile());
    QVERIFY(lib.deleteProfile("Scratch"));
    QVERIFY(!lib.hasProfile("Scratch"));
    QVERIFY(!lib.deleteProfile("Scratch"));   // second time is a no-op, not a crash
}

void AetherTxProfilesTest::theStoredCopyCarriesNoNameOfItsOwn()
{
    // The key is the name. A copy inside the object would be a second place
    // for it to be wrong after a rename, so addProfile strips it.
    AetherTxProfiles lib(nullptr);
    QJsonObject withName = sampleProfile();
    withName["name"] = QStringLiteral("Something Else");
    QVERIFY(lib.addProfile("Canonical", withName));

    const QString path = m_home.filePath("canonical.json");
    QVERIFY(lib.exportToFile("Canonical", path));
    QFile f(path);
    QVERIFY(f.open(QIODevice::ReadOnly));
    QCOMPARE(QJsonDocument::fromJson(f.readAll()).object().value("name").toString(),
             QStringLiteral("Canonical"));
}





// A save that cannot be written must leave both the file and the in-memory
// library exactly as they were, and must say so. The old code truncated the
// library before writing a byte and ignored the result, so an interrupted
// save destroyed every profile and the dialog still said "Saved".
void AetherTxProfilesTest::aRefusedWriteKeepsTheLibraryAndReportsFailure()
{
    AetherTxProfiles lib(nullptr);
    QVERIFY(lib.addProfile("Keep Me", sampleProfile(-11.0)));

    // Point the store at a regular file temporarily. This refuses writes
    // even under root and on Windows, without POSIX permissions or moving
    // the directory containing AppSettings' open SQLite database.
    const QByteArray savedDir = qgetenv("AETHER_SETTINGS_DIR");
    const QString blockedDir = m_home.filePath("not-a-directory");
    QFile obstruction(blockedDir);
    QVERIFY(obstruction.open(QIODevice::WriteOnly));
    obstruction.close();
    qputenv("AETHER_SETTINGS_DIR", blockedDir.toUtf8());
    const bool refused = !lib.addProfile("Should Not Land", sampleProfile());
    const bool deleteRefused = !lib.deleteProfile("Keep Me");
    const bool exportRefused = !lib.exportToFile("Keep Me", blockedDir + "/export.json");
    qputenv("AETHER_SETTINGS_DIR", savedDir); // restore before any assertion
    QVERIFY(refused);
    QVERIFY(deleteRefused);
    QVERIFY(exportRefused);
    // The refused name never entered the library...
    QVERIFY(!lib.hasProfile("Should Not Land"));
    // ...and the profile that was there is still there, on disk too.
    QVERIFY(lib.hasProfile("Keep Me"));
    AetherTxProfiles reopened(nullptr);
    QVERIFY(reopened.hasProfile("Keep Me"));
    QVERIFY(!reopened.hasProfile("Should Not Land"));
}

// The point of a profile: take the chain as it stands, change it, put the
// profile back, and get what you had. This is also the only cover on the
// captureTxJson/applyTxJson pair that ChannelStripPresets shares with this
// library — move either one and this notices.
void AetherTxProfilesTest::roundTripsTheLiveTransmitChain()
{
    AudioEngine engine;
    AetherTxProfiles lib(&engine, nullptr);

    auto* gate = engine.clientGateTx();
    auto* comp = engine.clientCompTx();
    QVERIFY(gate && comp);

    gate->setEnabled(true);
    gate->setThresholdDb(-29.5f);
    comp->setEnabled(false);
    comp->setRatio(6.5f);
    engine.setTxChainStages({ AudioEngine::TxChainStage::Comp,
                              AudioEngine::TxChainStage::Gate });

    QVERIFY(lib.saveFromCurrent("Round Trip"));

    gate->setEnabled(false);
    gate->setThresholdDb(-3.0f);
    comp->setEnabled(true);
    comp->setRatio(1.25f);
    engine.setTxChainStages({ AudioEngine::TxChainStage::Gate,
                              AudioEngine::TxChainStage::Comp });

    QVERIFY(lib.loadProfile("Round Trip"));

    QCOMPARE(gate->isEnabled(), true);
    QVERIFY(qAbs(gate->thresholdDb() - (-29.5f)) < 0.01f);
    QCOMPARE(comp->isEnabled(), false);
    QVERIFY(qAbs(comp->ratio() - 6.5f) < 0.01f);
    const auto chain = engine.txChainStages();
    QVERIFY(chain.size() >= 2);
    QCOMPARE(chain.at(0), AudioEngine::TxChainStage::Comp);
    QCOMPARE(chain.at(1), AudioEngine::TxChainStage::Gate);
}

void AetherTxProfilesTest::exportsAndRestoresCompressorDriveAndPhase()
{
    AudioEngine engine;
    AetherTxProfiles lib(&engine, nullptr);
    ChannelStripPresets presets(&engine);
    auto* comp = engine.clientCompTx();
    QVERIFY(comp);

    comp->setDriveDb(7.5f);
    comp->setPhaseRotatorStages(4);
    QVERIFY(lib.saveFromCurrent("Broadcast"));

    const QString path = m_home.filePath("compressor.json");
    QVERIFY(presets.exportCurrentToFile("Broadcast", path));
    QFile f(path);
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QJsonObject exported = QJsonDocument::fromJson(f.readAll())
                                     .object()
                                     .value("comp").toObject();
    QVERIFY(exported.contains("driveDb"));
    QVERIFY(exported.contains("phaseRotatorStages"));
    QCOMPARE(exported.value("driveDb").toDouble(), 7.5);
    QCOMPARE(exported.value("phaseRotatorStages").toInt(), 4);

    comp->setDriveDb(1.0f);
    comp->setPhaseRotatorStages(1);
    QVERIFY(lib.loadProfile("Broadcast"));
    QVERIFY(qAbs(comp->driveDb() - 7.5f) < 0.01f);
    QCOMPARE(comp->phaseRotatorStages(), 4);
}

void AetherTxProfilesTest::legacyCompressorProfilesKeepCurrentDriveAndPhase()
{
    AudioEngine engine;
    auto* comp = engine.clientCompTx();
    QVERIFY(comp);
    comp->setDriveDb(9.0f);
    comp->setPhaseRotatorStages(5);

    const QJsonObject legacy{
        { "comp", QJsonObject{
            { "enabled", true },
            { "thresholdDb", -24.0 },
            { "ratio", 4.0 },
        } },
    };
    ChannelStripPresets::applyTxJson(&engine, legacy);

    QVERIFY(qAbs(comp->driveDb() - 9.0f) < 0.01f);
    QCOMPARE(comp->phaseRotatorStages(), 5);
    QVERIFY(qAbs(comp->thresholdDb() - (-24.0f)) < 0.01f);
}

void AetherTxProfilesTest::importedCompressorValuesAreClamped()
{
    AudioEngine engine;
    auto* comp = engine.clientCompTx();
    QVERIFY(comp);

    ChannelStripPresets::applyTxJson(
        &engine, QJsonObject{
            { "comp", QJsonObject{
                { "driveDb", 1000.0 },
                { "phaseRotatorStages", 1000.0 },
            } },
        });
    QCOMPARE(comp->driveDb(), 18.0f);
    QCOMPARE(comp->phaseRotatorStages(), 6);

    ChannelStripPresets::applyTxJson(
        &engine, QJsonObject{
            { "comp", QJsonObject{
                { "driveDb", -1000.0 },
                { "phaseRotatorStages", -1000.0 },
            } },
        });
    QCOMPARE(comp->driveDb(), 0.0f);
    QCOMPARE(comp->phaseRotatorStages(), 0);
}

// The channel-strip library this store migrates from: both directions in one
// preset, TX at the top level and RX nested under "rx".
static bool writeLegacyLibrary(const QJsonObject& presets)
{
    QFile f(ChannelStripPresets::legacyLibraryPath());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    const QJsonObject root{ { "version", 1 }, { "presets", presets } };
    f.write(QJsonDocument(root).toJson());
    return true;
}

static void removeTxLibrary()
{
    QFile::remove(SettingsPaths::configDir() + "/AetherTxProfiles.json");
}

void AetherTxProfilesTest::aReceiveExportIsRefusedByKind()
{
    // The two windows' exports overlap almost completely — same stage keys,
    // overlapping chain names — so the stage-sniffing that lets a hand-written
    // file import would happily take a receive profile and write its values
    // into the transmit chain. "kind" is what tells them apart.
    const QString path = m_home.path() + "/rx-export.json";
    QJsonObject rx = sampleProfile();
    rx["kind"] = "AetherRX profile";
    rx["name"] = "Quiet Band";
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(QJsonDocument(rx).toJson());
    f.close();

    QString name, error;
    const QJsonObject got = AetherTxProfiles::readFile(path, &name, &error);
    QVERIFY(got.isEmpty());
    QVERIFY2(error.contains("AetherRX profile"), qPrintable(error));

    // A file with no "kind" at all still imports: that affordance is the
    // reason the check is conditional rather than required.
    QJsonObject bare = sampleProfile();
    const QString barePath = m_home.path() + "/bare.json";
    QFile b(barePath);
    QVERIFY(b.open(QIODevice::WriteOnly));
    b.write(QJsonDocument(bare).toJson());
    b.close();
    error.clear();
    QVERIFY(!AetherTxProfiles::readFile(barePath, &name, &error).isEmpty());
    QVERIFY(error.isEmpty());
}

void AetherTxProfilesTest::legacyPresetsMigrateOnFirstOpen()
{
    removeTxLibrary();
    QJsonObject preset = sampleProfile(-33.0);
    preset["rx"] = QJsonObject{ { "chain", QJsonArray{ "Gate" } },
                                { "gate", QJsonObject{ { "enabled", false } } } };
    QVERIFY(writeLegacyLibrary(QJsonObject{ { "Ragchew", preset } }));

    AetherTxProfiles lib(nullptr);
    QVERIFY2(lib.hasProfile("Ragchew"), qPrintable(lib.profileNames().join(',')));

    // The transmit half only: the receive block is the other window's, and
    // carrying it here is how a profile ends up changing both chains again.
    const QString out = m_home.path() + "/migrated.json";
    QVERIFY(lib.exportToFile("Ragchew", out));
    QFile f(out);
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QJsonObject got = QJsonDocument::fromJson(f.readAll()).object();
    QVERIFY(!got.contains("rx"));
    QCOMPARE(got.value("gate").toObject().value("thresholdDb").toDouble(), -33.0);

    // The legacy file is read, never rewritten: if this migration is wrong,
    // the original is still there to try again from.
    QFile legacy(ChannelStripPresets::legacyLibraryPath());
    QVERIFY(legacy.open(QIODevice::ReadOnly));
    QVERIFY(QJsonDocument::fromJson(legacy.readAll()).object()
                .value("presets").toObject().contains("Ragchew"));
}

void AetherTxProfilesTest::migrationDoesNotUndoADeliberateDelete()
{
    removeTxLibrary();
    QVERIFY(writeLegacyLibrary(QJsonObject{ { "Ragchew", sampleProfile() } }));

    AetherTxProfiles first(nullptr);
    QVERIFY(first.hasProfile("Ragchew"));
    QVERIFY(first.deleteProfile("Ragchew"));

    // Re-importing what the operator has just thrown away would be worse than
    // never importing at all, so the flag lives in the library file itself.
    AetherTxProfiles second(nullptr);
    QVERIFY(!second.hasProfile("Ragchew"));
}

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("aether-tx-profiles-test"));
    if (!profile.isValid()) {
        return 1;
    }
    QCoreApplication app(argc, argv);
    const QString expected = profile.path() + "/AetherSDR";
    if (SettingsPaths::configDir() != expected
        || SettingsPaths::databasePath() != expected + "/AetherSDR.db") {
        return 1;
    }
    AetherTxProfilesTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "aether_tx_profiles_test.moc"
