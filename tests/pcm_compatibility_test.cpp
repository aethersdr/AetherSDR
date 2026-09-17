// Socket-free production routing: backend publisher -> RadioModel -> audio
// ingress, plus Flex parser/DAX lifetime. No peers, devices or radio connect.
#include "TestSettingsProfile.h"
#include "core/AudioEngine.h"
#include "core/PanadapterStream.h"
#include "core/backends/IRadioBackend.h"
#include "core/backends/hl2/Hl2Backend.h"
#include "core/backends/hl2/Hl2RxDsp.h"
#include "models/RadioModel.h"

#include <QCoreApplication>
#include <QEvent>
#include <QJsonArray>
#include <QtEndian>
#include <cstdio>

namespace AetherSDR {
class PcmCompatibilityTestAccess {
public:
    static void narrow(PanadapterStream& stream, const QByteArray& packet)
    {
        stream.decodeNarrowAudio(reinterpret_cast<const uchar*>(packet.constData()),
                                 static_cast<int>(packet.size()), false, 0x123);
    }
    static void reduced(PanadapterStream& stream, const QByteArray& packet)
    {
        stream.decodeReducedBwAudio(reinterpret_cast<const uchar*>(packet.constData()),
                                     static_cast<int>(packet.size()), false, 0x123);
    }
    static void dax(PanadapterStream& stream, quint32 id, int channel, const QByteArray& pcm)
    {
        stream.publishLegacyDaxAudio(id, channel, pcm);
    }
};
} // namespace AetherSDR

namespace AetherSDR::hl2 {
struct Hl2PcmTestAccess {
    static void start(Hl2Backend& backend, Hl2RxDsp& first, Hl2RxDsp& second)
    {
        backend.m_rx.resize(2);
        backend.m_rx[0].dsp = &first;
        backend.m_rx[1].dsp = &second;
        backend.m_rx[1].audioMuted = true;
        backend.m_connected = true;
        emit backend.connected();
    }
    static void mix(Hl2Backend& backend, int index, const std::vector<float>& pcm)
    {
        backend.mixReceiverAudio(index, pcm);
    }
    static void unmuteSecond(Hl2Backend& backend)
    {
        backend.m_rx[1].audioMuted = false;
    }
    static void finish(Hl2Backend& backend)
    {
        // Stack-owned, unconfigured DSP objects: never connect a transport or
        // transfer their ownership to the backend's teardown path.
        backend.m_rx.clear();
        backend.m_connected = false;
        backend.retirePcmStreams();
    }
};
} // namespace AetherSDR::hl2

using namespace AetherSDR;

namespace {
int failures = 0;
int checks = 0;
void check(bool pass, const char* name)
{
    ++checks;
    if (!pass) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", name);
    }
}
QByteArray bytes(const QVector<float>& samples)
{
    return {reinterpret_cast<const char*>(samples.constData()),
            samples.size() * static_cast<qsizetype>(sizeof(float))};
}

class InjectedBackend final : public IRadioBackend {
public:
    RadioCapabilities capabilities() const override { return {}; }
    bool ownsRxAudio() const override { return true; }
    void connectRadio(const RadioConnectRequest&) override
    {
        m_connected = true;
        emit connected();
    }
    void disconnectRadio() override
    {
        m_connected = false;
        emit disconnected();
    }
    bool isConnected() const override { return m_connected; }
    void setSliceFrequency(int, double) override {}
    void setSliceMode(int, const QString&) override {}
    void setSliceFilter(int, int, int) override {}
    void setSliceAgc(int, const QString&, int) override {}
    void setPanCenter(const QString&, double, PanCenterIntent) override {}
    void setKeying(bool, const TxCoordinator::Operation&,
                   const TxCoordinator::Completion&) override {} // deliberately no transport
    void invokeExtension(const QString&, const QString&, quint64, const QVariant&) override {}
    void speaker(const QByteArray& pcm) { publishLegacyAudio(pcm); }
    void sliceAudio(int id, const QByteArray& pcm) { publishLegacySliceAudio(id, pcm); }
private:
    bool m_connected = false;
};

