// TCI IQ from a backend with its own IQ (HL2): no DAX IQ plane, the backend's
// per-slice tap instead. Socket-free: real RadioModel and TciServer, an
// injected backend that records the taps and emits sliceIqFrameReady.
#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/TciServer.h"
#include "core/TciClient.h"
#include "core/backends/IRadioBackend.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"
#include <QCoreApplication>
#include <QHash>
#include <QStringList>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

namespace AetherSDR {
namespace {
int checks = 0;
int failures = 0;
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
float sample(const QByteArray& packet, int index)
{
    float value = 0;
    std::memcpy(&value, packet.constData() + 64 + index * 4, 4);
    return value;
}
class HostIqBackend final : public IRadioBackend {
public:
    RadioCapabilities capabilities() const override { return {}; }   // no DAX
    bool ownsRxAudio() const override { return true; }
    void connectRadio(const RadioConnectRequest&) override {}
    void disconnectRadio() override { emit disconnected(); }
    bool isConnected() const override { return false; }
    void setSliceFrequency(int, double) override {}
    void setSliceMode(int, const QString&) override {}
    void setSliceFilter(int, int, int) override {}
    void setSliceAgc(int, const QString&, int) override {}
    void setPanCenter(const QString&, double, PanCenterIntent) override {}
    void setKeying(bool, const TxCoordinator::Operation&,
                   const TxCoordinator::Completion&) override {}
    void invokeExtension(const QString&, const QString&, quint64, const QVariant&) override {}
    bool setSliceIqTap(int sliceId, bool on) override
    {
        if (on && refuse) { return false; }
        taps[sliceId] = on;
        return true;
    }
    bool tapped(int sliceId) const { return taps.value(sliceId, false); }
    void add(int id)
    {
        SliceDelta delta;
        delta.frequency = 14.074;
        delta.mode = QStringLiteral("USB");
        emit sliceChanged(id, delta);
    }
    void feed(int id, int pairs, int rate)
    {
        std::vector<float> iq(static_cast<std::size_t>(pairs) * 2);
        for (int i = 0; i < pairs; ++i) { iq[2*i] = 0.25f; iq[2*i+1] = -0.5f; }
        emit sliceIqFrameReady(id, QByteArray(reinterpret_cast<const char*>(iq.data()),
                                              static_cast<qsizetype>(iq.size() * sizeof(float))),
                               rate);
    }
    QHash<int, bool> taps;
    bool refuse = false;
};
}

class TciHostIqTest {
    struct Fixture {
        RadioModel model;
        TciServer server{&model};
        HostIqBackend* backend{nullptr};
        QHash<TciClient*, QList<QByteArray>> frames;
        int binaryFrames = 0;  // IQ must take the droppable stream path
        QHash<TciClient*, QStringList> texts;
        Fixture()
        {
            auto replacement = std::make_unique<HostIqBackend>();
            backend = replacement.get();
            model.setBackendForTest(std::move(replacement), QStringLiteral("hl2"));
        }
        ~Fixture()
        {
            for (auto& client : server.m_clients) { delete client.protocol; }
            server.m_clients.clear();
        }
        TciClient* client()
        {
            auto* socket = new TciClient(&server);
            socket->textSink = [this, socket](const QString& t) { texts[socket].append(t); return qint64(t.size()); };
            socket->streamSink = [this, socket](const QByteArray& b) { frames[socket].append(b); return qint64(b.size()); };
            socket->binarySink = [this](const QByteArray& b) { ++binaryFrames; return qint64(b.size()); };
            TciServer::ClientState state;
            state.socket = socket;
            state.protocol = new TciProtocol(&model, &server.m_routingState, &server.m_trxMap);
            server.m_clients.append(state);
            QObject::connect(socket, &TciClient::textMessageReceived, &server, &TciServer::onTextMessage);
            QObject::connect(socket, &TciClient::disconnected, &server, &TciServer::onClientDisconnected);
            return socket;
        }
        void command(TciClient* socket, const QString& text)
        {
            emit socket->textMessageReceived(text);
            if (auto* state = server.clientStateFor(socket)) { server.syncClient(*state); }
        }
        bool said(TciClient* socket, const QString& text) const
        {
            return texts.value(socket).contains(text);
        }
    };
public:
    static void startStreamsStop()
    {
        Fixture f; f.backend->add(3); f.backend->add(17);
        TciClient* a = f.client();
        f.command(a, QStringLiteral("iq_start:0;"));
        check(f.said(a, QStringLiteral("iq_start:0;")), "iq_start confirmed without a DAX IQ plane");
        check(f.backend->tapped(3) && !f.backend->tapped(17), "only the receiver's slice is tapped");

        f.backend->feed(3, 2048, 96000);
        check(f.said(a, QStringLiteral("iq_samplerate:96000;")), "the radio's rate is announced");
        check(f.frames[a].size() == 1, "one frame per backend run");
        check(f.binaryFrames == 0, "IQ is sent droppable, never on the fatal binary path");
        if (!f.frames[a].isEmpty()) {
            const QByteArray& p = f.frames[a].first();
            check(word(p,0) == 0 && word(p,1) == 96000 && word(p,2) == 3
                  && word(p,6) == 0 && word(p,7) == 2 && word(p,5) == 4096
                  && p.size() == 64 + 4096 * 4,
                  "IQ header: receiver, rate, float32, type IQ, two channels, length");
            check(sample(p,0) == 0.25f && sample(p,1) == -0.5f, "I,Q pass through unchanged");
        }
        f.backend->feed(17, 2048, 96000);
        check(f.frames[a].size() == 1, "an untapped slice's IQ is dropped");

        f.command(a, QStringLiteral("iq_samplerate:48000;"));
        check(f.said(a, QStringLiteral("iq_samplerate:96000;")) && f.server.m_hostIqRate == 96000,
              "a rate request is answered with the radio's rate");

        f.command(a, QStringLiteral("iq_stop:0;"));
        check(!f.backend->tapped(3), "iq_stop releases the tap");
        f.backend->feed(3, 2048, 96000);
        check(f.frames[a].size() == 1, "no frames after iq_stop");
    }
    static void sharedAndDisconnect()
    {
        Fixture f; f.backend->add(3);
        TciClient* a = f.client();
        TciClient* b = f.client();
        f.command(a, QStringLiteral("iq_start:0;"));
        f.command(b, QStringLiteral("iq_start:0;"));
        f.command(a, QStringLiteral("iq_stop:0;"));
        check(f.backend->tapped(3), "the tap survives while another client wants it");
        f.backend->feed(3, 1024, 192000);
        check(f.frames[a].isEmpty() && f.frames[b].size() == 1, "frames go to the subscriber only");
        emit b->disconnected();
        check(!f.backend->tapped(3), "the last subscriber's disconnect releases the tap");
    }
    static void secondReceiverKeepsFirst()
    {
        Fixture f; f.backend->add(3); f.backend->add(17);
        TciClient* a = f.client();
        f.command(a, QStringLiteral("iq_start:0;"));
        f.command(a, QStringLiteral("iq_start:1;"));
        check(f.backend->tapped(3) && f.backend->tapped(17), "two receivers hold two taps");
        f.backend->feed(17, 2048, 48000);
        check(f.frames[a].size() == 1 && word(f.frames[a].first(),0) == 1,
              "the second slice's frames carry its own receiver");
    }
    static void refusalAnswers()
    {
        Fixture f; f.backend->add(3);
        f.backend->refuse = true;
        TciClient* a = f.client();
        f.command(a, QStringLiteral("iq_start:0;"));
        check(f.said(a, QStringLiteral("iq_stop:0;")) && f.server.m_hostIqSlices.isEmpty(),
              "a backend without IQ refuses with iq_stop");
    }
    static int run()
    {
        startStreamsStop(); sharedAndDisconnect(); secondReceiverKeepsFirst(); refusalAnswers();
        std::printf("TCI host IQ: %d checks, %d failures\n", checks, failures);
        return failures == 0 ? 0 : 1;
    }
};
}
int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("tci-host-iq-test"));
    QCoreApplication app(argc, argv);
    AetherSDR::AppSettings::instance().load();
    return AetherSDR::TciHostIqTest::run();
}
