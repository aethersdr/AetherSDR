// The TX chain order between AudioEngine, the audio thread and the settings
// file: the audio thread gets the 4-bit layout, what is persisted is exactly
// what it runs, a stored list or a preset keeps its order and gains the
// canonical stages it lacks, and a repeated name in a hand-edited file or an
// imported preset is dropped rather than packed. AetherTX's gate and de-esser
// are retired: a saved order that names them loads without them, an operator
// who had the gate on gets RN2 instead, once, and both stages' keys go.
// The final limiter: one object under ClientFinalLimiterTx; a fresh install
// gets it on at -0.5 dB with 1.5 ms of look-ahead, the legacy flat keys are
// folded into the object once and removed with their values kept, and a TX
// profile carries the look-ahead. Isolated settings, no audio device, no radio.

#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/AudioEngine.h"
#include "core/ChannelStripPresets.h"
#include "core/ClientFinalLimiter.h"
#include "core/SettingsPaths.h"
#include "core/TxChainLayout.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QStringList>
#include <QtTest>

using namespace AetherSDR;
using Stage = AudioEngine::TxChainStage;

namespace {
constexpr auto kKey = "ClientCompTxChainStages";

QString stored()
{
    return AppSettings::instance().value(kKey, "").toString();
}
}  // namespace

class TxChainSettingsTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void theAudioThreadGetsTheNibbleLayout();
    void persistsExactlyWhatRuns();
    void loadKeepsTheSavedOrderAndAppendsTheRest();
    void loadDropsRepeatedNames();
    void aPresetWithRepeatsCannotOverflowTheChain();
    void freshInstallGetsTheCanonicalOrder();
    void theOldDefaultBecomesTheNewDefault();
    void aGateThatWasOnBecomesRn2Once();
    void aGateThatWasOffLeavesRn2Alone();
    void aFreshInstallGetsTheLookaheadLimiterOn();
    void aSavedFinalLimiterIsKept();
    void aTxProfileCarriesTheLookahead();
};

void TxChainSettingsTest::initTestCase()
{
    const QString isolatedDir = qEnvironmentVariable("AETHER_SETTINGS_DIR");
    QVERIFY(!isolatedDir.isEmpty());
    QCOMPARE(SettingsPaths::configDir(), isolatedDir);
    QVERIFY(QStandardPaths::isTestModeEnabled());
}

// The word the audio thread loads, not the engine's own decode of it: an
// engine that packed bytes would round-trip through its own unpack and still
// hand TxVoiceProcessor an order it reads as one stage.
void TxChainSettingsTest::theAudioThreadGetsTheNibbleLayout()
{
    AudioEngine engine;
    const QVector<Stage> order{Stage::Reverb, Stage::Enh, Stage::Tube, Stage::Comp,
                               Stage::Eq};
    engine.setTxChainStages(order);
    uint64_t expected = 0;
    QVERIFY(TxChainLayout::pack(order, expected));
    QCOMPARE(engine.txChainPackedForTest(), expected);
}

void TxChainSettingsTest::persistsExactlyWhatRuns()
{
    AudioEngine engine;
    // The retired gate and de-esser are dropped like repeats.
    engine.setTxChainStages({Stage::Comp, Stage::Gate, Stage::DeEss, Stage::Comp, Stage::Eq});
    const QVector<Stage> expected{Stage::Comp, Stage::Eq, Stage::Tube,
                                  Stage::Enh, Stage::Reverb};
    QCOMPARE(engine.txChainStages(), expected);
    QCOMPARE(stored(), QStringLiteral("Comp,Eq,Tube,Enh,Reverb"));
}

void TxChainSettingsTest::loadKeepsTheSavedOrderAndAppendsTheRest()
{
    // An order saved before the gate and de-esser were retired: they go, the
    // rest keep their order.
    AppSettings::instance().setValue(kKey, "Tube,Gate,Comp,DeEss");
    AudioEngine engine;
    engine.loadClientCompSettings();
    const QVector<Stage> expected{Stage::Tube, Stage::Comp, Stage::Eq,
                                  Stage::Enh, Stage::Reverb};
    QCOMPARE(engine.txChainStages(), expected);
}

