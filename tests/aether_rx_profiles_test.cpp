// The AetherRX profile library: the file store, the import reader and the
// name collision rule. The dialog around it drives QFileDialog and
// QInputDialog, which no headless run can click, so everything that decides
// what lands on disk lives here where it can be checked.

#include "TestSettingsProfile.h"
#include "core/AetherRxProfiles.h"
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
        { "chain", QJsonArray{ "Eq", "Gate", "Comp", "Tube", "Pudu" } },
        { "gate",  QJsonObject{ { "enabled", true }, { "thresholdDb", threshold } } },
        { "eq",    QJsonObject{ { "enabled", false } } },
        { "rn2",   false },
    };
}
} // namespace

class AetherRxProfilesTest : public QObject {
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
    void roundTripsTheLiveReceiveChain();
    void aRetiredStageNameDoesNotDiscardTheStoredOrder();
    void anUnknownStageNameStillResetsToTheDefault();
    void retiredSettingsKeysAreDroppedOnLoad();
    void aRefusedWriteKeepsTheLibraryAndReportsFailure();
    void legacyPresetsMigrateTheirReceiveHalfOnly();

private:
    QTemporaryDir m_home;
};

void AetherRxProfilesTest::initTestCase()
{
    QVERIFY(m_home.isValid());
    const QString isolatedDir = qEnvironmentVariable("AETHER_SETTINGS_DIR");
    QVERIFY(!isolatedDir.isEmpty());
    QCOMPARE(SettingsPaths::configDir(), isolatedDir);
    QCOMPARE(SettingsPaths::databasePath(), isolatedDir + "/AetherSDR.db");
    QVERIFY(QStandardPaths::isTestModeEnabled());
}

void AetherRxProfilesTest::storesAndListsByName()
{
    AetherRxProfiles lib(nullptr);
    QVERIFY(lib.addProfile("Contest", sampleProfile()));
    // Check the production path, not just a second instance using the same
    // potentially incorrect location. No native settings path is writable
    // by the test: main() installs the sandbox before Qt starts.
    QFile stored(SettingsPaths::configDir() + "/AetherRxProfiles.json");
    QVERIFY(stored.open(QIODevice::ReadOnly));
    QVERIFY(QJsonDocument::fromJson(stored.readAll()).object()
                .value("profiles").toObject().contains("Contest"));
    QVERIFY(lib.hasProfile("Contest"));
    QVERIFY(lib.profileNames().contains("Contest"));

    // A second library over the same file sees it — it went to disk, not
    // just into memory.
    AetherRxProfiles reopened(nullptr);
    QVERIFY(reopened.hasProfile("Contest"));
}

void AetherRxProfilesTest::namesSortCaseInsensitively()
{
    AetherRxProfiles lib(nullptr);
    lib.addProfile("zulu", sampleProfile());
    lib.addProfile("Alpha", sampleProfile());
    const QStringList names = lib.profileNames();
    QVERIFY(names.indexOf("Alpha") < names.indexOf("zulu"));
}

void AetherRxProfilesTest::exportWritesAStandaloneFileThatReadsBack()
{
    AetherRxProfiles lib(nullptr);
    QVERIFY(lib.addProfile("DX Weak Signal", sampleProfile(-52.5)));

    const QString path = m_home.filePath("exported.json");
    QVERIFY(lib.exportToFile("DX Weak Signal", path));

    QFile f(path);
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    // The file names itself, so the importer has something to suggest.
    QCOMPARE(o.value("name").toString(), QStringLiteral("DX Weak Signal"));
    QCOMPARE(o.value("kind").toString(), QStringLiteral("AetherRX profile"));
    QCOMPARE(o.value("gate").toObject().value("thresholdDb").toDouble(), -52.5);

    QString suggested, error;
    const QJsonObject back = AetherRxProfiles::readFile(path, &suggested, &error);
    QVERIFY2(!back.isEmpty(), qPrintable(error));
    QCOMPARE(suggested, QStringLiteral("DX Weak Signal"));
    QCOMPARE(back.value("gate").toObject().value("thresholdDb").toDouble(), -52.5);
}

void AetherRxProfilesTest::readFileTakesTheFirstProfileOutOfALibrary()
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
    const QJsonObject got = AetherRxProfiles::readFile(path, &suggested, &error);
    QVERIFY2(!got.isEmpty(), qPrintable(error));
    QCOMPARE(suggested, QStringLiteral("Only One"));
    QCOMPARE(got.value("gate").toObject().value("thresholdDb").toDouble(), -33.0);
}

void AetherRxProfilesTest::readFileRejectsSomethingThatIsNotAProfile()
{
    const QString path = m_home.filePath("wrong.json");
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("{\"radio\":\"FLEX-8600\",\"slices\":3}");
    f.close();

    QString suggested, error;
    QVERIFY(AetherRxProfiles::readFile(path, &suggested, &error).isEmpty());
    QVERIFY(!error.isEmpty());
}

