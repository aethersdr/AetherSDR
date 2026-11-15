// The TX chain order between AudioEngine, the audio thread and the settings
// file: the audio thread gets the 4-bit layout, what is persisted is exactly
// what it runs, a stored list or a preset keeps its order and gains the
// canonical stages it lacks, and a repeated name in a hand-edited file or an
// imported preset is dropped rather than packed.
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
// hand TxVoiceProcessor an order it reads as Gate alone.
void TxChainSettingsTest::theAudioThreadGetsTheNibbleLayout()
{
    AudioEngine engine;
    const QVector<Stage> order{Stage::Reverb, Stage::Enh, Stage::Tube, Stage::Comp,
                               Stage::DeEss, Stage::Eq, Stage::Gate};
    engine.setTxChainStages(order);
    uint64_t expected = 0;
    QVERIFY(TxChainLayout::pack(order, expected));
    QCOMPARE(engine.txChainPackedForTest(), expected);
}

void TxChainSettingsTest::persistsExactlyWhatRuns()
{
    AudioEngine engine;
    engine.setTxChainStages({Stage::Comp, Stage::Gate, Stage::Comp, Stage::Eq});
    const QVector<Stage> expected{Stage::Comp, Stage::Gate, Stage::Eq, Stage::DeEss,
                                  Stage::Tube, Stage::Enh, Stage::Reverb};
    QCOMPARE(engine.txChainStages(), expected);
    QCOMPARE(stored(), QStringLiteral("Comp,Gate,Eq,DeEss,Tube,Enh,Reverb"));
}

void TxChainSettingsTest::loadKeepsTheSavedOrderAndAppendsTheRest()
{
    AppSettings::instance().setValue(kKey, "Tube,Comp,Gate");
    AudioEngine engine;
    engine.loadClientCompSettings();
    const QVector<Stage> expected{Stage::Tube, Stage::Comp, Stage::Gate, Stage::Eq,
                                  Stage::DeEss, Stage::Enh, Stage::Reverb};
    QCOMPARE(engine.txChainStages(), expected);
}

void TxChainSettingsTest::loadDropsRepeatedNames()
{
    // Seventeen names: more than the packed order holds, all of them repeats
    // of stages that exist. They load as the seven stages, in first-seen order.
    QStringList names;
    for (int i = 0; i < 17; ++i) {
        names << (i % 2 ? QStringLiteral("Eq") : QStringLiteral("Reverb"));
    }
    AppSettings::instance().setValue(kKey, names.join(','));
    AudioEngine engine;
    engine.loadClientCompSettings();
    const QVector<Stage> expected{Stage::Reverb, Stage::Eq, Stage::Gate, Stage::DeEss,
                                  Stage::Comp, Stage::Tube, Stage::Enh};
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
    const QVector<Stage> expected{Stage::Tube, Stage::Gate, Stage::Eq, Stage::DeEss,
                                  Stage::Comp, Stage::Enh, Stage::Reverb};
    QCOMPARE(engine.txChainStages(), expected);
    QCOMPARE(stored(), QStringLiteral("Tube,Gate,Eq,DeEss,Comp,Enh,Reverb"));
}

void TxChainSettingsTest::freshInstallGetsTheCanonicalOrder()
{
    AppSettings::instance().remove(kKey);
    AppSettings::instance().remove("ClientCompTxChainOrder");
    AudioEngine engine;
    engine.loadClientCompSettings();
    const QVector<Stage> expected{Stage::Gate, Stage::Eq, Stage::DeEss, Stage::Comp,
                                  Stage::Tube, Stage::Enh, Stage::Reverb};
    QCOMPARE(engine.txChainStages(), expected);
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