void backendAndAudioRouting()
{
    RadioModel model;
    AudioEngine audio;
    auto backend = std::make_unique<InjectedBackend>();
    InjectedBackend* source = backend.get();
    model.setBackendForTest(std::move(backend), QStringLiteral("rtl"));
    source->connectRadio({});
    int taps = 0;
    int slices = 0;
    PcmFrame oldSpeaker;
    PcmFrame oldSlice;
    const QByteArray expected = bytes({0.25f, -0.25f, 0.5f, -0.5f});
    QObject::connect(&model, &RadioModel::backendAudioFrameReady, &audio,
                     &AudioEngine::feedPcmFrame, Qt::QueuedConnection);
    QObject::connect(&model, &RadioModel::rxDemodAudioReady, &model,
                     [&](const PcmFrame& frame) {
        ++taps;
        oldSpeaker = frame;
        check(frame.legacyStereo24() == expected, "model demod tap is byte exact");
    });
    QObject::connect(&model, &RadioModel::backendSliceAudioFrameReady, &model,
                     [&](int id, const PcmFrame& frame) {
        ++slices;
        oldSlice = frame;
        check(id == 3 && frame.stream().sliceId == 3, "sparse slice ID preserved");
        check(frame.legacyStereo24() == expected, "pre-monitor slice samples preserved");
    });
    check(audio.startAutomationAudioCapture(5000, {QStringLiteral("raw")}).value("ok").toBool(),
          "automation capture started for production ingress checks");
    source->speaker(expected);
    source->sliceAudio(3, expected);
    QCoreApplication::sendPostedEvents(&audio, QEvent::MetaCall);
    QJsonArray chunks = audio.automationAudioCaptureSnapshot(true).value("chunks").toArray();
    check(taps == 1 && slices == 1 && chunks.size() == 1, "one speaker delivery, separate slice tap");
    if (!chunks.isEmpty()) {
        const QJsonObject chunk = chunks[0].toObject();
        check(chunk.value("sampleRate").toInt() == 24000 && chunk.value("frames").toInt() == 2,
              "AudioEngine retains producer rate and frame count");
        check(QByteArray::fromBase64(chunk.value("pcmBase64").toString().toLatin1()) == expected,
              "typed AudioEngine ingress preserves raw PCM exactly");
    }
    audio.feedPcmFrame(oldSpeaker);
    check(audio.automationAudioCaptureSnapshot(false).value("chunks").toArray().size() == 1,
          "AudioEngine refuses repeated frame");
    source->speaker(QByteArray(7, '\0'));
    source->sliceAudio(3, bytes({std::numeric_limits<float>::infinity(), 0.0f}));
    check(taps == 1 && slices == 1, "malformed backend PCM never enters model buses");

    const quint64 oldInstance = oldSlice.stream().receiverInstance;
    emit source->sliceRemoved(3);
    check(!oldSlice.current(), "slice removal revokes queued per-slice PCM");
    source->sliceAudio(3, expected);
    check(oldSlice.stream().receiverInstance != oldInstance, "reused slice gets distinct receiver instance");
    source->speaker(expected); // queued to AudioEngine, then invalidate before delivery
    source->disconnectRadio();
    source->connectRadio({});
    QCoreApplication::sendPostedEvents(&audio, QEvent::MetaCall);
    check(audio.automationAudioCaptureSnapshot(false).value("chunks").toArray().size() == 1,
          "queued previous-session speaker frame rejected after reconnect");
    source->speaker(expected);
    QCoreApplication::sendPostedEvents(&audio, QEvent::MetaCall);
    check(audio.automationAudioCaptureSnapshot(false).value("chunks").toArray().size() == 2,
          "new session speaker still delivered once");

    PcmProducer future;
    future.start(PcmPurpose::Speaker, -1, {48000, PcmLayout::Stereo});
    audio.feedPcmFrame(*future.produce({0.2f, -0.2f}));
    check(audio.automationAudioCaptureSnapshot(false).value("chunks").toArray().size() == 2,
          "AudioEngine refuses a competing 48 kHz producer while its speaker route is owned");
    source->retirePcmStreams();
    const int retiredTaps = taps;
    const int retiredSlices = slices;
    check(!oldSpeaker.current() && !oldSlice.current(), "retirement revokes both streams before disconnect notification");
    source->speaker(expected);
    source->sliceAudio(3, expected);
    check(taps == retiredTaps && slices == retiredSlices, "queued worker cannot mint PCM during retirement");
    source->connectRadio({});
    source->speaker(expected);
    const PcmFrame priorFamily = oldSpeaker;
    auto replacement = std::make_unique<InjectedBackend>();
    source = replacement.get();
    model.setBackendForTest(std::move(replacement), QStringLiteral("hl2"));
    check(!priorFamily.current(), "family switch revokes old backend PCM");
    source->connectRadio({});
    const int before = taps;
    source->speaker(expected);
    check(taps == before + 1, "shared production test binding has one relay after family swap");
    QCoreApplication::sendPostedEvents(&audio, QEvent::MetaCall);
    const int beforeKiwi = audio.automationAudioCaptureSnapshot(false).value("chunkCount").toInt();
    PcmProducer kiwi;
    kiwi.start(PcmPurpose::Auxiliary);
    const PcmFrame kiwiFrame = *kiwi.legacyStereo24(expected);
    const QString kiwiId = QStringLiteral("pcm-test-kiwi");
    audio.setKiwiSdrAudioSourceEnabled(kiwiId, true);
    audio.feedKiwiPcmFrame(kiwiId, kiwiFrame);
    const QJsonArray combined = audio.automationAudioCaptureSnapshot(true).value("chunks").toArray();
    check(combined.size() == beforeKiwi + 1, "concurrent Kiwi24 has an independent ingress cursor");
    if (combined.size() == beforeKiwi + 1) {
        const QJsonObject chunk = combined.last().toObject();
        check(chunk.value("source").toString() == "kiwi"
              && chunk.value("sourceId").toString() == kiwiId
              && chunk.value("sampleRate").toInt() == 24000
              && QByteArray::fromBase64(chunk.value("pcmBase64").toString().toLatin1()) == expected,
              "Kiwi24 retains source attribution, sample rate and stereo bytes");
    }
    audio.feedKiwiPcmFrame(kiwiId, kiwiFrame);
    kiwi.invalidate();
    audio.feedKiwiPcmFrame(kiwiId, kiwiFrame);
    check(audio.automationAudioCaptureSnapshot(false).value("chunkCount").toInt() == beforeKiwi + 1,
          "Kiwi ingress rejects duplicate and retired frames");
    audio.setKiwiSdrAudioSourceEnabled(kiwiId, false);
    audio.stopAutomationAudioCapture();
}