void AetherRxProfilesTest::readFileRejectsBrokenJson()
{
    const QString path = m_home.filePath("broken.json");
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("{ this is not json");
    f.close();

    QString suggested, error;
    QVERIFY(AetherRxProfiles::readFile(path, &suggested, &error).isEmpty());
    QVERIFY(error.contains("JSON"));
}

void AetherRxProfilesTest::uniqueNameStepsPastCollisions()
{
    AetherRxProfiles lib(nullptr);
    QCOMPARE(lib.uniqueName("Fresh"), QStringLiteral("Fresh"));

    lib.addProfile("Rain Static", sampleProfile());
    QCOMPARE(lib.uniqueName("Rain Static"), QStringLiteral("Rain Static (2)"));
    lib.addProfile("Rain Static (2)", sampleProfile());
    QCOMPARE(lib.uniqueName("Rain Static"), QStringLiteral("Rain Static (3)"));

    // An import whose file offered nothing still gets a name to land on.
    QVERIFY(!lib.uniqueName(QString()).isEmpty());
}

void AetherRxProfilesTest::deleteRemovesIt()
{
    AetherRxProfiles lib(nullptr);
    lib.addProfile("Scratch", sampleProfile());
    QVERIFY(lib.deleteProfile("Scratch"));
    QVERIFY(!lib.hasProfile("Scratch"));
    QVERIFY(!lib.deleteProfile("Scratch"));   // second time is a no-op, not a crash
}

