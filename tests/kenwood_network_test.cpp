#include "core/backends/kenwood/KenwoodBackend.h"
#include "core/backends/TransmitDelta.h"
#include "core/AudioEngine.h"
#include "core/AppSettings.h"
#include "core/ClientEq.h"
#include "core/ClientGate.h"
#include "core/ClientComp.h"
#include "core/ClientTube.h"
#include "core/ClientPudu.h"
#include "models/RadioModel.h"
#ifdef HAVE_WEBSOCKETS
#include "core/TciServer.h"
#include "core/TciIoWorker.h"
#include "core/TciClient.h"
#include "core/TciProtocol.h"
#include "core/TciAudioHeader.h"
#include "models/DaxReceiveModel.h"
#endif

#include <QCoreApplication>
#include <QBuffer>
#include <QScopeGuard>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
#include <QPair>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QUdpSocket>
#include <QtEndian>
#include <QtMath>

#include <cstdlib>
#include <cmath>
#include <cstring>
#include <memory>
#include <optional>
#include <functional>
#include <iostream>

namespace AetherSDR {
// Use the same memory-sink seam as audio_engine_rates_test. The production
// receive queue, DSP, resampling and sink writes run without QAudioSink.
class AudioEngineRatesTestAccess {
public:
    static void attach(AudioEngine& engine, QBuffer& output, int rate)
    {
        engine.m_rxTimer->stop();
        engine.m_audioDevice = &output;
        engine.setRxDeviceRate(rate);
    }
    static void detach(AudioEngine& engine) { engine.m_audioDevice = nullptr; }
    static bool memorySink(const AudioEngine& engine, const QBuffer& output) { return engine.m_audioDevice == &output; }
    static void drain(AudioEngine& engine, qsizetype bytes) { engine.drainRxAudio(bytes); }
};
#ifdef HAVE_WEBSOCKETS
class TciRxAudioTest {
public:
    using Packets = QHash<TciClient*, QList<QByteArray>>;
    static void capture(TciServer& server, Packets& packets)
    {
        for (float& gain : server.m_io->m_rxChannelGain) { gain = 1.0f; }
        server.m_io->m_rxSend = [&server, &packets](quint64 id, const QByteArray& packet) {
            packets[server.clientById(id)].append(packet);
            return packet.size();
        };
        server.m_io->m_rxBacklog = [](quint64) { return qint64{0}; };
    }
    static TciClient* subscribe(TciServer& server, RadioModel& model, int rate)
    {
        auto client = std::make_unique<TciClient>(&server);
        auto protocol = std::make_unique<TciProtocol>(&model, &server.m_routingState, &server.m_trxMap);
        TciServer::ClientState state;
        state.socket = client.get();
        state.protocol = protocol.get();
        server.m_clients.append(state);
        TciClient* socket = client.release();
        protocol.release(); // TciServer::stopIo owns the registered protocol.
        QObject::connect(socket, &TciClient::textMessageReceived, &server, &TciServer::onTextMessage);
        emit socket->textMessageReceived(QStringLiteral("audio_samplerate:%1;audio_stream_channels:2;audio_stream_sample_type:3;audio_start:0;").arg(rate));
        if (TciServer::ClientState* registered = server.clientStateFor(socket)) { server.syncClient(*registered); }
        return socket;
    }
};
#endif
}

namespace {
constexpr int kPacketsPerSession = 32;
void require(bool condition, const char* description)
{
    if (!condition) {
        std::cerr << description << '\n';
        std::exit(1);
    }
}

bool awaitCondition(const std::function<bool()>& condition)
{
    QElapsedTimer deadline;
    deadline.start();
    while (!condition() && deadline.elapsed() < 4000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    return condition();
}
}

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    QTemporaryDir settings;
    require(settings.isValid(), "isolated settings directory");
    qputenv("AETHER_SETTINGS_DIR", settings.path().toUtf8());
    AetherSDR::AppSettings::instance().load();

