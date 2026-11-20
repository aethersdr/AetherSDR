// RFC #7066: opt-in TX voice DSP for TCI transmit audio.
//
// Socket-free. Drives the real AudioEngine through injected PCM and reads the
// host-modulation transport seam (txTransportPcmReady), as
// tx_audio_context_test does. Covers the routing the ruling asks for (toggle
// off, toggle on, DAX untouched), the digital-mode bypass with the toggle on
// (#7379 review), the L/R fold to mono, that the chain really changes the
// audio and leaves the mic-only test tone out, and that no state carries from
// one over into the next.
#include "TestSettingsProfile.h"

#include "core/AudioEngine.h"
#include "core/ClientComp.h"
#include "core/ClientTxTestTone.h"
#include "core/backends/TxAudioSource.h"
#include "core/TxCoordinator.h"

#include <QCoreApplication>
#include <QVector>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

using namespace AetherSDR;

namespace {

int failures = 0;

void check(bool condition, const char* claim)
{
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", claim);
    failures += !condition;
}

constexpr int kFrames = 480;   // 20 ms at 24 kHz
constexpr int kBlocks = 25;    // 0.5 s, well past any chain priming

// Interleaved stereo float32 at 24 kHz: a 1 kHz tone at the given level on
// each side, continuing its phase across blocks.
QByteArray toneBlock(int block, float left, float right)
{
    QByteArray pcm(kFrames * 2 * static_cast<int>(sizeof(float)), Qt::Uninitialized);
    auto* s = reinterpret_cast<float*>(pcm.data());
    for (int i = 0; i < kFrames; ++i) {
        const double t = static_cast<double>(block * kFrames + i) / 24000.0;
        const float v = static_cast<float>(std::sin(2.0 * 3.14159265358979323846 * 1000.0 * t));
        s[2 * i] = left * v;
        s[2 * i + 1] = right * v;
    }
    return pcm;
}

struct Rig {
    AudioEngine audio;
    TxCoordinator coordinator{[](const auto&, auto) {}};
    TxCoordinator::Context live;
    QVector<QByteArray> transport;
    TxCoordinator::Producer producer;
    TxCoordinator::Actor actor;
    TxCoordinator::Operation op;

    Rig()
        : producer(coordinator.registerProducer())
    {
        actor = coordinator.registerActor({true, 0});
        op = coordinator.acquire(actor, TxCoordinator::monotonicMs()).operation;
        live = coordinator.mediaContext(producer, op);
        QObject::connect(&audio, &AudioEngine::txTransportPcmReady,
                         [this](const QByteArray& pcm, TxAudioSource, const TxCoordinator::Context&) {
            transport.append(pcm);
        });
        audio.setDaxTxAudioRoute(0x1234, DaxTxAudioPermit::confirmed(0x1234));
        audio.setTransmitting(true);
        audio.setHostModulation(true);
    }

    QByteArray joined() const
    {
        QByteArray all;
        for (const auto& b : transport) { all += b; }
        return all;
    }

