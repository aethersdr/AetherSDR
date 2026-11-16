// The stock AetherTX presets. Each one, applied to a real AudioEngine, reads
// back key for key (so nothing in it is ignored or clamped), runs the tuned
// chain (Peak control in the modulator route, LIM on at -0.5 dB), leaves RN2
// to the operator, carries its TX filter, and adds the latency the docs give.
// Isolated settings, no audio device, no radio.

#include "TestSettingsProfile.h"
#include "core/AetherTxProfiles.h"
#include "core/AetherTxStockProfiles.h"
#include "core/AppSettings.h"
#include "core/AudioEngine.h"
#include "core/ChannelStripPresets.h"
#include "core/ClientCessb.h"
#include "core/ClientComp.h"
#include "core/ClientEq.h"
#include "core/ClientMbLimiter.h"
#include "core/ClientFinalLimiter.h"
#include "core/SettingsPaths.h"
#include "core/TxChainLayout.h"
#include "core/TxVoiceProcessor.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QStandardPaths>
#include <QtTest>

#include <cmath>

using namespace AetherSDR;

namespace {

// Every leaf of `want` must be in `got` with the same value (numbers to
// 1e-4). Returns the first mismatch's path, or empty.
QString firstMismatch(const QJsonValue& want, const QJsonValue& got, const QString& path)
{
    if (want.isObject()) {
        if (!got.isObject()) return path;
        const QJsonObject w = want.toObject(), g = got.toObject();
        for (auto it = w.begin(); it != w.end(); ++it) {
            const QString m = firstMismatch(it.value(), g.value(it.key()), path + '.' + it.key());
            if (!m.isEmpty()) return m;
        }
        return {};
    }
    if (want.isArray()) {
        if (!got.isArray() || got.toArray().size() != want.toArray().size()) return path;
        for (int i = 0; i < want.toArray().size(); ++i) {
            const QString m = firstMismatch(want.toArray().at(i), got.toArray().at(i),
                                            path + QStringLiteral("[%1]").arg(i));
            if (!m.isEmpty()) return m;
        }
        return {};
    }
    if (want.isDouble()) {
        return got.isDouble() && std::abs(want.toDouble() - got.toDouble()) < 1e-4 ? QString() : path;
    }
    return want == got ? QString() : path;
}

} // namespace

class AetherTxStockProfilesTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void threeStockPresetsInOrder();
    void eachAppliesKeyForKey_data();
    void eachAppliesKeyForKey();
    void eachRunsTheTunedChain_data();
    void eachRunsTheTunedChain();
    void rn2StaysTheOperatorsChoice();
    void eachCarriesItsTxFilter();
    void latencyIsPeakControlPlusLim_data();
    void latencyIsPeakControlPlusLim();
};

void AetherTxStockProfilesTest::initTestCase()
{
    const QString isolatedDir = qEnvironmentVariable("AETHER_SETTINGS_DIR");
    QVERIFY(!isolatedDir.isEmpty());
    QCOMPARE(SettingsPaths::configDir(), isolatedDir);
    QVERIFY(QStandardPaths::isTestModeEnabled());
}

void AetherTxStockProfilesTest::threeStockPresetsInOrder()
{
    QCOMPARE(AetherTxStockProfiles::names(),
             (QStringList{"SSB 3 kHz", "DX Punch 2.7 kHz", "ESSB 6 kHz"}));
    QVERIFY(AetherTxStockProfiles::contains(QStringLiteral("ssb 3 khz")));  // any case
    QCOMPARE(AetherTxStockProfiles::canonicalName(QStringLiteral(" essb 6 khz ")),
             QStringLiteral("ESSB 6 kHz"));
    QVERIFY(!AetherTxStockProfiles::contains(QStringLiteral("SSB")));
    QVERIFY(AetherTxStockProfiles::profile(QStringLiteral("SSB")).isEmpty());
}

void AetherTxStockProfilesTest::eachAppliesKeyForKey_data()
{
    QTest::addColumn<QString>("name");
    for (const QString& n : AetherTxStockProfiles::names()) {
        QTest::newRow(qPrintable(n)) << n;
    }
}

// Applied, then captured, the engine holds exactly the preset: no key is
// unread and no value is clamped to something else.
void AetherTxStockProfilesTest::eachAppliesKeyForKey()
{
    QFETCH(QString, name);
    AudioEngine engine;
    AetherTxProfiles lib(&engine);
    QVERIFY(lib.loadStockProfile(name));
    const QJsonObject want = AetherTxStockProfiles::profile(name);
    QVERIFY(!want.isEmpty());
    const QJsonObject got = ChannelStripPresets::captureTxJson(&engine);
    QCOMPARE(firstMismatch(want, got, QStringLiteral("$")), QString());
}