    // Explicit opt-in diagnostic only. The peer models positive transport
    // exchanges; it is not evidence of Kenwood firmware interoperability.
    // The backend requires UDP 60001/60002; skip immediately if either is busy.
    QUdpSocket audioReservation;
    QUdpSocket txReservation;
    if (!audioReservation.bind(QHostAddress::LocalHost, 60001, QAbstractSocket::DontShareAddress)
        || !txReservation.bind(QHostAddress::LocalHost, 60002, QAbstractSocket::DontShareAddress)) {
        std::cerr << "SKIP: loopback UDP 60001 or 60002 is already in use\n";
        return 77;
    }
    audioReservation.close();
    txReservation.close();
    QTcpServer peer;
    QUdpSocket audioSender;
    if (!peer.listen(QHostAddress::LocalHost, 0)
        || !audioSender.bind(QHostAddress::LocalHost, 0)) {
        std::cerr << "SKIP: cannot bind loopback diagnostic endpoints\n";
        return 77;
    }

    int logins = 0;
    int receiveAudioRequests = 0;
    QPointer<QTcpSocket> active;
    QObject::connect(&peer, &QTcpServer::newConnection, &application, [&] {
        QTcpSocket* socket = peer.nextPendingConnection();
        require(socket != nullptr, "accepted loopback control socket");
        active = socket;
        QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        QObject::connect(socket, &QTcpSocket::readyRead, socket,
            [&, socket, pending = QByteArray{}]() mutable {
                pending.append(socket->readAll());
                for (qsizetype delimiter = pending.indexOf(';'); delimiter >= 0;
                     delimiter = pending.indexOf(';')) {
                    const QByteArray command = pending.first(delimiter);
                    pending.remove(0, delimiter + 1);
                    if (command == "##CN") {
                        socket->write("##C");
                        QTimer::singleShot(1, socket, [socket] { socket->write("N1;"); });
                    } else if (command == "##ID10406usersecret" || command == "##ID00406usersecret") {
                        ++logins;
                        socket->write("##ID1;##TI0;##UE1;");
                    } else if (command == "ID") {
                        socket->write("ID024;");
                    } else if (command == "AI2") {
                        socket->write("RX;");
                    } else if (command == "XV") {
                        socket->write("XV0;");
                    } else if (command == "XT") {
                        socket->write("XT0;");
                    } else if (command == "AN") {
                        socket->write("AN1000;");
                    } else if (command == "LP1505") {
                        socket->write("LP1505015;");
                    } else if (command == "LP1511") {
                        socket->write("LP1511025;");
                    } else if (command == "MS0") {
                        socket->write("MS010;");
                    } else if (command == "MS1") {
                        socket->write("MS103;");
                    } else if (command == "##KN30") {
                        socket->write("##KN30050;");
                    } else if (command == "##KN31") {
                        socket->write("##KN31060;");
                    } else if (command == "FA") {
                        socket->write(logins == 2
                            ? "FA00070100000;FB00007074000;FR0;FT0;OM02;"
                            : "FA00014074000;FB00007074000;FR0;FT0;OM02;");
                    } else if (command == "EX00505") {
                        socket->write("EX00505 001;");
                    } else if (command == "EX00102") {
                        socket->write("EX00102 001;");
                    } else if (command == "EX00606") {
                        socket->write("EX00606 001;");
                    } else if (command == "EX00607") {
                        socket->write("EX00607 005;");
                    } else if (command == "EX00608") {
                        socket->write("EX00608 000;");
                    } else if (command == "EX00609") {
                        socket->write("EX00609 007;");
                    } else if (command == "SF0") {
                        socket->write(logins == 2 ? "SF0000701000002;" : "SF0000140740002;");
                    } else if (command == "SF1") {
                        socket->write("SF1000070740003;");
                    } else if (command == "BS3") {
                        socket->write("BS30;BSK4;BSO0;BS42;");
                    } else if (command == "DD01") {
                        socket->write("##DD2" + QByteArray("46").repeated(640) + ';');
                    } else if (command == "##VP1") {
                        ++receiveAudioRequests;
                    } else if (command == "PS") {
                        socket->write("PS1;");
                    }
                }
            });
    });