void TxChainSettingsTest::loadDropsRepeatedNames()
{
    // Seventeen names: more than the packed order holds, all of them repeats
    // of stages that exist. They load as the five stages, in first-seen order.
    QStringList names;
    for (int i = 0; i < 17; ++i) {
        names << (i % 2 ? QStringLiteral("Eq") : QStringLiteral("Reverb"));
    }
    AppSettings::instance().setValue(kKey, names.join(','));
    AudioEngine engine;
    engine.loadClientCompSettings();
    const QVector<Stage> expected{Stage::Reverb, Stage::Eq, Stage::Tube,
                                  Stage::Comp, Stage::Enh};
    QCOMPARE(engine.txChainStages(), expected);
}

void TxChainSettingsTest::aPresetWithRepeatsCannotOverflowTheChain()
{
    QJsonArray chain;
    for (int i = 0; i < 20; ++i) {
        chain.append(i % 2 ? QStringLiteral("Gate") : QStringLiteral("Tube"));
    }
    AudioEngine engine;
    ChannelStripPresets::applyTxJson(&engine, QJsonObject{{"chain", chain}});
    // A preset saved before a stage existed must not remove it: the repeats
    // go, and the canonical stages the preset lacks follow its order.
    const QVector<Stage> expected{Stage::Tube, Stage::Eq,
                                  Stage::Comp, Stage::Enh, Stage::Reverb};
    QCOMPARE(engine.txChainStages(), expected);
    QCOMPARE(stored(), QStringLiteral("Tube,Eq,Comp,Enh,Reverb"));
}

void TxChainSettingsTest::freshInstallGetsTheCanonicalOrder()
{
    AppSettings::instance().remove(kKey);
    AppSettings::instance().remove("ClientCompTxChainOrder");
    AudioEngine engine;
    engine.loadClientCompSettings();
    // Tube leads: it is the mic preamp, and RN2 runs ahead of it.
    const QVector<Stage> expected{Stage::Tube, Stage::Eq, Stage::Comp,
                                  Stage::Enh, Stage::Reverb};
    QCOMPARE(engine.txChainStages(), expected);
}

// An upgrader whose saved order is the old default never chose it (it is
// saved on any profile load): it becomes today's, Tube first. The same five
// stages saved without retired names were chosen, and stay.
void TxChainSettingsTest::theOldDefaultBecomesTheNewDefault()
{
    const QVector<Stage> canonical{Stage::Tube, Stage::Eq, Stage::Comp,
                                   Stage::Enh, Stage::Reverb};
    AppSettings::instance().setValue(kKey, "Gate,Eq,DeEss,Comp,Tube,Enh,Reverb");
    {
        AudioEngine engine;
        engine.loadClientCompSettings();
        QCOMPARE(engine.txChainStages(), canonical);
    }
    AppSettings::instance().setValue(kKey, "Eq,Comp,Tube,Enh,Reverb");
    {
        AudioEngine engine;
        engine.loadClientCompSettings();
        const QVector<Stage> chosen{Stage::Eq, Stage::Comp, Stage::Tube,
                                    Stage::Enh, Stage::Reverb};
        QCOMPARE(engine.txChainStages(), chosen);
    }
    // The two-option setting from before the chain was reorderable: Tube
    // leads either way.
    AppSettings::instance().remove(kKey);
    AppSettings::instance().setValue("ClientCompTxChainOrder", "1");
    {
        AudioEngine engine;
        engine.loadClientCompSettings();
        QCOMPARE(engine.txChainStages().first(), Stage::Tube);
    }
    AppSettings::instance().remove("ClientCompTxChainOrder");
}