void hermesCompatibility()
{
    hl2::Hl2Backend backend;
    hl2::Hl2RxDsp first;
    hl2::Hl2RxDsp second;
    hl2::Hl2PcmTestAccess::start(backend, first, second);
    PcmFrame last;
    int deliveries = 0;
    QObject::connect(&backend, &IRadioBackend::audioFrameReady, &backend,
                     [&](const PcmFrame& frame) { last = frame; ++deliveries; });
    const std::vector<float> one{1.25f, -1.5f, 0.25f, -0.0f};
    hl2::Hl2PcmTestAccess::mix(backend, 0, one);
    check(deliveries == 1 && last.legacyStereo24() == bytes({1.25f, -1.5f, 0.25f, -0.0f}),
          "Hermes single-receiver mixer stays byte exact without clipping");
    hl2::Hl2PcmTestAccess::unmuteSecond(backend);
    hl2::Hl2PcmTestAccess::mix(backend, 0, {0.25f, -0.5f, 0.125f, -0.125f});
    check(deliveries == 1, "Hermes still waits for the second receiver's matching samples");
    hl2::Hl2PcmTestAccess::mix(backend, 1, {0.125f, -0.25f, 0.25f, -0.25f});
    check(deliveries == 2 && last.legacyStereo24() == bytes({0.375f, -0.75f, 0.375f, -0.375f}),
          "Hermes stereo sum and frame alignment unchanged by typed publication");
    hl2::Hl2PcmTestAccess::finish(backend);
    check(!last.current(), "Hermes compatibility frames revoked at retirement");
}