    AetherSDR::RadioModel radio;
    std::unique_ptr<AetherSDR::kenwood::KenwoodBackend> network = std::make_unique<AetherSDR::kenwood::KenwoodBackend>();
    AetherSDR::kenwood::KenwoodBackend& backend = *network;
    radio.setBackendForTest(std::move(network), QStringLiteral("kenwood"));
    QBuffer speakerOutput;
    require(speakerOutput.open(QIODevice::ReadWrite), "memory speaker sink opens");
    AetherSDR::AudioEngine playback;
    const auto detachSpeaker = qScopeGuard([&] { AetherSDR::AudioEngineRatesTestAccess::detach(playback); });
    playback.setMuted(false);
    playback.setRxBoost(false);
    playback.setRxOutputTrimDb(0.0f);
    playback.setRxBufferCapMs(1000);
    playback.clientEqRx()->setEnabled(false);
    playback.clientGateRx()->setEnabled(false);
    playback.clientCompRx()->setEnabled(false);
    playback.clientTubeRx()->setEnabled(false);
    playback.clientPuduRx()->setEnabled(false);
    qputenv("AETHER_AUTOMATION", "1");
    qunsetenv("AETHER_AUTOMATION_ALLOW_TX");
    QObject::connect(&radio, &AetherSDR::RadioModel::backendAudioFrameReady,
        &playback, &AetherSDR::AudioEngine::feedPcmFrame);
    int speakerFrames = 0;
    int demodFrames = 0;
    int sliceFrames = 0;
    std::optional<AetherSDR::PcmFrame> speakerLease;
    QObject::connect(&radio, &AetherSDR::RadioModel::backendAudioFrameReady, &application,
        [&](const AetherSDR::PcmFrame& frame) { ++speakerFrames; speakerLease = frame; });
    QObject::connect(&radio, &AetherSDR::RadioModel::rxDemodAudioReady, &application,
        [&](const AetherSDR::PcmFrame&) { ++demodFrames; });
    QObject::connect(&radio, &AetherSDR::RadioModel::backendSliceAudioFrameReady, &application,
        [&](int id, const AetherSDR::PcmFrame& frame) {
            require(id == 0 && frame.stream().sliceId == 0 && frame.current(), "native slice PCM retains physical slice-zero identity");
            ++sliceFrames;
        });
#ifdef HAVE_WEBSOCKETS
    AetherSDR::TciRxAudioTest::Packets tciPackets;
    AetherSDR::TciServer routing(&radio);
    AetherSDR::TciRxAudioTest::capture(routing, tciPackets);
    const QList<QPair<AetherSDR::TciClient*, int>> tciClients{
        {AetherSDR::TciRxAudioTest::subscribe(routing, radio, 24000), 24000},
        {AetherSDR::TciRxAudioTest::subscribe(routing, radio, 48000), 48000}};
    AetherSDR::DaxReceiveModel virtualAudio(radio, routing);
    virtualAudio.setEnabled(true);
    int virtualFrames = 0;
    QByteArray virtualPcm;
    QObject::connect(&virtualAudio, &AetherSDR::DaxReceiveModel::audioReady, &application,
        [&](int channel, const QByteArray& pcm) {
            require(channel == 1, "one native physical receiver maps to virtual audio channel one");
            ++virtualFrames;
            virtualPcm = pcm;
        });
#endif
    std::optional<bool> nativeSidetone;
    QVariantMap inputs;
    std::optional<int> nativeTunePower;
    std::optional<bool> nativeLowerBfo;
    QObject::connect(&backend, &AetherSDR::IRadioBackend::transmitChanged, &application,
        [&](const AetherSDR::TransmitDelta& delta) {
            if (delta.cwSidetone) { nativeSidetone = *delta.cwSidetone; }
            if (delta.tunePower) { nativeTunePower = *delta.tunePower; }
            if (delta.cwlEnabled) { nativeLowerBfo = *delta.cwlEnabled; }
        });
    int connected = 0;
    int disconnected = 0;
    int audioFrames = 0;
    int spectrumFrames = 0;
    double frequency = 0;
    QString receiveMode;
    QString error;
    QVariantMap audioLevels;
    QVariantMap vfos;
    QObject::connect(&backend, &AetherSDR::IRadioBackend::extensionStatus, &application,
        [&](const QString&, const QString& kind, const QVariantMap& state) {
            if (kind == "inputs") { inputs = state; }
            if (kind == "audio.levels") { audioLevels = state; }
            if (kind == "vfo") { vfos = state; }
        });
    QObject::connect(&backend, &AetherSDR::IRadioBackend::connected, &application, [&] { ++connected; });
    QObject::connect(&backend, &AetherSDR::IRadioBackend::disconnected, &application, [&] { ++disconnected; });
    QObject::connect(&backend, &AetherSDR::IRadioBackend::connectionError, &application,
        [&](const QString& reason) { error = reason; });
    QObject::connect(&backend, &AetherSDR::IRadioBackend::sliceChanged, &application,
        [&](int id, const AetherSDR::SliceDelta& delta) {
            require(id == 0, "one physical receiver on the socket path");
            if (delta.frequency) { frequency = *delta.frequency; }
            if (delta.mode) { receiveMode = *delta.mode; }
        });
    QObject::connect(&backend, &AetherSDR::IRadioBackend::audioFrameReady, &application,
        [&](const AetherSDR::PcmFrame& frame) {
            require(frame.current() && frame.stream().format.sampleRateHz == 24000,
                "UDP audio publishes current native playback PCM");
            ++audioFrames;
        });
    QObject::connect(&backend, &AetherSDR::IRadioBackend::spectrumFrameReady, &application,
        [&](int, const QByteArray& frame, const AetherSDR::SpectrumCoverage&) {
            require(frame.size() == 640 * sizeof(float), "TCP scope publishes bounded bins");
            ++spectrumFrames;
        });

