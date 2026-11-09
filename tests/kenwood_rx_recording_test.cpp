// Injected KNS RTP through the production backend/model/recorder path.
// No socket, audio device or physical radio is opened.
#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/QsoRecorder.h"
#include "core/Resampler.h"
#include "core/backends/kenwood/KenwoodBackend.h"
#include "models/RadioModel.h"

#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QtEndian>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <numbers>
#include <optional>

namespace AetherSDR::kenwood {
struct KenwoodBackendTestAccess {
    static void authenticate(KenwoodBackend& backend)
    {
        backend.m_clock.start();
        backend.m_protocol.start("fixture", "fixture", false, 0);
        backend.m_protocol.receive("##CN1;", 1);
        backend.m_protocol.receive("##ID1;", 2);
        backend.ready();
        backend.adopt("ID024");
    }
    static int delay(const KenwoodBackend& backend)
    { return backend.m_rxResampler->groupDelayInputFrames(); }
    static void audio(KenwoodBackend& backend, const QByteArray& packet, qint64 now)
    { backend.adoptAudio(packet, now); }
};
}
using namespace AetherSDR;
namespace {
void require(bool condition, const char* claim)
{
    if (!condition) {
        std::cerr << claim << '\n';
        std::exit(1);
    }
}
QByteArray packet(int sequence)
{
    // Captured-shaped KNS RX: RTP v2/PT96, 320 signed little-endian mono
    // samples per 20 ms. TX's offset-binary big-endian payload is different.
    QByteArray bytes(652, '\0');
    bytes[0] = char(0x80);
    bytes[1] = 96;
    qToBigEndian<quint16>(sequence, bytes.data() + 2);
    qToBigEndian<quint32>(sequence * 320, bytes.data() + 4);
    qToBigEndian<quint32>(0x12345678, bytes.data() + 8);
    for (int sample = 0; sample < 320; ++sample) {
        const double angle = 2 * std::numbers::pi * 1000 * (sequence * 320 + sample) / 16000;
        qToLittleEndian<qint16>(qint16(8192 * std::sin(angle)), bytes.data() + 12 + sample * 2);
    }
    return bytes;
}
}
int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("kenwood-rx-recording"));
    require(profile.isValid(), "isolated settings profile");
    QCoreApplication application(argc, argv);
    AppSettings::instance().load();
    AppSettings::instance().setValue(QStringLiteral("RecordingMode"), QStringLiteral("Client"));
    // Seam-native recording remains available with PC speaker playback off.
    AppSettings::instance().setValue(QStringLiteral("PcAudioEnabled"), QStringLiteral("False"));
    QTemporaryDir directory;
    require(directory.isValid(), "temporary recording directory");
    RadioModel radio;
    auto owned = std::make_unique<kenwood::KenwoodBackend>(kenwood::KenwoodBackend::Transport{
        [](const QByteArray&) { return true; }, [](const QByteArray&) { return true; }});
    kenwood::KenwoodBackend* backend = owned.get();
    radio.setBackendForTest(std::move(owned), QStringLiteral("kenwood"));
    QsoRecorder recorder;
    recorder.setRecordingDir(directory.path());
    recorder.setAutoRecordEnabled(false);
    recorder.setBackendOwnsRxAudioProvider([backend] { return backend->ownsRxAudio(); });
    QObject::connect(&radio, &RadioModel::rxDemodAudioReady, &recorder, &QsoRecorder::feedRxFrame);
    std::optional<PcmFrame> held;
    qint64 receivedFrames = 0;
    int deliveries = 0;
    QObject::connect(&radio, &RadioModel::rxDemodAudioReady, &application, [&](const PcmFrame& frame) {
        require(frame.current() && frame.stream().purpose == PcmPurpose::Speaker
            && frame.stream().format.sampleRateHz == 24000
            && frame.stream().format.layout == PcmLayout::Stereo,
            "real model demod bus carries normalized current 24 kHz stereo speaker PCM");
        held = frame;
        receivedFrames += frame.frameCount();
        ++deliveries;
    });
    using Access = kenwood::KenwoodBackendTestAccess;
    Access::authenticate(*backend);
    // prewarm() removes no-output startup, not the documented linear-phase
    // acoustic delay. Let live RX settle before beginning the one-second file.
    const int delayFrames = Access::delay(*backend);
    const int prerollPackets = (delayFrames + 319) / 320 + 2;
    for (int sequence = 0; sequence < prerollPackets; ++sequence) {
        Access::audio(*backend, packet(sequence), sequence * 20);
    }
    require(held.has_value(), "KNS RX reaches actual model demod route");
    receivedFrames = 0;
    recorder.startRecording();
    require(recorder.isRecording(), "native backend recording starts with speaker playback disabled");
    const QString path = recorder.recordingFilePath();
    for (int sequence = prerollPackets; sequence < prerollPackets + 50; ++sequence) {
        Access::audio(*backend, packet(sequence), sequence * 20);
    }
    require(deliveries == prerollPackets + 50 && receivedFrames >= 23520 && receivedFrames <= 24480,
        "fifty network packets deliver one second of normalized RX without duplicate routes");
    const qint64 expectedFrames = receivedFrames;
    backend->disconnectRadio();
    require(held && !held->current(), "disconnect retires the actual captured recording source lease");
    recorder.feedRxFrame(*held); // Delayed consumer delivery from the retired session.
    Access::audio(*backend, packet(prerollPackets + 50), (prerollPackets + 50) * 20);
    require(receivedFrames == expectedFrames && deliveries == prerollPackets + 50,
        "retired backend input cannot publish another model frame");
    recorder.stopRecording();
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "open finalized production WAV");
    const QByteArray wav = file.readAll();
    require(wav.size() == 44 + expectedFrames * 4, "WAV contains exactly accepted frames and no retired replay");
    require(wav.first(4) == "RIFF" && wav.mid(8, 8) == "WAVEfmt " && wav.mid(36, 4) == "data",
        "canonical RIFF PCM WAV chunks");
    require(qFromLittleEndian<quint32>(wav.constData() + 24) == 24000
        && qFromLittleEndian<quint32>(wav.constData() + 28) == 96000
        && qFromLittleEndian<quint16>(wav.constData() + 20) == 1
        && qFromLittleEndian<quint16>(wav.constData() + 22) == 2
        && qFromLittleEndian<quint16>(wav.constData() + 34) == 16
        && qFromLittleEndian<quint32>(wav.constData() + 40) == expectedFrames * 4
        && qFromLittleEndian<quint32>(wav.constData() + 4) == wav.size() - 8,
        "WAV format and lengths describe physical 24 kHz PCM16 stereo samples");
    double sine = 0, cosine = 0, energy = 0;
    for (qint64 frame = 0; frame < expectedFrames; ++frame) {
        const qint16 left = qFromLittleEndian<qint16>(wav.constData() + 44 + frame * 4);
        const qint16 right = qFromLittleEndian<qint16>(wav.constData() + 46 + frame * 4);
        require(left == right, "network mono reaches both recording channels identically");
        const double value = left / 32767.0;
        const double angle = 2 * std::numbers::pi * 1000 * frame / 24000;
        sine += value * std::sin(angle);
        cosine += value * std::cos(angle);
        energy += value * value;
    }
    const double amplitude = 2 * std::hypot(sine, cosine) / expectedFrames;
    std::cout << "RX delayInputFrames=" << delayFrames << " prerollPackets=" << prerollPackets
        << " recordedFrames=" << expectedFrames << " amplitude=" << amplitude
        << " meanSquare=" << energy / expectedFrames << std::endl;
    require(amplitude > 0.23 && amplitude < 0.27 && energy / expectedFrames > 0.025,
        "actual WAV preserves the signed 1 kHz RX tone amplitude and time scale");
    std::cout << "Kenwood RX recording: " << expectedFrames << " frames, amplitude=" << amplitude << '\n';
}
