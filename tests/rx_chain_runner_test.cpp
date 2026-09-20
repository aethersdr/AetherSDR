// Does the receive chain actually run in the order the operator arranged it?
//
// The question is not whether a vector came back in the right order — it is
// whether the SAMPLES differ. Reordering was cosmetic for a long time exactly
// because the stored order and the processing order were two different things,
// and every test in sight asserted on the stored one.

#include "core/AudioEngine.h"
#include "core/ClientComp.h"
#include "core/ClientEq.h"
#include "core/ClientGate.h"
#include "core/ClientPudu.h"
#include "core/ClientTube.h"
#include "core/RxChainRunner.h"

#include <QtTest>

#include <cmath>

using namespace AetherSDR;

namespace {

constexpr double kSampleRate = 48000.0;
constexpr int    kFrames     = 4096;

// Pack stages into the uint64 the engine stores, low slot first.
uint64_t pack(std::initializer_list<AudioEngine::RxChainStage> stages)
{
    uint64_t packed = 0;
    int slot = 0;
    for (auto s : stages) {
        packed |= (static_cast<uint64_t>(s) & 0xFF) << (slot * 8);
        ++slot;
    }
    return packed;   // remaining slots stay None, which terminates the walk
}

// A quiet steady tone, well below the gate threshold used below.
QByteArray tone(float amplitude)
{
    QByteArray pcm;
    pcm.resize(kFrames * 2 * static_cast<int>(sizeof(float)));
    auto* f = reinterpret_cast<float*>(pcm.data());
    for (int i = 0; i < kFrames; ++i) {
        const auto v = static_cast<float>(
            amplitude * std::sin(2.0 * M_PI * 700.0 * i / kSampleRate));
        f[i * 2] = v;
        f[i * 2 + 1] = v;
    }
    return pcm;
}

float rms(const QByteArray& pcm)
{
    const int n = pcm.size() / static_cast<int>(sizeof(float));
    const auto* f = reinterpret_cast<const float*>(pcm.constData());
    double acc = 0.0;
    for (int i = 0; i < n; ++i) acc += double(f[i]) * f[i];
    return n > 0 ? float(std::sqrt(acc / n)) : 0.0f;
}

} // namespace

class RxChainRunnerTest : public QObject {
    Q_OBJECT

private slots:
    void orderChangesTheSamples();
    void aDisabledStageIsSkippedButKeepsItsPlace();
    void theTxBypassSkipsEveryStage();
    void theEqTapFollowsTheEqSlot();
    void anEmptyChainReturnsTheInputUntouched();
};

// The decisive one. A boost EQ in front of a gate lifts the tone over the
// threshold and the gate passes it; the same two stages the other way round
// leave the gate looking at the quiet original, so it closes and the boost
// then only amplifies what little got through.
void RxChainRunnerTest::orderChangesTheSamples()
{
    ClientEq eq;
    eq.prepare(kSampleRate);
    eq.setEnabled(true);
    // Bands at their flat defaults: ClientEq::process() returns immediately
    // with an active count of zero, so the master gain would never apply.
    eq.setActiveBandCount(10);
    eq.setMasterGain(4.0f);        // clamped there anyway

    ClientGate gate;
    gate.prepare(kSampleRate);
    gate.setEnabled(true);
    gate.setMode(ClientGate::Mode::Gate);
    gate.setThresholdDb(-30.0f);
    gate.setFloorDb(-80.0f);
    gate.setHoldMs(0.0f);
    gate.setReleaseMs(5.0f);

    RxChainModules modules;
    modules.eq = &eq;
    modules.gate = &gate;

    const QByteArray input = tone(0.02f);   // ~-34 dBFS: below the threshold

    RxChainScratch scratchA;
    const QByteArray* eqFirst = runRxChain(
        pack({AudioEngine::RxChainStage::Eq, AudioEngine::RxChainStage::Gate}),
        input, modules, scratchA, /*bypass=*/false);
    const float eqFirstRms = rms(*eqFirst);

    eq.reset();
    gate.reset();

    RxChainScratch scratchB;
    const QByteArray* gateFirst = runRxChain(
        pack({AudioEngine::RxChainStage::Gate, AudioEngine::RxChainStage::Eq}),
        input, modules, scratchB, /*bypass=*/false);
    const float gateFirstRms = rms(*gateFirst);

    // Both orders must actually have produced audio...
    QVERIFY(eqFirstRms > 0.0f);
    // ...and boosting before the gate must get materially more through it.
    QVERIFY2(eqFirstRms > gateFirstRms * 1.5f,
             qPrintable(QStringLiteral("eq-first RMS %1 vs gate-first %2")
                            .arg(double(eqFirstRms)).arg(double(gateFirstRms))));
}

void RxChainRunnerTest::aDisabledStageIsSkippedButKeepsItsPlace()
{
    ClientEq eq;
    eq.prepare(kSampleRate);
    eq.setEnabled(false);            // present in the order, switched off
    eq.setActiveBandCount(10);
    eq.setMasterGain(4.0f);

    RxChainModules modules;
    modules.eq = &eq;

    const QByteArray input = tone(0.05f);
    RxChainScratch scratch;
    const QByteArray* out = runRxChain(
        pack({AudioEngine::RxChainStage::Eq}), input, modules, scratch, false);

    QCOMPARE(out, &input);           // untouched, not merely equal
}

void RxChainRunnerTest::theTxBypassSkipsEveryStage()
{
    ClientEq eq;
    eq.prepare(kSampleRate);
    eq.setEnabled(true);
    eq.setActiveBandCount(10);
    eq.setMasterGain(4.0f);

    RxChainModules modules;
    modules.eq = &eq;

    const QByteArray input = tone(0.05f);
    RxChainScratch scratch;
    const QByteArray* out = runRxChain(
        pack({AudioEngine::RxChainStage::Eq}), input, modules, scratch,
        /*bypass=*/true);

    QCOMPARE(out, &input);
}

// The analyzer tap means "after the EQ", so it has to follow the EQ when the
// operator moves it rather than staying at the second position.
void RxChainRunnerTest::theEqTapFollowsTheEqSlot()
{
    ClientEq eq;
    eq.prepare(kSampleRate);
    eq.setEnabled(true);
    eq.setActiveBandCount(10);
    eq.setMasterGain(4.0f);

    ClientGate gate;
    gate.prepare(kSampleRate);
    gate.setEnabled(false);          // present but inert, so only EQ moves it

    RxChainModules modules;
    modules.eq = &eq;
    modules.gate = &gate;

    const QByteArray input = tone(0.05f);
    const float inputRms = rms(input);

    RxChainScratch scratch;
    const QByteArray* postEq = nullptr;
    runRxChain(pack({AudioEngine::RxChainStage::Gate,
                     AudioEngine::RxChainStage::Eq}),
               input, modules, scratch, false, &postEq);

    QVERIFY(postEq);
    // EQ ran last, so the tap must show the boosted signal — not the input it
    // would have shown had the tap stayed pinned to the second slot.
    QVERIFY2(rms(*postEq) > inputRms * 1.5f,
             qPrintable(QStringLiteral("post-EQ RMS %1 vs input %2")
                            .arg(double(rms(*postEq))).arg(double(inputRms))));
}

void RxChainRunnerTest::anEmptyChainReturnsTheInputUntouched()
{
    RxChainModules modules;      // no modules at all
    RxChainScratch scratch;
    const QByteArray input = tone(0.05f);

    const QByteArray* out = runRxChain(0, input, modules, scratch, false);
    QCOMPARE(out, &input);
}

QTEST_MAIN(RxChainRunnerTest)
#include "rx_chain_runner_test.moc"