    AetherSDR::RadioConnectRequest request;
    request.host = QStringLiteral("localhost");
    request.port = peer.serverPort();
    request.localBindAddress = QHostAddress::LocalHost;
    request.params = {{QStringLiteral("kenwood.username"), QStringLiteral("user")},
        {QStringLiteral("kenwood.password"), QStringLiteral("secret")},
        {QStringLiteral("kenwood.administrator"), false}};
    for (int session = 1; session <= 3; ++session) {
        request.params.insert(QStringLiteral("kenwood.administrator"), session == 2);
        request.params.insert(QStringLiteral("kenwood.variant"),
            session == 2 ? QStringLiteral("e") : session == 3 ? QStringLiteral("k") : QString());
        const double expectedFrequency = session == 2 ? 70.1 : 14.074;
        nativeSidetone.reset();
        nativeLowerBfo.reset();
        nativeTunePower.reset();
        audioLevels.clear();
#ifdef HAVE_WEBSOCKETS
        tciPackets.clear();
#endif
        inputs.clear();
        vfos.clear();
        receiveMode.clear();
        backend.connectRadio(request);
        require(awaitCondition([&] {
            return !error.isEmpty() || (logins == session && receiveAudioRequests == session
                && spectrumFrames == session && frequency == expectedFrequency && receiveMode == "USB"
                && backend.capabilities().hasTxFilterControls
                && backend.capabilities().radioTransmitSettings->fields.contains(AetherSDR::TransmitControlRequest::Field::CwSidetone)
                && nativeSidetone == false && nativeLowerBfo == true
                && nativeTunePower == (session == 2 ? 25 : 15)
                && backend.capabilities().radioTransmitSettings->fields.contains(AetherSDR::TransmitControlRequest::Field::TunePower)
                && backend.capabilities().radioTransmitSettings->fields.contains(AetherSDR::TransmitControlRequest::Field::CwlEnabled)
                && vfos.value("modeA").toString() == "USB" && vfos.value("modeB").toString() == "CW"
                && inputs.contains("send") && inputs.value("send").toInt() == 0
                && inputs.value("data").toInt() == 3
                && audioLevels.value("inputLevel").toInt() == 50 && audioLevels.value("outputLevel").toInt() == 60
                && (session != 2 || audioLevels.value("writable").toBool()));
        }), "production TCP login/state/scope converges");
        if (!error.isEmpty()) { std::cerr << error.toStdString() << '\n'; }
        require(error.isEmpty() && connected == session, "successful authenticated socket session");
        require(backend.capabilities().sliceFrequencyControl.maximumHz
            == (session == 2 ? 74800000 : 60000000), "regional tuning limit follows each connection");
        active->write("FR1;");
        require(awaitCondition([&] { return frequency == 7.074 && receiveMode == "CW"; }),
            "socket receive selection follows physical VFO B frequency and mode");
        active->write("FR0;");
        require(awaitCondition([&] { return frequency == expectedFrequency && receiveMode == "USB"; }),
            "socket receive selection restores physical VFO A without using display mode");
        const int deviceRate = session == 1 ? 24000 : session == 2 ? 44100 : 48000;
        speakerOutput.buffer().clear();
        speakerOutput.seek(0);
        AetherSDR::AudioEngineRatesTestAccess::attach(playback, speakerOutput, deviceRate);
        require(playback.startAutomationAudioCapture(5000, {QStringLiteral("raw"), QStringLiteral("post"),
                QStringLiteral("output"), QStringLiteral("final")}).value("ok").toBool(),
            "playback capture includes DSP and memory-sink output without a physical audio endpoint");
        QByteArray audio(652, '\0');
        for (int sample = 0; sample < 320; ++sample) { qToLittleEndian<qint16>(8192, audio.data() + 12 + sample * 2); }
        audio[0] = static_cast<char>(0x80);
        audio[1] = 96;
        for (int packet = 1; packet <= kPacketsPerSession; ++packet) {
            qToBigEndian<quint16>(packet, audio.data() + 2);
            qToBigEndian<quint32>((packet - 1) * 320, audio.data() + 4);
            require(audioSender.writeDatagram(audio, QHostAddress::LocalHost, 60001) == audio.size(),
                "loopback peer sends bounded RX packets");
        }
        const int expectedFrames = session * kPacketsPerSession;
        require(awaitCondition([&] { return audioFrames == expectedFrames; }), "production UDP path reaches playback");
        require(awaitCondition([&] { return speakerFrames == expectedFrames && demodFrames == expectedFrames && sliceFrames == expectedFrames; }),
            "network PCM traverses the production model speaker, demod and slice routes once per packet");
        for (int tick = 0; tick < 32; ++tick) {
            AetherSDR::AudioEngineRatesTestAccess::drain(playback, deviceRate * 2 * sizeof(float));
        }
        playback.stopAutomationAudioCapture();
        const QJsonArray captured = playback.automationAudioCaptureSnapshot(true).value("chunks").toArray();
        QJsonArray chunks;
        QByteArray postDsp;
        QByteArray outputPcm;
        QByteArray finalPcm;
        for (const QJsonValue& value : captured) {
            const QJsonObject chunk = value.toObject();
            const QString point = chunk.value("point").toString();
            if (point == QLatin1String("raw")) { chunks.append(chunk); continue; }
            require(chunk.value("sampleRate").toInt() == deviceRate && chunk.value("channels").toInt() == 2,
                "processed playback chunks retain the negotiated device-rate stereo domain");
            const QByteArray pcm = QByteArray::fromBase64(chunk.value("pcmBase64").toString().toLatin1());
            if (point == QLatin1String("post")) { postDsp += pcm; }
            else if (point == QLatin1String("output")) { outputPcm += pcm; }
            else if (point == QLatin1String("final")) { finalPcm += pcm; }
        }
        require(chunks.size() == kPacketsPerSession && chunks[0].toObject().value("sampleRate").toInt() == 24000
            && chunks[0].toObject().value("channels").toInt() == 2,
            "playback ingress accepts resampled native stereo at 24 kHz on every session");
        int outputFrames = 0;
        for (const QJsonValue& value : chunks) {
            const QJsonObject chunk = value.toObject();
            require(chunk.value("sampleRate").toInt() == 24000 && chunk.value("channels").toInt() == 2,
                "every playback ingress chunk retains the native sample rate and layout");
            outputFrames += chunk.value("frames").toInt();
        }
        require(std::abs(outputFrames - kPacketsPerSession * 480) <= 2,
            "network 16-to-24 kHz conversion preserves the expected stream duration");
        const QByteArray raw = QByteArray::fromBase64(chunks.last().toObject().value("pcmBase64").toString().toLatin1());
        const auto verifyPcm = [](const QByteArray& pcm, const char* route) {
            require(pcm.size() >= 64 * 2 * qsizetype(sizeof(float)) && pcm.size() % (2 * sizeof(float)) == 0,
                "native stereo PCM has bounded aligned frames");
            const qsizetype frames = pcm.size() / (2 * sizeof(float));
            for (qsizetype frame = 32; frame < frames; ++frame) {
                float stereo[2];
                std::memcpy(stereo, pcm.constData() + frame * 2 * sizeof(float), sizeof(stereo));
                if (!std::isfinite(stereo[0]) || std::abs(stereo[0] - 0.25f) >= 0.03f
                    || std::abs(stereo[0] - stereo[1]) >= 1e-6f) {
                    std::cerr << route << " sample " << frame << ": " << stereo[0] << ", " << stereo[1] << '\n';
                }
                require(std::isfinite(stereo[0]) && std::abs(stereo[0] - 0.25f) < 0.03f
                    && std::abs(stereo[0] - stereo[1]) < 1e-6f,
                    "signed little-endian network samples preserve amplitude and stereo duplication through resampling");
            }
        };
        verifyPcm(raw, "playback ingress");
        const qsizetype deviceFrames = speakerOutput.data().size() / (2 * sizeof(float));
        const qint64 expectedDeviceFrames = qRound64(outputFrames * deviceRate / 24000.0);
        require(std::abs(deviceFrames - expectedDeviceFrames) <= 2,
            "production DSP/sink conversion preserves native stream duration at 24, 44.1 and 48 kHz");
        require(!postDsp.isEmpty() && outputPcm == speakerOutput.data() && finalPcm == speakerOutput.data(),
            "post-DSP output reaches the memory speaker exactly once and matches final capture");
        verifyPcm(postDsp.right(512 * 2 * sizeof(float)), "post DSP");
        verifyPcm(finalPcm.right(512 * 2 * sizeof(float)), "speaker output");
#ifdef HAVE_WEBSOCKETS
        require(awaitCondition([&] { return virtualFrames == expectedFrames; }),
            "native network PCM reaches the production TCI binding and virtual-audio converter");
        verifyPcm(virtualPcm, "virtual audio");
        for (const auto& client : tciClients) {
            require(awaitCondition([&] { return !tciPackets.value(client.first).isEmpty(); }),
                "native network PCM reaches both negotiated TCI binary streams");
            QByteArray tciPcm;
            for (const QByteArray& packet : tciPackets.value(client.first)) {
                AetherSDR::TciAudioHeader header{};
                require(packet.size() >= qsizetype(sizeof(header)), "TCI receive packet contains its full header");
                std::memcpy(&header, packet.constData(), sizeof(header));
                bool reservedZero = true;
                for (quint32 value : header.reserved) { reservedZero = reservedZero && value == 0; }
                require(header.receiver == 0 && header.sampleRate == quint32(client.second) && header.format == 3
                    && header.codec == 0 && header.crc == 0 && header.type == 1 && header.channels == 2 && reservedZero
                    && header.length > 0 && header.length <= 2048 && header.length % 2 == 0
                    && packet.size() == qsizetype(sizeof(header) + header.length * sizeof(float)),
                    "TCI binary packets retain negotiated rate, receiver, float stereo and bounded scalar counts");
                tciPcm += packet.sliced(sizeof(header));
            }
            require(tciPcm.size() <= qRound64(outputFrames * client.second / 24000.0) * 2 * qsizetype(sizeof(float))
                && tciPcm.size() >= outputFrames * client.second / 24000 * qsizetype(sizeof(float)),
                "TCI streaming output stays bounded by the source duration with resampler/packetization latency");
            verifyPcm(tciPcm.right(512 * 2 * sizeof(float)), "TCI binary audio");
        }
        require(!routing.isRunning(), "virtual receive adapter needs no externally listening TCI server");
#endif
        require(AetherSDR::AudioEngineRatesTestAccess::memorySink(playback, speakerOutput)
            && !playback.isRxStreaming() && !playback.isTxStreaming(),
            "network receive diagnostic uses only its memory sink, with no physical audio input or output");
        backend.disconnectRadio();
        require(speakerLease && !speakerLease->current(), "disconnect retires PCM held by model and playback consumers");
        require(disconnected == session, "disconnect retires each transport session");
        require(awaitCondition([&] { return !active; }), "peer observes client socket closure");
    }
    return 0;
}
