// Socket-free production RX: real RadioModel binding, protocol negotiation,
// routing, conversion and encoding. Only WebSocket writes/backlog are injected.
#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/TciServer.h"
#include "core/TciRxConverter.h"
#include "core/backends/IRadioBackend.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"
#include <QCoreApplication>
#include <QStringList>
#include "core/TciClient.h"
#include <array>
#include <atomic>
#include <thread>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

namespace AetherSDR {
namespace {
int checks = 0;
int failures = 0;
QStringList transportWarnings;
void captureTransportWarning(QtMsgType type, const QMessageLogContext&, const QString& message)
{
    if (type == QtWarningMsg && message.startsWith(QStringLiteral("TCI: RX audio stopped"))) {
        transportWarnings.append(message);
    }
}
void check(bool ok, const char* name)
{
    ++checks;
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", name); }
}
quint32 word(const QByteArray& packet, int index)
{
    quint32 value = 0;
    if (packet.size() >= (index + 1) * 4) {
        std::memcpy(&value, packet.constData() + index * 4, 4);
    }
    return value;
}
QVector<float> floats(const QList<QByteArray>& packets)
{
    QVector<float> result;
    for (const QByteArray& packet : packets) {
        for (qsizetype offset = 64; offset < packet.size(); offset += 4) {
            float sample;
            std::memcpy(&sample, packet.constData() + offset, 4);
            result.append(sample);
        }
    }
    return result;
}
QVector<float> tone(int count, int rate, bool antiphase = false, double leftHz = 1000)
{
    QVector<float> samples(count * 2);
    for (int i = 0; i < count; ++i) {
        samples[2*i] = 0.5f * std::sin(2 * 3.141592653589793 * leftHz * i / rate);
        samples[2*i+1] = antiphase ? -samples[2*i]
            : 0.25f * std::cos(2 * 3.141592653589793 * 2200 * i / rate);
    }
    return samples;
}
double energy(const QVector<float>& samples, int channel)
{
    double sum = 0;
    const qsizetype first = std::min<qsizetype>(4096, samples.size()/4) * 2;
    for (qsizetype i = first + channel; i < samples.size(); i += 2) {
        sum += static_cast<double>(samples[i]) * samples[i];
    }
    return samples.size() > first ? sum / ((samples.size()-first)/2) : 0;
}
class InjectedBackend final : public IRadioBackend {
public:
    RadioCapabilities capabilities() const override { return {}; }
    bool ownsRxAudio() const override { return true; }
    void connectRadio(const RadioConnectRequest&) override {} // no transport
    void disconnectRadio() override { retirePcmStreams(); emit disconnected(); }
    bool isConnected() const override { return false; }
    void setSliceFrequency(int, double) override {}
    void setSliceMode(int, const QString&) override {}
    void setSliceFilter(int, int, int) override {}
    void setSliceAgc(int, const QString&, int) override {}
    void setPanCenter(const QString&, double, PanCenterIntent) override {}
    void setKeying(bool, const TxCoordinator::Operation&,
                   const TxCoordinator::Completion&) override {}
    void invokeExtension(const QString&, const QString&, quint64, const QVariant&) override {}
    void add(int id, int dax = 0)
    {
        SliceDelta delta;
        delta.frequency = 14.074;
        delta.mode = QStringLiteral("DIGU");
        delta.daxChannel = dax;
        emit sliceChanged(id, delta);
    }
    void remove(int id) { emit sliceRemoved(id); }
};
}

class TciRxAudioTest {
    struct Fixture {
        RadioModel model;
        TciServer server{&model};
        InjectedBackend* backend{nullptr};
        QHash<TciClient*, QList<QByteArray>> packets;
        Fixture()
        {
            replaceBackend();
            for (float& gain : server.m_io->m_rxChannelGain) { gain = 1.0f; }
            server.m_io->m_rxSend = [this](quint64 id, const QByteArray& packet) { TciClient* socket = server.clientById(id);
                packets[socket].append(packet);
                return packet.size();
            };
            server.m_io->m_rxBacklog = [](quint64) { return qint64{0}; };
        }
        ~Fixture()
        {
            for (auto& client : server.m_clients) { delete client.protocol; }
            server.m_clients.clear();
        }
        void replaceBackend()
        {
            auto replacement = std::make_unique<InjectedBackend>();
            backend = replacement.get();
            model.setBackendForTest(std::move(replacement), QStringLiteral("rtl"));
        }
        TciClient* client(int rate = 24000, int receiver = -1, int channels = 2, int format = 3)
        {
            auto* socket = new TciClient(&server);
            TciServer::ClientState state;
            state.socket = socket;
            state.protocol = new TciProtocol(&model, &server.m_routingState, &server.m_trxMap);
            server.m_clients.append(state);
            QObject::connect(socket, &TciClient::textMessageReceived, &server, &TciServer::onTextMessage);
            QObject::connect(socket, &TciClient::disconnected, &server, &TciServer::onClientDisconnected);
            command(socket, QStringLiteral("audio_samplerate:%1;audio_stream_channels:%2;audio_stream_sample_type:%3;audio_start:%4;")
                    .arg(rate).arg(channels).arg(format).arg(receiver));
            return socket;
        }
        void command(TciClient* socket, const QString& text) { emit socket->textMessageReceived(text); if (auto* state = server.clientStateFor(socket)) { server.syncClient(*state); } }
        void feed(int id, const PcmFrame& frame) { emit backend->sliceAudioFrameReady(id, frame); }
    };
    static PcmFrame frame(PcmProducer& producer, int count, float left = 0.25f,
                          float right = -0.5f, std::optional<quint64> first = {}, bool gap = false)
    {
        QVector<float> samples(count * 2);
        for (int i = 0; i < count; ++i) { samples[2*i] = left; samples[2*i+1] = right; }
        const auto value = producer.produce(samples, first, gap);
        check(value.has_value(), "valid test frame production");
        return value.value_or(PcmFrame{});
    }
    static void headers(const QList<QByteArray>& packets, int rate, int receiver, int channels, int format)
    {
        check(!packets.isEmpty(), "accepted stream emits packets");
        for (const QByteArray& packet : packets) {
            bool reservedZero = true;
            for (int i = 8; i < 16; ++i) { reservedZero &= word(packet, i) == 0; }
            check(word(packet,0) == static_cast<quint32>(receiver)
                && word(packet,1) == static_cast<quint32>(rate)
                && word(packet,2) == static_cast<quint32>(format)
                && word(packet,3) == 0 && word(packet,4) == 0 && word(packet,6) == 1
                && word(packet,7) == static_cast<quint32>(channels) && reservedZero,
                "literal TCI RX header fields and reserved words");
            check(word(packet,5) > 0 && word(packet,5) <= static_cast<quint32>(1024*channels)
                && packet.size() == 64 + word(packet,5)*(format == 3 ? 4 : 2),
                "scalar count matches bounded encoded payload");
        }
    }
public:
    static void rateMatrixAndStereo()
    {
        // Catches forced 24k producer interpretation,
        // stereo downmix, unbounded packets and last-client-only accounting.
        for (int sourceRate : {24000,48000}) {
            Fixture f;
            f.backend->add(3);
            PcmProducer producer;
            check(producer.start(PcmPurpose::Slice,3,{sourceRate,PcmLayout::Stereo}), "start matrix producer");
            QList<TciClient*> clients;
            const std::array<int,4> rates{8000,12000,24000,48000};
            for (int rate : rates) { clients.append(f.client(rate,0)); }
            const int inputFrames = 32768;
            const auto input = producer.produce(tone(inputFrames,sourceRate,true));
            f.feed(3,*input);
            qint64 totalFrames = 0;
            for (int i = 0; i < clients.size(); ++i) {
                const auto& packets = f.packets[clients[i]];
                headers(packets,rates[i],0,2,3);
                const QVector<float> samples = floats(packets);
                const double wanted = static_cast<double>(inputFrames)*rates[i]/sourceRate;
                check(std::abs(samples.size()/2-wanted) <= 1.0, "continuous rate-specific frame count");
                check(energy(samples,0)>0.08 && energy(samples,1)>0.08, "antiphase channels retain independent energy");
                bool anti = true;
                for (qsizetype j = 0; j < samples.size(); j += 2) { anti &= std::abs(samples[j]+samples[j+1]) < 0.00001f; }
                check(anti, "antiphase is preserved sample by sample");
                totalFrames += samples.size()/2;
            }
            check(f.server.m_io->m_rxAudioFramesSent == totalFrames, "count actual frames of every client");
        }
        {
            Fixture f; f.backend->add(17);
            TciClient* client = f.client(48000,0);
            PcmProducer producer; producer.start(PcmPurpose::Slice,17,{48000,PcmLayout::Stereo});
            const auto input = producer.produce(tone(32768,48000,false,15000));
            f.feed(17,*input);
            const QVector<float> samples = floats(f.packets[client]);
            check(energy(samples,0)>0.08 && energy(samples,1)>0.02,
                  "48k producer preserves independent 15k left and 2.2k right tones");
        }
    }
    static void unsupportedRatePreservesStream()
    {
        for (int acceptedRate : {48000,24000}) {
            Fixture f; f.backend->add(3);
            // Invalid zero leaves the initial 48k default untouched; the
            // other arm explicitly negotiates 24k before requesting 44.1k.
            TciClient* client = f.client(acceptedRate == 48000 ? 0 : acceptedRate,0);
            TciClient* control = f.client(acceptedRate,0);
            PcmProducer producer;
            producer.start(PcmPurpose::Slice,3,{acceptedRate == 48000 ? 24000 : 48000,PcmLayout::Stereo});
            // Leave an incomplete resampler block pending in both clients.
            f.feed(3,frame(producer,13));
            const auto converters = f.server.m_io->m_rxClients[client->id()].converters;
            const quint64 generation = f.server.clientStateFor(client)->rxGeneration;
            f.command(client,QStringLiteral("audio_samplerate:44100;"));
            check(f.server.clientStateFor(client)->audioSampleRate == acceptedRate,
                  "unsupported 44100 request retains the accepted wire rate");
            check(f.server.clientStateFor(client)->rxGeneration == generation
                  && f.server.m_io->m_rxClients[client->id()].converters == converters,
                  "unsupported rate preserves converter identity and staged input");
            f.feed(3,frame(producer,32768));
            headers(f.packets[client],acceptedRate,0,2,3);
            check(f.packets[client] == f.packets[control],
                  "rejected negotiation preserves exact PCM continuity against untouched client");
        }
    }
    static void sparseRoutingAndSingleFeed()
    {
        Fixture f; f.backend->add(3); f.backend->add(17);
        TciClient* first = f.client(24000,0);
        TciClient* second = f.client(24000,1);
        f.server.m_io->m_rxChannelGain[1] = 0.5f;
        PcmProducer a,b,speaker;
        a.start(PcmPurpose::Slice,3); b.start(PcmPurpose::Slice,17); speaker.start();
        const PcmFrame input = frame(b,3);
        SliceDelta mute; mute.audioMute = true; mute.audioGain = 0;
        emit f.backend->sliceChanged(17,mute);
        emit f.backend->audioFrameReady(frame(speaker,3,0.8f,0.8f));
        check(f.packets[first].isEmpty() && f.packets[second].isEmpty(), "speaker bus never feeds TCI");
        f.feed(17,input);
        check(f.packets[first].isEmpty() && f.packets[second].size()==1, "sparse slice 17 feeds stable TRX1 exactly once while speaker muted");
        check(floats(f.packets[second]) == QVector<float>({0.125f,-0.25f,0.125f,-0.25f,0.125f,-0.25f}), "native gain uses TRX plus one rather than sparse slice id");
        f.backend->remove(3); a.invalidate();
        f.feed(17,frame(b,3));
        headers(f.packets[second],24000,1,2,3);
        f.backend->add(3);
        check(a.start(PcmPurpose::Slice,3), "restart earlier receiver");
        f.feed(3,frame(a,3));
        check(f.packets[first].size()==1 && f.packets[second].size()==2, "earlier slice recreation never renumbers survivor");
    }
    static void formatEncoding()
    {
        Fixture f; f.backend->add(3);
        TciClient* monoFloat = f.client(24000,0,1,3);
        TciClient* stereoInt = f.client(24000,0,2,0);
        TciClient* monoInt = f.client(24000,0,1,0);
        PcmProducer producer; producer.start(PcmPurpose::Slice,3);
        f.feed(3,frame(producer,1,1.5f,-0.5f));
        headers(f.packets[monoFloat],24000,0,1,3);
        headers(f.packets[stereoInt],24000,0,2,0);
        headers(f.packets[monoInt],24000,0,1,0);
        check(floats(f.packets[monoFloat]) == QVector<float>({0.5f}), "mono float averages independently preserved stereo");
        if (f.packets[stereoInt].isEmpty() || f.packets[monoInt].isEmpty()) { return; }
        qint16 stereo[2]{}; qint16 mono{};
        std::memcpy(stereo,f.packets[stereoInt][0].constData()+64,4);
        std::memcpy(&mono,f.packets[monoInt][0].constData()+64,2);
        check(stereo[0]==32767 && stereo[1]==-16384 && mono==16384, "int16 saturation and deliberate mono downmix");
        producer.setFormat({24000,PcmLayout::Mono});
        f.packets.clear();
        const auto input=producer.produce({0.25f,-0.5f});
        f.feed(3,*input);
        check(floats(f.packets[monoFloat]) == QVector<float>({0.25f,-0.5f}), "typed mono input survives conversion and mono encoding");
    }
    static void replayAndEpochs()
    {
        Fixture f; f.backend->add(3);
        PcmProducer producer; producer.start(PcmPurpose::Slice,3);
        const PcmFrame initial=frame(producer,128);
        f.server.onSlicePcmReady(3,initial); // no client yet: still consume cursor
        TciClient* client=f.client();
        f.server.onSlicePcmReady(3,initial);
        check(f.packets[client].isEmpty(), "ingress accepted without subscribers cannot replay after start");
        const PcmFrame next=frame(producer,128);
        f.server.onSlicePcmReady(3,next);
        f.server.onSlicePcmReady(3,next);
        check(f.packets[client].size()==1, "duplicate live frame rejected by server gate");
        if (f.packets[client].isEmpty()) { return; }
        f.command(client,QStringLiteral("audio_samplerate:48000;audio_samplerate:24000;audio_stop;audio_start:0;"));
        f.server.onSlicePcmReady(3,next);
        check(f.packets[client].size()==1, "negotiation and stop never erase replay cursor");
        PcmProducer intruder; intruder.start(PcmPurpose::Slice,3);
        f.server.onSlicePcmReady(3,frame(intruder,128,0.9f,0.9f));
        check(f.packets[client].size()==1, "different live producer cannot steal route");
        producer.setFormat({48000,PcmLayout::Stereo});
        f.server.onSlicePcmReady(3,next);
        const PcmFrame wide=frame(producer,256);
        f.server.onSlicePcmReady(3,wide);
        producer.setFormat({24000,PcmLayout::Stereo});
        const PcmFrame narrow=frame(producer,17,0.7f,-0.2f);
        f.server.onSlicePcmReady(3,narrow);
        check(floats({f.packets[client].last()})==narrow.samples(), "24 to 48 to 24 starts fresh native epoch");
        const qsizetype accepted=f.packets[client].size();
        f.server.onSlicePcmReady(3,wide); f.server.onSlicePcmReady(3,next);
        f.server.onSlicePcmReady(17,frame(producer,128));
        PcmProducer wrong; wrong.start(PcmPurpose::Speaker);
        f.server.onSlicePcmReady(3,frame(wrong,128));
        check(f.packets[client].size()==accepted, "revoked rate generations purpose and slice mismatches rejected");
    }
    static void resetIsolation()
    {
        Fixture f; f.backend->add(3); f.backend->add(17);
        TciClient* client=f.client(48000);
        PcmProducer a,b; a.start(PcmPurpose::Slice,3); b.start(PcmPurpose::Slice,17);
        f.server.onSlicePcmReady(3,frame(a,128,0.9f,0.9f));
        f.server.onSlicePcmReady(17,frame(b,128,0.4f,-0.4f));
        // An admitted discontinuity must discard A's partial block, while B
        // retains its independent partial block and paired filter histories.
        f.server.onSlicePcmReady(3,frame(a,128,0,0,1024,true));
        check(f.packets[client].isEmpty(), "gap drops only prior staging without filling missing duration");
        f.server.onSlicePcmReady(17,frame(b,128,0.4f,-0.4f));
        check(!f.packets[client].isEmpty() && word(f.packets[client].first(),0)==1,
              "resetting one receiver retains other receiver staging");
        if (f.packets[client].isEmpty()) { return; }
        f.server.onSlicePcmReady(3,frame(a,128,0,0));
        const QVector<float> zeros=floats({f.packets[client].last()});
        bool silent=true; for(float value:zeros) { silent &= value==0; }
        check(word(f.packets[client].last(),0)==0 && silent, "gap retires both filter histories before zero continuation");
        f.packets.clear();
        f.server.onSlicePcmReady(3,frame(a,128,0.9f,0.9f));
        f.command(client,QStringLiteral("audio_samplerate:24000;"));
        const PcmFrame current=frame(a,13,-0.2f,0.3f);
        f.server.onSlicePcmReady(3,current);
        check(floats(f.packets[client])==current.samples(), "client rate change drops old rate partial staging");
        // A second client remains uninterrupted when the first changes format.
        TciClient* other=f.client(48000,1);
        f.server.onSlicePcmReady(17,frame(b,128));
        f.command(client,QStringLiteral("audio_stream_channels:1;audio_stream_sample_type:0;"));
        f.server.onSlicePcmReady(17,frame(b,128));
        check(!f.packets[other].isEmpty(), "client format changes preserve another client's staging");
    }
    static void subscriptionAndForwardGapStaging()
    {
        // These fail if a reset drops just filter history or just the partial
        // input block. A zero continuation cannot contain the earlier signal.
        const QStringList resets{QStringLiteral("audio_stop;audio_start:0;"),
            QStringLiteral("audio_stream_channels:1;"),
            QStringLiteral("audio_stream_sample_type:0;")};
        for (const QString& reset : resets) {
            Fixture f; f.backend->add(3); TciClient* client=f.client(48000,0);
            PcmProducer producer; producer.start(PcmPurpose::Slice,3);
            f.server.onSlicePcmReady(3,frame(producer,128,0.9f,0.9f));
            f.command(client,reset);
            f.server.onSlicePcmReady(3,frame(producer,128,0,0));
            check(f.packets[client].isEmpty(), "subscription format or channel reset discards partial input");
            f.server.onSlicePcmReady(3,frame(producer,128,0,0));
            bool zero=!f.packets[client].isEmpty();
            for(const QByteArray& packet:f.packets[client]) {
                if (word(packet,2)==3) {
                    for(float sample:floats({packet})) { zero &= sample==0.0f; }
                } else {
                    for(qsizetype i=64;i<packet.size();++i) { zero &= packet[i]==0; }
                }
            }
            check(zero, "post-reset zero continuation has no stale channel history");
        }
        Fixture f; f.backend->add(3); TciClient* client=f.client(48000,0);
        PcmProducer producer; producer.start(PcmPurpose::Slice,3);
        f.server.onSlicePcmReady(3,frame(producer,128,0.9f,0.9f));
        frame(producer,128); // legitimate missed delivery, without discontinuity flag
        const PcmFrame afterGap=frame(producer,128,0,0);
        check(!afterGap.discontinuity(), "forward-gap fixture uses ordinary continuity metadata");
        f.server.onSlicePcmReady(3,afterGap);
        check(f.packets[client].isEmpty(), "unflagged forward gap discards previous partial block");
        f.server.onSlicePcmReady(3,frame(producer,128,0,0));
        const QVector<float> output=floats(f.packets[client]);
        bool zero=!output.isEmpty(); for(float sample:output) { zero &= sample==0; }
        check(zero, "unflagged forward gap resets paired histories before continuation");
    }
    static void retiredRouteAndCapacity()
    {
        Fixture f; f.backend->add(3);
        TciClient* client=f.client();
        PcmProducer old,replacement; old.start(PcmPurpose::Slice,3);
        f.server.onSlicePcmReady(3,frame(old,3));
        f.backend->remove(3); f.backend->add(3);
        replacement.start(PcmPurpose::Slice,3);
        const PcmFrame pending=frame(replacement,3);
        f.server.onSlicePcmReady(3,pending);
        check(f.packets[client].size()==1, "removed live predecessor tombstone blocks recreated slice");
        old.invalidate();
        f.server.onSlicePcmReady(3,pending);
        check(f.packets[client].size()==2, "revocation permits replacement without consuming refused frame");
        emit f.model.connectionStateChanged(false);
        f.server.onSlicePcmReady(3,frame(replacement,3));
        check(f.packets[client].size()==2, "disconnect retires live route until producer revocation");
        replacement.invalidate(); replacement.start(PcmPurpose::Slice,3);
        f.server.onSlicePcmReady(3,frame(replacement,3));
        check(f.packets[client].size()==3, "new connection epoch restores route");

        Fixture capped;
        TciClient* observer=capped.client();
        std::vector<std::unique_ptr<PcmProducer>> producers;
        for (int id=0; id<32; ++id) {
            capped.backend->add(id);
            auto source=std::make_unique<PcmProducer>(); source->start(PcmPurpose::Slice,id);
            capped.server.onSlicePcmReady(id,frame(*source,1));
            capped.backend->remove(id); // retain live tombstone deliberately
            producers.push_back(std::move(source));
        }
        capped.backend->add(100);
        PcmProducer overflow; overflow.start(PcmPurpose::Slice,100);
        const PcmFrame waiting=frame(overflow,1);
        capped.server.onSlicePcmReady(100,waiting);
        check(capped.packets[observer].size()==32, "32 live pins refuse overflow without evicting a stream");
        producers.front()->invalidate();
        capped.server.onSlicePcmReady(100,waiting);
        check(capped.packets[observer].size()==33, "revoked retired pin reclaimed before capacity check");
    }
    static void daxLifecycle()
    {
        Fixture f; f.backend->add(3,2);
        TciClient* client=f.client();
        PcmProducer producer; producer.start(PcmPurpose::Auxiliary);
        const PcmFrame a=frame(producer,3);
        f.server.onDaxPcmReady(1,a);
        check(f.packets[client].isEmpty(), "cold DAX mapping never guesses a foreign slice");
        f.server.onDaxPcmReady(2,a);
        check(floats(f.packets[client])==a.samples(), "Flex DAX24 preserves stereo sample bytes");
        f.backend->add(3,0);
        f.server.onDaxPcmReady(2,frame(producer,3));
        check(f.packets[client].size()==2, "transient DAX zero retains live owner cache");
        f.server.onDaxStreamUnregistered(2,123);
        f.backend->add(3,2);
        f.server.onDaxPcmReady(2,frame(producer,3));
        check(f.packets[client].size()==2, "DAX unregister retires old live epoch");
        producer.invalidate(); producer.start(PcmPurpose::Auxiliary);
        f.server.onDaxPcmReady(2,frame(producer,3));
        check(f.packets[client].size()==3, "DAX re-registration starts after old revocation");
        f.backend->remove(3); producer.invalidate(); f.backend->add(17);
        producer.start(PcmPurpose::Auxiliary);
        f.server.onDaxPcmReady(2,frame(producer,3));
        check(f.packets[client].size()==3, "dead DAX owner cache cannot migrate to another receiver");
        f.backend->add(17,2); producer.setFormat({48000,PcmLayout::Stereo});
        f.server.onDaxPcmReady(2,frame(producer,3));
        check(f.packets[client].size()==3, "DAX refuses 48k relabeling");
    }
    static void daxOwnerTransition()
    {
        Fixture f; f.backend->add(3,2); f.backend->add(17);
        TciClient* client=f.client(48000);
        PcmProducer producer; producer.start(PcmPurpose::Auxiliary);
        f.server.onDaxPcmReady(2,frame(producer,128,0.9f,0.9f));
        f.backend->add(3,0); f.backend->add(17,2);
        f.server.onDaxPcmReady(2,frame(producer,128,0,0));
        check(f.packets[client].isEmpty(), "DAX owner reassignment retires earlier owner partial staging");
        f.server.onDaxPcmReady(2,frame(producer,128,0,0));
        check(!f.packets[client].isEmpty(), "live channel producer follows authoritative new slice owner");
        if (!f.packets[client].isEmpty()) {
            headers(f.packets[client],48000,1,2,3);
            bool silent=true; for(float sample:floats(f.packets[client])) { silent &= sample==0; }
            check(silent, "DAX owner replacement has no previous owner filter tail");
        }
        f.command(client,QStringLiteral("audio_samplerate:24000;"));
        f.packets.clear();
        f.server.m_io->m_rxSend = [&](quint64 id,const QByteArray& packet) { TciClient* socket = f.server.clientById(id);
            f.packets[socket].append(packet);
            f.backend->add(17,0); f.backend->add(3,2);
            return packet.size();
        };
        f.server.onDaxPcmReady(2,frame(producer,4096));
        check(f.packets[client].size()==1, "DAX owner move during send cancels remaining old receiver packets");
    }
    static void staleFinalCheckAndChurn()
    {
        Fixture f; f.backend->add(3);
        TciClient* client=f.client(); TciClient* other=f.client();
        PcmProducer producer; producer.start(PcmPurpose::Slice,3);
        int sends=0;
        f.server.m_io->m_rxBacklog = [&](quint64) { producer.invalidate(); return qint64{0}; };
        f.server.m_io->m_rxSend = [&](quint64,const QByteArray& packet) { ++sends; return packet.size(); };
        f.server.onSlicePcmReady(3,frame(producer,4096));
        check(sends==0, "epoch revoked after conversion before send never crosses transport");
        producer.start(PcmPurpose::Slice,3);
        f.server.m_io->m_rxBacklog = [](quint64) { return qint64{0}; };
        f.server.m_io->m_rxSend = [&](quint64 id,const QByteArray& packet) { TciClient* socket = f.server.clientById(id);
            ++sends;
            if(socket==client) { emit client->disconnected(); }
            f.packets[socket].append(packet); return packet.size();
        };
        f.server.onSlicePcmReady(3,frame(producer,4096));
        check(f.packets[client].size()==1 && f.packets[other].size()==4,
              "disconnect inside send retires only departing client and does not invalidate iteration");
        f.server.m_io->m_rxSend = [&](quint64 id,const QByteArray& packet) { TciClient* socket = f.server.clientById(id);
            f.packets[socket].append(packet);
            f.command(socket,QStringLiteral("audio_stop;audio_start:0;audio_samplerate:48000;"));
            return packet.size();
        };
        f.packets.clear();
        f.server.onSlicePcmReady(3,frame(producer,4096));
        check(f.packets[other].size()==1, "reentrant stop start and rate change cancel remaining old output");
        // Reentrant server destruction must leave the local converter and
        // callback alive until their stack unwinds.
        RadioModel model;
        auto injected=std::make_unique<InjectedBackend>(); InjectedBackend* backend=injected.get();
        model.setBackendForTest(std::move(injected),QStringLiteral("rtl")); backend->add(3);
        auto server=std::make_unique<TciServer>(&model);
        TciClient socket;
        TciServer::ClientState state; state.socket=&socket; state.audioEnabled=true; state.audioSampleRate=24000;
        server->m_clients.append(state);
        server->syncClient(state);
        server->m_io->m_rxBacklog = [](quint64) { return qint64{0}; };
        server->m_io->m_rxSend = [&](quint64,const QByteArray& packet) { server.reset(); return packet.size(); };
        PcmProducer source; source.start(PcmPurpose::Slice,3);
        server->onSlicePcmReady(3,frame(source,4096));
        check(!server, "server may be destroyed by an output callback without use after free");
    }
    static void levelCallbackRetirement()
    {
        Fixture f; f.backend->add(3);
        TciClient* client=f.client(48000);
        PcmProducer producer; producer.start(PcmPurpose::Slice,3);
        const auto connection=QObject::connect(&f.server,&TciServer::rxLevel,&f.server,[&](int,float) {
            emit f.model.connectionStateChanged(false);
        });
        f.server.onSlicePcmReady(3,frame(producer,128));
        check(f.packets[client].isEmpty() && f.server.m_io->m_rxClients[client->id()].converters.isEmpty(),
              "rxLevel retirement cannot restage sub-block input after clearing route");
        QObject::disconnect(connection);
        producer.invalidate(); producer.start(PcmPurpose::Slice,3);
        const auto stopped=QObject::connect(&f.server,&TciServer::rxLevel,&f.server,[&](int,float) {
            f.command(client,QStringLiteral("audio_stop;audio_start:0;"));
        });
        f.server.onSlicePcmReady(3,frame(producer,128));
        check(f.server.m_io->m_rxClients[client->id()].converters.isEmpty(),
              "rxLevel client reset cancels captured recipient before staging");
        QObject::disconnect(stopped);
    }
    static void negotiationClientChurn()
    {
        Fixture f; f.backend->add(3);
        TciClient* original=f.client();
        bool churned=false;
        const auto connection=QObject::connect(&f.server,&TciServer::clientsChanged,&f.server,[&]() {
            if (churned) { return; }
            churned=true;
            for (int i=0; i<7; ++i) { f.client(); }
        });
        f.command(original,QStringLiteral("audio_start:0;audio_samplerate:12000;"));
        check(f.server.clientStateFor(original)->audioSampleRate==12000,
              "batched RX negotiation refetches client after subscription callbacks grow list");
        QObject::disconnect(connection);
        const auto dropped=QObject::connect(&f.server,&TciServer::tciMessage,&f.server,
            [&](const QString& direction,const QString&) {
                if (direction==QStringLiteral("rx")) { emit original->disconnected(); }
            });
        f.command(original,QStringLiteral("audio_samplerate:24000;"));
        check(!f.server.clientStateFor(original), "inbound monitor callback can remove client before negotiation");
        QObject::disconnect(dropped);
    }
    static void lifecycleChurnAndConcurrentRevocation()
    {
        Fixture f; f.backend->add(3);
        PcmProducer producer;
        for (int iteration=0; iteration<32; ++iteration) {
            TciClient* client=f.client(iteration%2 ? 12000 : 48000,0);
            check(producer.start(PcmPurpose::Slice,3), "churn starts a new producer session");
            for (int rate : {24000,48000,24000}) {
                producer.setFormat({rate,PcmLayout::Stereo});
                f.feed(3,frame(producer,512));
                f.command(client,QStringLiteral("audio_stop;audio_start:0;audio_samplerate:%1;")
                          .arg(rate==24000 ? 48000 : 24000));
                f.feed(3,frame(producer,512));
            }
            check(!f.packets[client].isEmpty(), "churn epoch and subscription transitions continue delivery");
            producer.invalidate();
            f.backend->remove(3);
            if (iteration%4==0) { f.replaceBackend(); }
            f.backend->add(3);
            emit client->disconnected();
            f.packets.remove(client);
        }
        check(f.server.m_clients.isEmpty(), "churn tears down every subscriber");
        TciClient* client=f.client(24000,0);
        producer.start(PcmPurpose::Slice,3);
        const PcmFrame pending=frame(producer,4096);
        std::atomic<bool> request{false};
        std::atomic<bool> revoked{false};
        int packets=0;
        std::thread revoker([&]() {
            request.wait(false,std::memory_order_acquire);
            producer.invalidate(); // the sole permitted concurrent producer operation
            revoked.store(true,std::memory_order_release);
            revoked.notify_one();
        });
        f.server.m_io->m_rxSend = [&](quint64,const QByteArray& packet) {
            ++packets;
            request.store(true,std::memory_order_release);
            request.notify_one();
            revoked.wait(false,std::memory_order_acquire);
            return packet.size();
        };
        f.server.onSlicePcmReady(3,pending);
        // Also releases the worker if a regression prevented the first callback.
        request.store(true,std::memory_order_release);
        request.notify_one();
        revoker.join();
        check(packets==1 && !pending.current(),
              "joined concurrent epoch revocation stops after the already committed packet");
        check(f.server.m_io->m_rxClients[client->id()].converters.isEmpty(),
              "concurrent revocation retires converter staging after callback unwinds");
    }
    static void failedSendIsolation()
    {
        for (bool failed : {false,true}) {
            Fixture f; f.backend->add(3);
            TciClient* slow = f.client();
            TciClient* fast = f.client();
            int attempts = 0;
            f.server.m_io->m_rxSend = [&](quint64 id, const QByteArray& packet) -> qint64 { TciClient* socket = f.server.clientById(id);
                if (socket == slow) {
                    ++attempts;
                    return failed ? -1 : packet.size()-1;
                }
                f.packets[socket].append(packet);
                return packet.size();
            };
            PcmProducer producer; producer.start(PcmPurpose::Slice,3);
            transportWarnings.clear();
            const QtMessageHandler previousHandler = qInstallMessageHandler(captureTransportWarning);
            f.feed(3,frame(producer,4096));
            f.feed(3,frame(producer,4096));
            qInstallMessageHandler(previousHandler);
            check(transportWarnings.size() == 1
                  && transportWarnings.first().contains(failed ? "send failed" : "short send")
                  && transportWarnings.first().contains("packet_bytes= 8256")
                  && transportWarnings.first().contains(failed ? "sent_bytes= -1" : "sent_bytes= 8255"),
                  "send failure logs its reason and exact byte counts once per stopped stream");
            const auto* client = f.server.clientStateFor(slow);
            check(attempts == 1 && client && !client->audioEnabled && f.server.m_io->m_rxClients[slow->id()].converters.isEmpty(),
                  "short or failed send stops and resets audio across current and later batches");
            check(f.packets[fast].size() == 8 && f.server.m_io->m_rxAudioFramesSent == 8192,
                  "failed client contributes no sent frames and healthy client continues");
        }
    }
    static void failedSendSocketDeletion()
    {
        Fixture f; f.backend->add(3);
        std::unique_ptr<TciClient> departing(f.client());
        TciClient* fast = f.client();
        int attempts = 0;
        f.server.m_io->m_rxSend = [&](quint64 id, const QByteArray& packet) -> qint64 { TciClient* socket = f.server.clientById(id);
            if (socket == departing.get()) {
                ++attempts;
                departing.reset();
                return -1;
            }
            f.packets[socket].append(packet);
            return packet.size();
        };
        PcmProducer producer; producer.start(PcmPurpose::Slice,3);
        f.feed(3,frame(producer,4096));
        check(!departing && attempts == 1 && f.packets[fast].size() == 4,
              "failed send may destroy its socket before diagnostics without disrupting healthy client");
    }
    // #5722 review S1: the two refusal paths differ deliberately — a backlog
    // refusal requests graceful close of the shared audio/CAT/PTT socket, a
    // short/failed send only stops audio. Both are stated in the PR body and in
    // docs/tci-receive-audio.md, and until now neither direction was pinned:
    // deleting the close() call left all assertions green.
    // #5722 review: a DAX route retired by a slice removal must recover when
    // the slice comes back. The Slice route recovers because IRadioBackend's
    // ctor revokes its producer on sliceRemoved; a DAX producer lives in
    // PanadapterStream::m_daxPcm and a slice removal revokes NOTHING, so the
    // pin stays current, the retired-route sweep cannot erase it, and the
    // `live && retired` guard drops every later packet. Flex band recall
    // drops and re-creates a slice with the SAME id, so this is a routine
    // operator action, not a corner case.
    static void daxRouteSurvivesSliceRecreate()
    {
        Fixture f; f.backend->add(3,2);
        TciClient* client = f.client();
        PcmProducer producer; producer.start(PcmPurpose::Auxiliary);
        f.server.onDaxPcmReady(2, frame(producer,3));
        check(f.packets[client].size() == 1, "DAX route delivers before the recall");

        // Band recall: slice dropped and re-created with the same id. The DAX
        // producer is NOT revoked — that is the whole point.
        f.backend->remove(3);
        f.backend->add(3,2);
        f.server.onDaxPcmReady(2, frame(producer,3));
        check(f.packets[client].size() == 2,
              "a re-created slice resumes its DAX route without producer revocation");
    }