void flexCompatibility()
{
    PanadapterStream stream; // no init(), sockets, bind or peer
    PcmFrame last;
    int speaker = 0;
    QObject::connect(&stream, &PanadapterStream::pcmFrameReady, &stream,
                     [&](const PcmFrame& frame) { last = frame; ++speaker; });
    const QVector<float> samples{0.25f, -0.5f, 1.125f, -0.0f};
    QByteArray packet(PanadapterStream::VITA49_HEADER_BYTES, '\0');
    for (float value : samples) {
        quint32 bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        const quint32 wire = qToBigEndian(bits);
        packet.append(reinterpret_cast<const char*>(&wire), sizeof(wire));
    }
    PcmCompatibilityTestAccess::narrow(stream, packet);
    check(speaker == 1, "Flex publishes one typed frame per decoded packet");
    check(last.legacyStereo24() == bytes(samples), "Flex LR float decode remains byte exact");
    PcmCompatibilityTestAccess::narrow(stream, packet.left(packet.size() - 4));
    check(speaker == 1, "malformed Flex stereo alignment rejected");
    QByteArray invalid = packet;
    const quint32 nan = qToBigEndian(quint32{0x7fc00000});
    std::memcpy(invalid.data() + PanadapterStream::VITA49_HEADER_BYTES, &nan, 4);
    PcmCompatibilityTestAccess::narrow(stream, invalid);
    check(speaker == 1, "nonfinite Flex PCM rejected before concealment history");
    PcmCompatibilityTestAccess::narrow(stream, packet);
    check(last.legacyStereo24() == bytes(samples), "malformed packet cannot poison next Flex frame");
    QByteArray reduced(PanadapterStream::VITA49_HEADER_BYTES, '\0');
    for (qint16 value : {qint16{8192}, qint16{-16384}}) {
        const qint16 wire = qToBigEndian(value);
        reduced.append(reinterpret_cast<const char*>(&wire), sizeof(wire));
    }
    PcmCompatibilityTestAccess::reduced(stream, reduced);
    check(last.legacyStereo24() == bytes({0.25f, 0.25f, -0.5f, -0.5f}),
          "Flex reduced-bandwidth mono duplicates to stereo unchanged");
    const int before = speaker;
    PcmCompatibilityTestAccess::reduced(stream, reduced.left(reduced.size() - 1));
    check(speaker == before, "partial reduced-bandwidth sample rejected");

    PcmFrame dax;
    int daxCount = 0;
    QObject::connect(&stream, &PanadapterStream::daxPcmReady, &stream,
                     [&](int channel, const PcmFrame& frame) {
        check(channel == 4, "DAX channel preserved independently of slice IDs");
        dax = frame;
        ++daxCount;
    });
    stream.registerDaxStream(0x101, 4);
    PcmCompatibilityTestAccess::dax(stream, 0x101, 4, bytes(samples));
    check(daxCount == 1 && dax.legacyStereo24() == bytes(samples), "Flex DAX byte exact");
    stream.unregisterDaxStream(0x101);
    check(!dax.current(), "DAX removal revokes queued PCM");
    PcmCompatibilityTestAccess::dax(stream, 0x101, 4, bytes(samples));
    check(daxCount == 1, "removed DAX stream cannot emit more PCM");
    stream.stop();
    check(!last.current(), "Flex stop revokes queued speaker frame");
    PcmCompatibilityTestAccess::narrow(stream, packet);
    check(speaker == before, "stopped Flex PCM adapter refuses publication");
}
} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile settings(QStringLiteral("pcm-compatibility"));
    qputenv("AETHER_AUTOMATION", "1");
    QCoreApplication app(argc, argv);
    check(settings.isValid(), "isolated settings profile");
    backendAndAudioRouting();
    flexCompatibility();
    hermesCompatibility();
    std::printf("PCM compatibility: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
