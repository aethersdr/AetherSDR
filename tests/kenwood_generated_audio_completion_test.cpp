// Production generated-audio route, DSP/barrier, model and Kenwood timer.
// All transport writes are injected; no sockets, devices, credentials or RF.
#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/AudioEngine.h"
#include "core/GeneratedAudioTransmitter.h"
#include "core/VoiceKeyerTxRoute.h"
#include "core/backends/kenwood/KenwoodBackend.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QTimer>
#include <QList>
#include <QString>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>

namespace AetherSDR {
class TxOperationIntegrationTestAccess {
public:
    static void caps(RadioModel& model) { model.publishCapabilities(model.isConnected()); }
    static void pcm(RadioModel& model, const QByteArray& pcm, TxAudioSource source, const TxCoordinator::Context& context)
    { model.submitTxAudio(pcm, 24000, source, context); }
    static void finish(RadioModel& model, quint64 token, const TxCoordinator::Context& context)
    { model.finishTxAudio(token, context); }
};
namespace kenwood {
struct KenwoodBackendTestAccess {
    static void authenticate(KenwoodBackend& backend)
    {
        backend.m_clock.start();
        backend.m_protocol.start("user", "password", false, 0);
        backend.m_protocol.receive("##CN1;", 1);
        backend.m_protocol.receive("##ID1;", 2);
        backend.ready();
    }
    static void feed(KenwoodBackend& backend, const QByteArray& message) { backend.adopt(message); }
};
}
}
using namespace AetherSDR;
namespace {
void require(bool value, const char* claim)
{
    if (!value) { std::cerr << claim << '\n'; std::exit(1); }
}
void spin(int milliseconds)
{
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
}
QByteArray signal()
{
    QByteArray pcm(24000 * 600 / 1000 * 2 * sizeof(float), '\0');
    for (int frame = 0; frame < 14400; ++frame) {
        const int markerHz = frame < 1200 ? 600 : frame >= 13200 ? 1000 : 0;
        const float value = float(0.45 * std::sin(2 * 3.141592653589793 * markerHz * frame / 24000));
        std::memcpy(pcm.data() + frame * 8, &value, sizeof(value));
        std::memcpy(pcm.data() + frame * 8 + 4, &value, sizeof(value));
    }
    return pcm;
}
void run(bool cancel)
{
    QElapsedTimer clock;
    clock.start();
    RadioModel radio;
    AudioEngine audio;
    audio.setHostModulation(true);
    audio.setPcMicGain(100);
    QList<QByteArray> packets;
    QList<qint64> packetTimes;
    qint64 releaseTime = -1;
    qint64 lastContentTime = -1;
    int receives = 0;
    int drainBudget = -1;
    QList<int> submittedFrames;
    QList<qint64> submitTimes;
    auto owned = std::make_unique<kenwood::KenwoodBackend>(kenwood::KenwoodBackend::Transport{
        [&](const QByteArray& command) {
            if (command == "RX;") { releaseTime = clock.elapsed(); }
            return true;
        }, [&](const QByteArray& packet) {
            packets.append(packet);
            packetTimes.append(clock.elapsed());
            for (int sample = 0; sample < 320; ++sample) {
                const int value = qFromBigEndian<quint16>(packet.constData() + 12 + sample * 2) - 32768;
                if (std::abs(value) > 10) { lastContentTime = clock.elapsed(); }
            }
            return true;
        }});
    kenwood::KenwoodBackend* backend = owned.get();
    radio.setBackendForTest(std::move(owned), QStringLiteral("kenwood"));
    require(radio.automationApplySliceFixture(0, QStringLiteral("A")), "isolated physical slice fixture");
    SliceDelta slice;
    slice.txSlice = true;
    slice.mode = QStringLiteral("USB");
    slice.panId = QStringLiteral("0");
    radio.slice(0)->applyChanges(slice);
    radio.transmitModel().setTxModeGetter([] { return QStringLiteral("USB"); });
    QObject::connect(backend, &IRadioBackend::capabilitiesChanged, &radio,
        [&] { TxOperationIntegrationTestAccess::caps(radio); });
    QObject::connect(backend, &IRadioBackend::transmitChanged, &radio,
        [&](const TransmitDelta& delta) { radio.applyBackendTransmitDeltaForTest(delta); });
    QObject::connect(backend, &IRadioBackend::keyingStateConfirmed, &radio, &RadioModel::radioTransmitConfirmed);
    QObject::connect(backend, &IRadioBackend::keyingStateConfirmed, &radio,
        [&](bool transmitting) { if (!transmitting) { ++receives; } });
    using Access = kenwood::KenwoodBackendTestAccess;
    Access::authenticate(*backend);
    for (const QByteArray& message : {QByteArray("ID024"), QByteArray("##TI1"), QByteArray("##UE1"),
         QByteArray("FR0"), QByteArray("FT0"), QByteArray("SF0000140740002"), QByteArray("MS103"), QByteArray("RX")}) {
        Access::feed(*backend, message);
    }
    backend->observePcAudioRouting(true);
    TxOperationIntegrationTestAccess::caps(radio);
    receives = 0;
    QObject::connect(&audio, &AudioEngine::txTransportPcmReady, &radio,
        [&](const QByteArray& pcm, TxAudioSource source, const TxCoordinator::Context& context) {
            submittedFrames.append(static_cast<int>(pcm.size() / 4));
            submitTimes.append(clock.elapsed());
            TxOperationIntegrationTestAccess::pcm(radio, pcm, source, context);
        });
    QObject::connect(&audio, &AudioEngine::modemTxAudioFinished, &radio,
        [&](quint64 token, const TxCoordinator::Context& context) { TxOperationIntegrationTestAccess::finish(radio, token, context); });
    VoiceKeyerTxRoute route(radio, &audio);
    GeneratedAudioTiming timing;
    timing.settleMs = 5;
    timing.leadMs = 5;
    timing.tailMs = 30;
    // Preserve production chunk and FIFO-cushion defaults.
    GeneratedAudioTransmitter transmitter(&route, nullptr, timing);
    qint64 lastHeartbeat = clock.elapsed();
    qint64 maxEventLoopGap = 0;
    QTimer heartbeat;
    heartbeat.setTimerType(Qt::PreciseTimer);
    heartbeat.setInterval(1);
    QObject::connect(&heartbeat, &QTimer::timeout, &transmitter, [&] {
        const qint64 now = clock.elapsed();
        const auto phase = transmitter.phase();
        if (phase == GeneratedAudioTransmitter::Phase::Sending
            || phase == GeneratedAudioTransmitter::Phase::Draining
            || phase == GeneratedAudioTransmitter::Phase::Tail) {
            maxEventLoopGap = std::max(maxEventLoopGap, now - lastHeartbeat);
        }
        lastHeartbeat = now;
    });
    heartbeat.start();
    QObject::connect(&radio, &RadioModel::txAudioFinished, &transmitter,
        [&](quint64 token, int milliseconds) { drainBudget = milliseconds; transmitter.onAudioFinished(token, milliseconds); });
    QObject::connect(&radio, &RadioModel::radioTransmitConfirmed, &transmitter, &GeneratedAudioTransmitter::onRadioKeyed);
    bool done = false;
    GeneratedAudioTransmitter::Outcome outcome = GeneratedAudioTransmitter::Outcome::Failed;
    QObject::connect(&transmitter, &GeneratedAudioTransmitter::finished, &radio,
        [&](GeneratedAudioTransmitter::Outcome value, const QString& reason) {
            done = true; outcome = value;
            if (value == GeneratedAudioTransmitter::Outcome::Failed) { std::cerr << reason.toStdString() << '\n'; }
        });
    QString error;
    require(transmitter.start(signal(), error), qPrintable(error));
    // Only an injected radio observation confirms keying; no socket is opened.
    QTimer::singleShot(20, &radio, [&] { Access::feed(*backend, "TX1"); });
    if (cancel) { QTimer::singleShot(200, &transmitter, &GeneratedAudioTransmitter::stop); }
    for (int step = 0; step < 200 && !done; ++step) { spin(10); }
    require(done && releaseTime >= 0, "real event-loop completion releases the original key request");
    const qsizetype sent = packets.size();
    spin(150);
    require(packets.size() == sent && receives == 0, "release discards pending media without inventing physical RX");
    struct Marker {
        int nonzero = 0;
        double real = 0;
        double imaginary = 0;
        double absolute = 0;
        void add(int sample, int position, int frequency)
        {
            if (std::abs(sample) > 10) { ++nonzero; }
            const double angle = 2 * 3.141592653589793 * frequency * position / 16000;
            real += sample * std::cos(angle);
            imaginary += sample * std::sin(angle);
            absolute += std::abs(sample);
        }
        bool present() const
        { return nonzero >= 200 && std::hypot(real, imaginary) > absolute * 0.4; }
    } initial, terminal;
    for (qsizetype packet = 0; packet < packets.size(); ++packet) {
        for (int sample = 0; sample < 320; ++sample) {
            const int position = static_cast<int>(packet * 320) + sample;
            const int value = qFromBigEndian<quint16>(packets[packet].constData() + 12 + sample * 2) - 32768;
            if (position < 1600) { initial.add(value, position, 600); }
            if (position >= 8800) { terminal.add(value, position, 1000); }
        }
    }
    if (cancel) {
        require(outcome == GeneratedAudioTransmitter::Outcome::Stopped && terminal.nonzero == 0,
            "explicit cancellation drops the unsent terminal tone and pending tail");
    } else {
        std::cout << "packets=" << packets.size() << " drainMs=" << drainBudget
            << " firstPacketMs=" << (packetTimes.isEmpty() ? -1 : packetTimes.first())
            << " lastPacketMs=" << (packetTimes.isEmpty() ? -1 : packetTimes.last())
            << " lastContentMs=" << lastContentTime << " releaseMs=" << releaseTime
            << " maxEventLoopGapMs=" << maxEventLoopGap << '\n';
        for (qsizetype index = 0; index < submittedFrames.size(); ++index) {
            std::cout << "submitFrames=" << submittedFrames[index] << " atMs=" << submitTimes[index] << '\n';
        }
        for (qsizetype index = 0; index < packetTimes.size(); ++index) {
            std::cout << "packet=" << index << " atMs=" << packetTimes[index] << '\n';
        }
        std::cout.flush();
        require(outcome == GeneratedAudioTransmitter::Outcome::Completed && drainBudget > 0,
            "actual EOF barrier obtains a positive native drain allowance");
        require(lastContentTime > 0 && lastContentTime <= releaseTime,
            "terminal tone reaches the injected transport before local release");
        require(packets.size() >= 30, "production pacing preserves complete finite stream duration");
        require(initial.present(), "initial 600 Hz marker survives in the first 100 ms of captured PCM");
        require(terminal.present(), "terminal 1 kHz marker survives after sample 8800 of captured PCM");
        for (qsizetype index = 1; index < packetTimes.size(); ++index) {
            require(packetTimes[index] - packetTimes[index - 1] >= 5, "backend pacer never emits a catch-up burst");
        }
    }
}
}
int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("kenwood-generated-completion"));
    require(profile.isValid(), "isolated settings");
    QCoreApplication application(argc, argv);
    AppSettings::instance().load();
    run(false);
    run(true);
    std::cout << "Kenwood production finite completion integration passed\n";
}