    static void refusalCloseDistinction()
    {
        // Backlog refusal MUST request close.
        {
            Fixture f; f.backend->add(3);
            TciClient* client = f.client();
            int closes = 0;
            f.server.m_io->m_rxClose = [&](quint64, QWebSocketProtocol::CloseCode, const QString&) { ++closes; };
            f.server.m_io->m_rxBacklog = [](quint64) { return qint64{256*1024}; };
            PcmProducer producer; producer.start(PcmPurpose::Slice,3);
            f.feed(3,frame(producer,4096));
            check(closes == 1, "a backlog refusal requests graceful close of the shared socket");
        }
        // A short send MUST NOT close: the socket still carries CAT and PTT.
        {
            Fixture f; f.backend->add(3);
            TciClient* client = f.client();
            int closes = 0;
            f.server.m_io->m_rxClose = [&](quint64, QWebSocketProtocol::CloseCode, const QString&) { ++closes; };
            f.server.m_io->m_rxSend = [&](quint64, const QByteArray& packet) -> qint64 {
                return packet.size() - 1;
            };
            PcmProducer producer; producer.start(PcmPurpose::Slice,3);
            f.feed(3,frame(producer,4096));
            TciServer::ClientState* state = f.server.clientStateFor(client);
            check(closes == 0 && state && !state->audioEnabled,
                  "a short send stops audio without closing the shared socket");
        }
    }