void AetherTxStockProfilesTest::eachRunsTheTunedChain_data()
{
    eachAppliesKeyForKey_data();
}

void AetherTxStockProfilesTest::eachRunsTheTunedChain()
{
    QFETCH(QString, name);
    AudioEngine engine;
    AetherTxProfiles lib(&engine);
    QVERIFY(lib.loadStockProfile(name));
    auto* peak = engine.clientCessbTx();
    QVERIFY(peak->isEnabled());
    QVERIFY(peak->overshootControl());
    // The modulator where the radio has one (HL2), the chain elsewhere.
    QCOMPARE(peak->route(), ClientCessb::Route::Modulator);
    auto* lim = engine.clientFinalLimiterTx();
    QVERIFY(lim->isEnabled());
    QCOMPARE(lim->ceilingDb(), -0.5f);
    QCOMPARE(lim->lookaheadMs(), 1.5f);
    QVERIFY(engine.clientCompTx()->isEnabled());
    QVERIFY(engine.clientEqTx()->isEnabled());
    QVERIFY(!engine.clientMbLimiterTx()->isEnabled());
}

void AetherTxStockProfilesTest::rn2StaysTheOperatorsChoice()
{
    AudioEngine engine;
    AetherTxProfiles lib(&engine);
    for (const bool on : {true, false}) {
        engine.setRn2TxEnabled(on);
        for (const QString& n : AetherTxStockProfiles::names()) {
            QVERIFY(!AetherTxStockProfiles::profile(n).contains(QStringLiteral("rn2")));
            QVERIFY(lib.loadStockProfile(n));
            QCOMPARE(engine.rn2TxEnabled(), on);
        }
    }
}

void AetherTxStockProfilesTest::eachCarriesItsTxFilter()
{
    const auto f = [](const char* n) { return *AetherTxStockProfiles::txFilter(QString::fromLatin1(n)); };
    QCOMPARE(f("SSB 3 kHz").lowHz, 100);
    QCOMPARE(f("SSB 3 kHz").highHz, 3100);
    QCOMPARE(f("DX Punch 2.7 kHz").lowHz, 200);
    QCOMPARE(f("DX Punch 2.7 kHz").highHz, 2900);
    QCOMPARE(f("ESSB 6 kHz").lowHz, 50);
    QCOMPARE(f("ESSB 6 kHz").highHz, 6050);
    QVERIFY(!AetherTxStockProfiles::txFilter(QStringLiteral("not stock")));
}

void AetherTxStockProfilesTest::latencyIsPeakControlPlusLim_data()
{
    eachAppliesKeyForKey_data();
}

// The docs give each preset's added delay as Peak control plus LIM, about
// 41 ms: pin it through the processor the app runs.
void AetherTxStockProfilesTest::latencyIsPeakControlPlusLim()
{
    QFETCH(QString, name);
    AudioEngine engine;
    AetherTxProfiles lib(&engine);
    QVERIFY(lib.loadStockProfile(name));
    const auto f = *AetherTxStockProfiles::txFilter(name);
    engine.setTxFilterPassband(f.lowHz, f.highHz);

    TxVoiceProcessor bypass;
    QVERIFY(bypass.prepare(48000, 480));
    TxVoiceProcessor processor;
    TxVoiceProcessor::Processors p;
    p.eq = engine.clientEqTx();
    p.comp = engine.clientCompTx();
    p.tube = engine.clientTubeTx();
    p.pudu = engine.clientPuduTx();
    p.reverb = engine.clientReverbTx();
    p.cfc = engine.clientCfcTx();
    p.leveler = engine.clientLevelerTx();
    p.cessb = engine.clientCessbTx();
    p.mbLimiter = engine.clientMbLimiterTx();
    p.finalLimiter = engine.clientFinalLimiterTx();
    processor.setProcessors(p);
    std::vector<TxVoiceProcessor::Stage> stages;
    for (const auto s : engine.txChainStages()) {
        stages.push_back(static_cast<TxVoiceProcessor::Stage>(static_cast<uint8_t>(s)));
    }
    uint64_t order = 0;
    QVERIFY(TxChainLayout::pack(stages, order));
    processor.setStageOrder(order);
    QVERIFY(processor.prepare(48000, 480));

    const int added = processor.latencyFrames() - bypass.latencyFrames();
    QCOMPARE(added, engine.clientCessbTx()->latencyFrames() + engine.clientFinalLimiterTx()->latencyFrames());
    const double ms = added / 48.0;
    QVERIFY2(ms > 40.0 && ms < 42.0, qPrintable(QStringLiteral("added %1 ms").arg(ms)));
}

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("aether-tx-stock-profiles-test"));
    if (!profile.isValid()) {
        return 1;
    }
    QCoreApplication app(argc, argv);
    AppSettings::instance().load();
    AetherTxStockProfilesTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "aether_tx_stock_profiles_test.moc"