// Across the store boundary: the seeded keys are committed first, and every
// check after the migration reloads the database, so a migration that only
// changed the in-memory cache would fail here.
void TxChainSettingsTest::aGateThatWasOnBecomesRn2Once()
{
    auto& s = AppSettings::instance();
    s.setValue("AetherialTubePreampTx", "{}");
    s.setValue("ClientGateTxEnabled", "True");
    s.setValue("ClientGateTxThresholdDb", "-38");
    s.setValue("ClientDeEssTxEnabled", "True");
    s.setValue("ClientDeEssTxAmountDb", "-9");
    s.save();
    {
        AudioEngine engine;
        QVERIFY(engine.rn2TxEnabled());
    }
    s.load();  // what is on disk, not the cache
    QVERIFY(!s.contains("ClientGateTxEnabled"));
    QVERIFY(!s.contains("ClientGateTxThresholdDb"));
    // The de-esser has no replacement; its keys just go.
    QVERIFY(!s.contains("ClientDeEssTxEnabled"));
    QVERIFY(!s.contains("ClientDeEssTxAmountDb"));
    QVERIFY(s.value("AetherialTubePreampTx").toString().contains(QStringLiteral("\"rn2\":true")));
    // RN2 was persisted, and with the keys gone the migration cannot run
    // again, so switching RN2 off sticks across a reload too.
    {
        AudioEngine engine;
        QVERIFY(engine.rn2TxEnabled());
        engine.setRn2TxEnabled(false);
    }
    s.save();
    s.load();
    AudioEngine engine;
    QVERIFY(!engine.rn2TxEnabled());
}

void TxChainSettingsTest::aGateThatWasOffLeavesRn2Alone()
{
    auto& s = AppSettings::instance();
    s.setValue("AetherialTubePreampTx", "{}");
    s.setValue("ClientGateTxEnabled", "False");
    s.setValue("ClientGateTxRatio", "3");
    s.save();
    {
        AudioEngine engine;
        QVERIFY(!engine.rn2TxEnabled());
    }
    s.load();
    QVERIFY(!s.contains("ClientGateTxEnabled"));
    QVERIFY(!s.contains("ClientGateTxRatio"));
    QVERIFY(!s.value("AetherialTubePreampTx").toString().contains(QStringLiteral("\"rn2\":true")));
}

namespace {
constexpr auto kFinalLimiterKey = "ClientFinalLimiterTx";
constexpr const char* kLegacyFinalLimiterKeys[] = {
    "ClientFinalLimiterTxEnabled", "ClientFinalLimiterTxCeilingDb",
    "ClientFinalLimiterTxOutputTrimDb", "ClientFinalLimiterTxDcBlock",
};

void clearFinalLimiterSettings()
{
    auto& s = AppSettings::instance();
    s.remove(kFinalLimiterKey);
    for (const char* key : kLegacyFinalLimiterKeys) {
        s.remove(key);
    }
    s.save();
}

QJsonObject storedFinalLimiter()
{
    return QJsonDocument::fromJson(
        AppSettings::instance().value(kFinalLimiterKey, "{}").toString().toUtf8()).object();
}
}  // namespace

void TxChainSettingsTest::aFreshInstallGetsTheLookaheadLimiterOn()
{
    clearFinalLimiterSettings();
    {
        AudioEngine engine;
        auto* lim = engine.clientFinalLimiterTx();
        QVERIFY(lim);
        QVERIFY(lim->isEnabled());
        QCOMPARE(lim->ceilingDb(), -0.5f);
        QCOMPARE(lim->lookaheadMs(), 1.5f);
        lim->setLookaheadMs(4.5f);
        engine.saveClientFinalLimiterSettings();
    }
    auto& s = AppSettings::instance();
    s.save();
    s.load();  // what is on disk, not the cache
    // One object, and no flat key written beside it.
    const QJsonObject o = storedFinalLimiter();
    QCOMPARE(o.value("lookaheadMs").toDouble(), 4.5);
    QCOMPARE(o.value("enabled").toBool(false), true);
    QCOMPARE(o.value("ceilingDb").toDouble(), -0.5);
    for (const char* key : kLegacyFinalLimiterKeys) {
        QVERIFY2(!s.contains(key), key);
    }
    AudioEngine reread;
    QCOMPARE(reread.clientFinalLimiterTx()->lookaheadMs(), 4.5f);
    clearFinalLimiterSettings();
}