    static void backlogDiagnostics()
    {
        for (qint64 pending : {qint64{-1},qint64{256*1024}}) {
            Fixture f; f.backend->add(3);
            TciClient* client = f.client();
            f.server.m_io->m_rxBacklog = [pending](quint64) { return pending; };
            PcmProducer producer; producer.start(PcmPurpose::Slice,3);
            transportWarnings.clear();
            const QtMessageHandler previousHandler = qInstallMessageHandler(captureTransportWarning);
            f.feed(3,frame(producer,4096));
            f.feed(3,frame(producer,4096));
            qInstallMessageHandler(previousHandler);
            check(transportWarnings.size() == 1
                  && transportWarnings.first().contains(pending < 0 ? "invalid backlog" : "backlog limit")
                  && transportWarnings.first().contains(QStringLiteral("pending_bytes= %1").arg(pending))
                  && transportWarnings.first().contains("packet_bytes= 8256"),
                  "backlog rejection logs its reason and exact byte counts once per stopped stream");
            check(f.packets[client].isEmpty(), "invalid or excessive backlog admits no packet");
        }
    }
    static void pressureAndReplacement()
    {
        Fixture f; f.backend->add(3);
        TciClient* slow=f.client(); TciClient* fast=f.client();
        f.server.m_io->m_rxBacklog = [&](quint64 id) { TciClient* socket = f.server.clientById(id); return socket==slow ? qint64{256*1024} : qint64{0}; };
        PcmProducer producer; producer.start(PcmPurpose::Slice,3);
        f.server.onSlicePcmReady(3,frame(producer,65536));
        check(f.packets[slow].isEmpty() && f.packets[fast].size()==64,
              "backlog cap refuses slow client while bounded packets serve other client");
        check(!f.server.clientStateFor(slow) || !f.server.clientStateFor(slow)->audioEnabled,
              "pressured client stops accumulating audio");
        const PcmFrame old=frame(producer,3);
        f.replaceBackend(); f.backend->add(3);
        f.server.onSlicePcmReady(3,old);
        check(f.packets[fast].size()==64, "backend replacement holds old live source tombstone");
        producer.invalidate(); producer.start(PcmPurpose::Slice,3);
        f.feed(3,frame(producer,3));
        check(f.packets[fast].size()==65, "production binding survives backend replacement exactly once");
    }
    static void ingressOutlivesController()
    {
        RadioModel model;
        auto server = std::make_unique<TciServer>(&model);
        const auto sink = server->daxPcmSink();
        PcmProducer producer;
        producer.start(PcmPurpose::Auxiliary);
        const PcmFrame pcm = frame(producer, 16);
        std::atomic<bool> entered{false};
        std::thread source([&] {
            entered.store(true, std::memory_order_release);
            for (int i = 0; i < 2048; ++i) { sink(1, pcm); }
        });
        while (!entered.load(std::memory_order_acquire)) { std::this_thread::yield(); }
        server.reset();
        source.join();
        sink(1, pcm);
        check(true, "copied network ingress remains inert after concurrent controller destruction");
    }

    static int run()
    {
        ingressOutlivesController(); rateMatrixAndStereo(); unsupportedRatePreservesStream(); sparseRoutingAndSingleFeed(); formatEncoding();
        replayAndEpochs(); resetIsolation(); subscriptionAndForwardGapStaging(); retiredRouteAndCapacity(); daxLifecycle(); daxOwnerTransition();
        staleFinalCheckAndChurn(); levelCallbackRetirement(); negotiationClientChurn(); lifecycleChurnAndConcurrentRevocation(); failedSendIsolation(); failedSendSocketDeletion(); backlogDiagnostics(); refusalCloseDistinction(); daxRouteSurvivesSliceRecreate(); pressureAndReplacement();
        std::printf("TCI RX: %d checks, %d failures\n",checks,failures);
        return failures==0 ? 0 : 1;
    }
};
}
int main(int argc,char** argv)
{
    TestSettingsProfile profile(QStringLiteral("tci-rx-audio-test"));
    QCoreApplication app(argc,argv);
    AetherSDR::AppSettings::instance().load();
    return AetherSDR::TciRxAudioTest::run();
}
