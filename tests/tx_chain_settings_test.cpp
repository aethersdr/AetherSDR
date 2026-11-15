// The TX chain order between AudioEngine, the audio thread and the settings
// file: the audio thread gets the 4-bit layout, what is persisted is exactly
// what it runs, a stored list or a preset keeps its order and gains the
// canonical stages it lacks, and a repeated name in a hand-edited file or an
// imported preset is dropped rather than packed. AetherTX's gate and de-esser
// are retired: a saved order that names them loads without them, an operator
// who had the gate on gets RN2 instead, once, and both stages' keys go.
// Isolated settings, no audio device, no radio.

#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/AudioEngine.h"
#include "core/ChannelStripPresets.h"
#include "core/SettingsPaths.h"
#include "core/TxChainLayout.h"

#include <QJsonArray>
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
    void aGateThatWasOnBecomesRn2Once();
    void aGateThatWasOffLeavesRn2Alone();
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

void TxChainSettingsTest::aGateThatWasOnBecomesRn2Once()
{
    auto& s = AppSettings::instance();
    s.setValue("AetherialTubePreampTx", "{}");
    s.setValue("ClientGateTxEnabled", "True");
    s.setValue("ClientGateTxThresholdDb", "-38");
    s.setValue("ClientDeEssTxEnabled", "True");
    s.setValue("ClientDeEssTxAmountDb", "-9");
    {
        AudioEngine engine;
        QVERIFY(engine.rn2TxEnabled());
    }
    QVERIFY(!s.contains("ClientGateTxEnabled"));
    QVERIFY(!s.contains("ClientGateTxThresholdDb"));
    // The de-esser has no replacement; its keys just go.
    QVERIFY(!s.contains("ClientDeEssTxEnabled"));
    QVERIFY(!s.contains("ClientDeEssTxAmountDb"));
    // RN2 was persisted; and with the keys gone the migration cannot run
    // again, so switching RN2 off sticks.
    {
        AudioEngine engine;
        QVERIFY(engine.rn2TxEnabled());
        engine.setRn2TxEnabled(false);
    }
    AudioEngine engine;
    QVERIFY(!engine.rn2TxEnabled());
}

void TxChainSettingsTest::aGateThatWasOffLeavesRn2Alone()
{
    auto& s = AppSettings::instance();
    s.setValue("AetherialTubePreampTx", "{}");
    s.setValue("ClientGateTxEnabled", "False");
    s.setValue("ClientGateTxRatio", "3");
    AudioEngine engine;
    QVERIFY(!engine.rn2TxEnabled());
    QVERIFY(!s.contains("ClientGateTxEnabled"));
    QVERIFY(!s.contains("ClientGateTxRatio"));
}

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("tx-chain-settings-test"));
    if (!profile.isValid()) {
        return 1;
    }
    QCoreApplication app(argc, argv);
    TxChainSettingsTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tx_chain_settings_test.moc"