void AetherRxProfilesTest::theStoredCopyCarriesNoNameOfItsOwn()
{
    // The key is the name. A copy inside the object would be a second place
    // for it to be wrong after a rename, so addProfile strips it.
    AetherRxProfiles lib(nullptr);
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

// The whole point of a profile: take the chain as it stands, change it, put
// the profile back, and get what you had. This is also the only cover on the
// captureRxJson/applyRxJson pair that ChannelStripPresets now shares with
// this library — move either one and this notices.
void AetherRxProfilesTest::roundTripsTheLiveReceiveChain()
{
    AudioEngine engine;
    AetherRxProfiles lib(&engine, nullptr);

    auto* gate = engine.clientGateRx();
    auto* comp = engine.clientCompRx();
    QVERIFY(gate && comp);

    gate->setEnabled(true);
    gate->setThresholdDb(-37.5f);
    comp->setEnabled(false);
    comp->setRatio(7.25f);
    engine.setRxChainStages({ AudioEngine::RxChainStage::Comp,
                              AudioEngine::RxChainStage::Gate });

    QVERIFY(lib.saveFromCurrent("Round Trip"));

    // Move everything the profile claims to hold.
    gate->setEnabled(false);
    gate->setThresholdDb(-5.0f);
    comp->setEnabled(true);
    comp->setRatio(1.5f);
    engine.setRxChainStages({ AudioEngine::RxChainStage::Gate,
                              AudioEngine::RxChainStage::Comp });

    QVERIFY(lib.loadProfile("Round Trip"));

    QCOMPARE(gate->isEnabled(), true);
    QVERIFY(qAbs(gate->thresholdDb() - (-37.5f)) < 0.01f);
    QCOMPARE(comp->isEnabled(), false);
    QVERIFY(qAbs(comp->ratio() - 7.25f) < 0.01f);
    const auto chain = engine.rxChainStages();
    QVERIFY(chain.size() >= 2);
    QCOMPARE(chain.at(0), AudioEngine::RxChainStage::Comp);
    QCOMPARE(chain.at(1), AudioEngine::RxChainStage::Gate);
}

// A settings file written before the RX de-esser was retired still names it.
// That is not a foreign file, and the operator's ordering has to survive it —
// the load path used to treat any unrecognised name as evidence of a strange
// settings file and reset the whole chain to the default, which would have
// silently undone every reorder in the same release that made the chain
// drag-reorderable.
void AetherRxProfilesTest::aRetiredStageNameDoesNotDiscardTheStoredOrder()
{
    AppSettings::instance().setValue(
        QStringLiteral("ClientRxChainStages"),
        QStringLiteral("Pudu,DeEss,Tube,Comp,Eq,Gate"));

    AudioEngine engine;
    engine.loadClientRxChainOrder();

    const QVector<AudioEngine::RxChainStage> got = engine.rxChainStages();
    const QVector<AudioEngine::RxChainStage> want{
        AudioEngine::RxChainStage::Pudu, AudioEngine::RxChainStage::Tube,
        AudioEngine::RxChainStage::Comp, AudioEngine::RxChainStage::Eq,
        AudioEngine::RxChainStage::Gate,
    };
    QCOMPARE(got, want);

    // And the name is gone from the file, rather than waiting to be dropped
    // again on every launch.
    QVERIFY(!AppSettings::instance()
                 .value(QStringLiteral("ClientRxChainStages"))
                 .toString()
                 .contains(QStringLiteral("DeEss")));
}

// A name from neither this build nor its retired list is still evidence of a
// settings file worth distrusting, and still resets to the canonical order.
void AetherRxProfilesTest::anUnknownStageNameStillResetsToTheDefault()
{
    AppSettings::instance().setValue(
        QStringLiteral("ClientRxChainStages"),
        QStringLiteral("Pudu,Flanger,Gate"));

    AudioEngine engine;
    engine.loadClientRxChainOrder();

    // The canonical order, not the stored one: Pudu and Gate are discarded
    // along with the name that could not be placed.
    const QVector<AudioEngine::RxChainStage> got = engine.rxChainStages();
    const QVector<AudioEngine::RxChainStage> canonical{
        AudioEngine::RxChainStage::Gate, AudioEngine::RxChainStage::Eq,
        AudioEngine::RxChainStage::Comp, AudioEngine::RxChainStage::Tube,
        AudioEngine::RxChainStage::Pudu,
    };
    QCOMPARE(got, canonical);
}

// The de-esser's keys and the two fixed attack values are read by nothing
// now; they should not keep riding along in every settings file.
void AetherRxProfilesTest::retiredSettingsKeysAreDroppedOnLoad()
{
    auto& settings = AppSettings::instance();
    settings.setValue(QStringLiteral("ClientDeEssRxEnabled"), QStringLiteral("True"));
    settings.setValue(QStringLiteral("ClientDeEssRxThresholdDb"), QStringLiteral("-24.0"));
    settings.setValue(QStringLiteral("ClientGateTxAttackMs"), QStringLiteral("0.5"));
    settings.setValue(QStringLiteral("ClientTubeRxAttackMs"), QStringLiteral("5.0"));
    // A key that is still live must survive the sweep.
    settings.setValue(QStringLiteral("ClientGateTxHoldMs"), QStringLiteral("20.0"));

    AudioEngine engine;   // its constructor runs the load sequence

    for (const char* gone : {"ClientDeEssRxEnabled", "ClientDeEssRxThresholdDb",
                             "ClientGateTxAttackMs", "ClientTubeRxAttackMs"}) {
        QVERIFY2(settings.value(QLatin1String(gone), QString()).toString().isEmpty(),
                 gone);
    }
    QCOMPARE(settings.value(QStringLiteral("ClientGateTxHoldMs")).toString(),
             QStringLiteral("20.0"));
}

// A save that cannot be written must leave both the file and the in-memory
// library exactly as they were, and must say so. The old code truncated the
// library before writing a byte and ignored the result, so an interrupted
// save destroyed every profile and the dialog still said "Saved".
void AetherRxProfilesTest::aRefusedWriteKeepsTheLibraryAndReportsFailure()
{
    AetherRxProfiles lib(nullptr);
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
    AetherRxProfiles reopened(nullptr);
    QVERIFY(reopened.hasProfile("Keep Me"));
    QVERIFY(!reopened.hasProfile("Should Not Land"));
}

void AetherRxProfilesTest::legacyPresetsMigrateTheirReceiveHalfOnly()
{
    // A channel-strip preset carried both directions. The receive half is the
    // nested "rx" block, and it is the only part that belongs here — taking
    // the top level too would drag transmit values into the receive chain.
    QFile::remove(SettingsPaths::configDir() + "/AetherRxProfiles.json");

    QJsonObject preset{
        { "createdBy", "AetherSDR" },
        { "chain", QJsonArray{ "Gate", "Eq" } },                     // TX half
        { "gate",  QJsonObject{ { "thresholdDb", -11.0 } } },        // TX half
        { "rx",    sampleProfile(-55.0) },
    };
    QJsonObject noRx = preset;
    noRx.remove("rx");

    QFile f(ChannelStripPresets::legacyLibraryPath());
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(QJsonDocument(QJsonObject{
        { "version", 1 },
        { "presets", QJsonObject{ { "Ragchew", preset },
                                  // Saved before the RX chain existed: no
                                  // receive half, so nothing to migrate.
                                  { "Ancient", noRx } } } }).toJson());
    f.close();

    AetherRxProfiles lib(nullptr);
    QVERIFY2(lib.hasProfile("Ragchew"), qPrintable(lib.profileNames().join(',')));
    QVERIFY(!lib.hasProfile("Ancient"));

    const QString out = m_home.path() + "/migrated-rx.json";
    QVERIFY(lib.exportToFile("Ragchew", out));
    QFile g(out);
    QVERIFY(g.open(QIODevice::ReadOnly));
    const QJsonObject got = QJsonDocument::fromJson(g.readAll()).object();
    QCOMPARE(got.value("gate").toObject().value("thresholdDb").toDouble(), -55.0);
}

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("aether-rx-profiles-test"));
    if (!profile.isValid()) {
        return 1;
    }
    QCoreApplication app(argc, argv);
    const QString expected = profile.path() + "/AetherSDR";
    if (SettingsPaths::configDir() != expected
        || SettingsPaths::databasePath() != expected + "/AetherSDR.db") {
        return 1;
    }
    AetherRxProfilesTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "aether_rx_profiles_test.moc"