    // Unkey and key again: the next blocks belong to a new over, as
    // TciIoWorker gives each over its own context.
    void nextOver()
    {
        (void)coordinator.finishLocalIntent(op);
        op = coordinator.acquire(actor, TxCoordinator::monotonicMs()).operation;
        live = coordinator.mediaContext(producer, op);
        transport.clear();
    }
};

QByteArray silenceBlock()
{
    return QByteArray(kFrames * 2 * static_cast<int>(sizeof(float)), '\0');
}

double peakOf(const QByteArray& pcm)
{
    // The transport seam is 24 kHz stereo Int16 on the host-modulation path.
    const auto* s = reinterpret_cast<const qint16*>(pcm.constData());
    const int n = pcm.size() / static_cast<int>(sizeof(qint16));
    double peak = 0.0;
    for (int i = 0; i < n; ++i) { peak = std::max(peak, std::fabs(s[i] / 32768.0)); }
    return peak;
}

void foldToMono()
{
    QByteArray pcm(4 * 2 * static_cast<int>(sizeof(float)), Qt::Uninitialized);
    auto* s = reinterpret_cast<float*>(pcm.data());
    const float in[8] = {1.0f, 0.0f, -0.2f, 0.6f, 0.0f, -1.0f, 0.25f, 0.25f};
    std::memcpy(s, in, sizeof(in));
    AudioEngine::foldTciStereoToMono(pcm);
    const float want[4] = {0.5f, 0.2f, -0.5f, 0.25f};
    bool ok = pcm.size() == static_cast<int>(sizeof(in));
    for (int i = 0; i < 4; ++i) {
        ok = ok && std::fabs(s[2 * i] - want[i]) < 1e-6f && s[2 * i] == s[2 * i + 1];
    }
    check(ok, "fold: each frame becomes (L + R) / 2 on both channels, length unchanged");

    QByteArray empty;
    AudioEngine::foldTciStereoToMono(empty);
    check(empty.isEmpty(), "fold: an empty block stays empty");
}

void offIsTheDaxPath()
{
    Rig tci;
    Rig dax;
    check(!tci.audio.tciTxVoiceDsp(), "off by default");
    for (int b = 0; b < kBlocks; ++b) {
        tci.audio.feedTciTxAudio(toneBlock(b, 0.5f, 0.5f), tci.live);
        dax.audio.feedDaxTxAudio(toneBlock(b, 0.5f, 0.5f), dax.live);
    }
    check(!tci.transport.isEmpty() && tci.joined() == dax.joined(),
          "toggle off: TCI audio reaches transport byte for byte as the DAX path sends it");
    check(tci.audio.tciTxVoiceBlocks() == 0, "toggle off: the TCI voice chain never runs");
}

void onRunsTheChain()
{
    Rig rig;
    rig.audio.setTciTxVoiceDsp(true);
    rig.audio.setTciTxVoiceMode(true);
    for (int b = 0; b < kBlocks; ++b) {
        rig.audio.feedTciTxAudio(toneBlock(b, 0.5f, 0.5f), rig.live);
    }
    check(rig.audio.tciTxVoiceBlocks() > 0, "toggle on: TCI audio runs through the voice chain");
    check(!rig.transport.isEmpty() && peakOf(rig.joined()) > 0.05,
          "toggle on: the chain's output reaches transport");
}

void onFoldsOneSidedClient()
{
    Rig rig;
    rig.audio.setTciTxVoiceDsp(true);
    rig.audio.setTciTxVoiceMode(true);
    for (int b = 0; b < kBlocks; ++b) {
        rig.audio.feedTciTxAudio(toneBlock(b, 0.0f, 0.5f), rig.live);
    }
    check(!rig.transport.isEmpty() && peakOf(rig.joined()) > 0.05,
          "toggle on: a client that fills only the right channel still transmits");
}

void onWithDigitalModeIsTheDaxPath()
{
    // WSJT-X, JTDX, fldigi: the TX slice is in DIGU/DIGL/RTTY/FDV*, so
    // TciServer leaves the voice-mode gate closed even with the option on.
    Rig tci;
    Rig dax;
    tci.audio.setTciTxVoiceDsp(true);
    check(!tci.audio.tciTxVoiceMode(), "voice-mode gate closed by default");
    for (int b = 0; b < kBlocks; ++b) {
        tci.audio.feedTciTxAudio(toneBlock(b, 0.5f, 0.0f), tci.live);
        dax.audio.feedDaxTxAudio(toneBlock(b, 0.5f, 0.0f), dax.live);
    }
    check(tci.audio.tciTxVoiceBlocks() == 0,
          "toggle on, digital TX mode: the TCI voice chain never runs");
    check(!tci.transport.isEmpty() && tci.joined() == dax.joined(),
          "toggle on, digital TX mode: TCI audio reaches transport byte for byte as the DAX path sends it");
}

void modeSwitchClosesAndReopens()
{
    Rig rig;
    rig.audio.setTciTxVoiceDsp(true);
    rig.audio.setTciTxVoiceMode(true);
    for (int b = 0; b < kBlocks; ++b) {
        rig.audio.feedTciTxAudio(toneBlock(b, 0.5f, 0.5f), rig.live);
    }
    const quint64 voiceBlocks = rig.audio.tciTxVoiceBlocks();
    rig.audio.setTciTxVoiceMode(false);
    for (int b = 0; b < kBlocks; ++b) {
        rig.audio.feedTciTxAudio(toneBlock(b, 0.5f, 0.5f), rig.live);
    }
    check(voiceBlocks > 0 && rig.audio.tciTxVoiceBlocks() == voiceBlocks,
          "a switch to a digital mode stops the chain at the next block");
    rig.audio.setTciTxVoiceMode(true);
    for (int b = 0; b < kBlocks; ++b) {
        rig.audio.feedTciTxAudio(toneBlock(b, 0.5f, 0.5f), rig.live);
    }
    check(rig.audio.tciTxVoiceBlocks() > voiceBlocks,
          "a switch back to a voice mode runs the chain again");
}

void onChangesTheAudio()
{
    // A stage with an audible effect: compressor makeup below threshold is
    // plain gain, so the chain's output must be louder than the same chain
    // without it. A bypass that only bumped the block counter would not be.
    Rig plain;
    Rig comp;
    for (Rig* r : {&plain, &comp}) {
        r->audio.setTciTxVoiceDsp(true);
        r->audio.setTciTxVoiceMode(true);
    }
    ClientComp* c = comp.audio.clientCompTx();
    c->setThresholdDb(0.0f);
    c->setRatio(2.0f);
    c->setMakeupDb(6.0f);
    c->setEnabled(true);
    for (int b = 0; b < kBlocks; ++b) {
        plain.audio.feedTciTxAudio(toneBlock(b, 0.2f, 0.2f), plain.live);
        comp.audio.feedTciTxAudio(toneBlock(b, 0.2f, 0.2f), comp.live);
    }
    const double p = peakOf(plain.joined());
    const double q = peakOf(comp.joined());
    std::printf("  plain peak %.4f, with +6 dB makeup %.4f\n", p, q);
    check(p > 0.05 && q > 1.5 * p,
          "toggle on: a TX stage's effect (compressor makeup) is on the TCI audio");
}

void onLeavesTheTestToneOut()
{
    // The test tone is a mic-path feature: with it on, TCI silence through
    // the chain must stay silence.
    Rig rig;
    rig.audio.clientTxTestTone()->setEnabled(true);
    rig.audio.setTciTxVoiceDsp(true);
    rig.audio.setTciTxVoiceMode(true);
    for (int b = 0; b < kBlocks; ++b) {
        rig.audio.feedTciTxAudio(silenceBlock(), rig.live);
    }
    check(rig.audio.tciTxVoiceBlocks() > 0 && !rig.transport.isEmpty()
              && peakOf(rig.joined()) == 0.0,
          "toggle on: the mic test tone never reaches TCI audio");
}

void noStateAcrossOvers()
{
    // Over 1 is a tone, over 2 digital silence under a new context. The
    // chain holds back a few ms; without a reset on the new context that
    // tail went out at the start of over 2 (#7379 review, finding 1).
    Rig rig;
    rig.audio.setTciTxVoiceDsp(true);
    rig.audio.setTciTxVoiceMode(true);
    for (int b = 0; b < kBlocks; ++b) {
        rig.audio.feedTciTxAudio(toneBlock(b, 0.5f, 0.5f), rig.live);
    }
    const double firstOverPeak = peakOf(rig.joined());
    rig.nextOver();
    for (int b = 0; b < 5; ++b) {
        rig.audio.feedTciTxAudio(silenceBlock(), rig.live);
    }
    const double secondOverPeak = peakOf(rig.joined());
    std::printf("  first over peak %.4f, second over peak %.4f\n",
                firstOverPeak, secondOverPeak);
    check(firstOverPeak > 0.05 && !rig.transport.isEmpty() && secondOverPeak == 0.0,
          "a new over starts clean: nothing of the previous over is transmitted");
}

void daxUntouched()
{
    Rig on;
    Rig off;
    on.audio.setTciTxVoiceDsp(true);
    on.audio.setTciTxVoiceMode(true);
    for (int b = 0; b < kBlocks; ++b) {
        on.audio.feedDaxTxAudio(toneBlock(b, 0.5f, 0.0f), on.live);
        off.audio.feedDaxTxAudio(toneBlock(b, 0.5f, 0.0f), off.live);
    }
    check(on.audio.tciTxVoiceBlocks() == 0, "toggle on: DAX audio never enters the voice chain");
    check(!on.transport.isEmpty() && on.joined() == off.joined(),
          "toggle on: DAX audio reaches transport exactly as with the toggle off");
}

} // namespace

int main(int argc, char** argv)
{
    // Keep the engine out of the operator's config directory.
    TestSettingsProfile profile(QStringLiteral("tci-tx-voice-dsp-test"));
    if (!profile.isValid()) {
        return 1;
    }
    QCoreApplication app(argc, argv);
    foldToMono();
    offIsTheDaxPath();
    onRunsTheChain();
    onFoldsOneSidedClient();
    onWithDigitalModeIsTheDaxPath();
    modeSwitchClosesAndReopens();
    onChangesTheAudio();
    onLeavesTheTestToneOut();
    noStateAcrossOvers();
    daxUntouched();
    return failures ? 1 : 0;
}
