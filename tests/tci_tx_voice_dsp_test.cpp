// RFC #7066: opt-in TX voice DSP for TCI transmit audio.
//
// Socket-free. Drives the real AudioEngine through injected PCM and reads the
// host-modulation transport seam (txTransportPcmReady), as
// tx_audio_context_test does. Covers the routing the ruling asks for (toggle
// off, toggle on, DAX untouched) and the L/R fold to mono.
#include "core/AudioEngine.h"
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

    Rig()
        : producer(coordinator.registerProducer())
    {
        const auto actor = coordinator.registerActor({true, 0});
        const auto op = coordinator.acquire(actor, TxCoordinator::monotonicMs()).operation;
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
};

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
    for (int b = 0; b < kBlocks; ++b) {
        rig.audio.feedTciTxAudio(toneBlock(b, 0.0f, 0.5f), rig.live);
    }
    check(!rig.transport.isEmpty() && peakOf(rig.joined()) > 0.05,
          "toggle on: a client that fills only the right channel still transmits");
}

void daxUntouched()
{
    Rig on;
    Rig off;
    on.audio.setTciTxVoiceDsp(true);
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
    QCoreApplication app(argc, argv);
    foldToMono();
    offIsTheDaxPath();
    onRunsTheChain();
    onFoldsOneSidedClient();
    daxUntouched();
    return failures ? 1 : 0;
}