// An installation that saved its limiter in the legacy flat keys (off, as
// #3004 shipped it) keeps every value; the keys fold into the object once and
// go, and the look-ahead, which had no flat key, takes its default.
void TxChainSettingsTest::aSavedFinalLimiterIsKept()
{
    clearFinalLimiterSettings();
    auto& s = AppSettings::instance();
    s.setValue("ClientFinalLimiterTxEnabled", "False");
    s.setValue("ClientFinalLimiterTxCeilingDb", "-1");
    s.setValue("ClientFinalLimiterTxOutputTrimDb", "2.5");
    s.setValue("ClientFinalLimiterTxDcBlock", "False");
    s.save();
    {
        AudioEngine engine;
        auto* lim = engine.clientFinalLimiterTx();
        QVERIFY(lim);
        QVERIFY(!lim->isEnabled());
        QCOMPARE(lim->ceilingDb(), -1.0f);
        QCOMPARE(lim->outputTrimDb(), 2.5f);
        QVERIFY(!lim->dcBlockEnabled());
        QCOMPARE(lim->lookaheadMs(), 1.5f);
    }
    s.load();
    for (const char* key : kLegacyFinalLimiterKeys) {
        QVERIFY2(!s.contains(key), key);
    }
    const QJsonObject o = storedFinalLimiter();
    QCOMPARE(o.value("enabled").toBool(true), false);
    QCOMPARE(o.value("ceilingDb").toDouble(), -1.0);
    // The migration ran once: the object alone reproduces the saved state,
    // and a legacy key that comes back (an older settings import) neither
    // overrides it nor stays.
    s.setValue("ClientFinalLimiterTxEnabled", "True");
    s.save();
    {
        AudioEngine reread;
        QVERIFY(!reread.clientFinalLimiterTx()->isEnabled());
        QCOMPARE(reread.clientFinalLimiterTx()->outputTrimDb(), 2.5f);
    }
    s.load();
    QVERIFY(!s.contains("ClientFinalLimiterTxEnabled"));
    clearFinalLimiterSettings();
}

void TxChainSettingsTest::aTxProfileCarriesTheLookahead()
{
    AudioEngine engine;
    auto* lim = engine.clientFinalLimiterTx();
    QVERIFY(lim);
    lim->setLookaheadMs(4.0f);
    const QJsonObject captured = ChannelStripPresets::captureTxJson(&engine);
    QCOMPARE(captured.value("finalLimiter").toObject().value("lookaheadMs").toDouble(), 4.0);

    lim->setLookaheadMs(0.5f);
    ChannelStripPresets::applyTxJson(&engine, captured);
    QCOMPARE(lim->lookaheadMs(), 4.0f);

    // An older profile without the key leaves the look-ahead alone.
    QJsonObject old = captured;
    QJsonObject fl = old.value("finalLimiter").toObject();
    fl.remove("lookaheadMs");
    old["finalLimiter"] = fl;
    lim->setLookaheadMs(2.0f);
    ChannelStripPresets::applyTxJson(&engine, old);
    QCOMPARE(lim->lookaheadMs(), 2.0f);
    clearFinalLimiterSettings();
}

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("tx-chain-settings-test"));
    if (!profile.isValid()) {
        return 1;
    }
    QCoreApplication app(argc, argv);
    // Open the isolated store, so save() commits and load() re-reads it.
    AppSettings::instance().load();
    TxChainSettingsTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tx_chain_settings_test.moc"
