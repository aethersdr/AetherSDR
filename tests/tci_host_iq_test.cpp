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
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <numbers>
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

        // Asked for the radio's own rate: nothing to decimate.
        f.command(a, QStringLiteral("iq_samplerate:96000;"));
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
        check(f.said(a, QStringLiteral("iq_samplerate:48000;")) && f.server.m_hostIqRate == 96000,
              "a lower rate request is granted while the radio stays at its own");
        f.backend->feed(3, 2048, 96000);
        check(f.frames[a].size() == 2 && word(f.frames[a].last(),1) == 48000
                  && word(f.frames[a].last(),5) == 2048,
              "the next frame carries half the pairs at the requested rate");

        f.command(a, QStringLiteral("iq_stop:0;"));
        check(!f.backend->tapped(3), "iq_stop releases the tap");
        f.backend->feed(3, 2048, 96000);
        check(f.frames[a].size() == 2, "no frames after iq_stop");
    }
    // The HL2 case behind the 1009 closes: a 384 kHz pan streamed 3 MB/s of
    // IQ to a phone that never asked for more than 192 kHz.
    static void decimatesToRequestedRate()
    {
        Fixture f; f.backend->add(3);
        TciClient* a = f.client();
        f.command(a, QStringLiteral("iq_start:0;"));
        f.backend->feed(3, 4096, 384000);
        check(f.said(a, QStringLiteral("iq_samplerate:48000;")) && !f.said(a, QStringLiteral("iq_samplerate:384000;")),
              "unasked, the TCI default of 48 kHz is announced, not the pan's rate");
        check(f.frames[a].size() == 1 && word(f.frames[a].first(),1) == 48000
                  && word(f.frames[a].first(),5) == 2 * 512,
              "384 kHz is decimated by eight");

        f.command(a, QStringLiteral("iq_samplerate:192000;"));
        check(f.said(a, QStringLiteral("iq_samplerate:192000;")), "192 kHz is granted below a 384 kHz radio");
        f.backend->feed(3, 4096, 384000);
        check(f.frames[a].size() == 2 && word(f.frames[a].last(),1) == 192000
                  && word(f.frames[a].last(),5) == 2 * 2048,
              "and the frames follow at 192 kHz");

        f.backend->feed(3, 4096, 96000);
        check(f.said(a, QStringLiteral("iq_samplerate:96000;")) && word(f.frames[a].last(),1) == 96000,
              "a radio below the request is passed through at its own rate, never raised");
        const auto at96k = f.texts[a].count(QStringLiteral("iq_samplerate:96000;"));
        f.command(a, QStringLiteral("iq_samplerate:192000;"));
        check(f.texts[a].count(QStringLiteral("iq_samplerate:96000;")) == at96k + 1,
              "and a request above it is answered with what the frames carry");
    }
    static void decimatorKeepsTonesAndRejectsAliases()
    {
        // Complex tone at a fraction of the input rate, amplitude 1.
        auto tone = [](double cyclesPerSample, int pairs) {
            std::vector<float> iq(static_cast<std::size_t>(pairs) * 2);
            for (int n = 0; n < pairs; ++n) {
                iq[2*n] = static_cast<float>(std::cos(2.0 * std::numbers::pi * cyclesPerSample * n));
                iq[2*n+1] = static_cast<float>(std::sin(2.0 * std::numbers::pi * cyclesPerSample * n));
            }
            return iq;
        };
        // Peak magnitude past the filter's start-up transient.
        auto peak = [](const std::vector<float>& out) {
            double p = 0.0;
            for (std::size_t n = 200; 2 * n + 1 < out.size(); ++n) {
                p = std::max(p, std::hypot(double(out[2*n]), double(out[2*n+1])));
            }
            return p;
        };
        const int pairs = 8192;
        std::vector<float> out;

        TciIqDecimator pass;
        pass.configure(384000, 192000);
        auto in = tone(0.2, pairs);   // 76.8 kHz: 0.4 of the 192 kHz span
        pass.process(in.data(), pairs, out);
        check(out.size() == 2 * (pairs / 2) && std::abs(peak(out) - 1.0) < 0.01,
              "a tone inside the decimated span keeps its level");

        TciIqDecimator alias;
        alias.configure(384000, 192000);
        in = tone(-0.35, pairs);      // would fold onto +0.3 of the output rate
        alias.process(in.data(), pairs, out);
        check(peak(out) < 0.001, "a tone outside it is rejected, not folded in (> 60 dB)");

        // Block boundaries of any length, odd ones included, change nothing.
        TciIqDecimator whole;
        TciIqDecimator pieces;
        whole.configure(384000, 48000);
        pieces.configure(384000, 48000);
        in = tone(0.01, pairs);
        std::vector<float> wholeOut;
        whole.process(in.data(), pairs, wholeOut);
        std::vector<float> joined;
        for (int at = 0, step = 1; at < pairs; at += step, step = step % 37 + 3) {
            const int n = std::min(step, pairs - at);
            pieces.process(in.data() + 2 * at, n, out);
            joined.insert(joined.end(), out.begin(), out.end());
        }
        check(joined == wholeOut && wholeOut.size() == 2 * pairs / 8,
              "uneven blocks give the same stream as one block");

        check(TciIqDecimator::outputRate(384000, 192000) == 192000
                  && TciIqDecimator::outputRate(384000, 24000) == 24000
                  && TciIqDecimator::outputRate(96000, 192000) == 96000
                  && TciIqDecimator::outputRate(48000, 48000) == 48000,
              "rates halve toward the request and never go up");
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
        decimatesToRequestedRate(); decimatorKeepsTonesAndRejectsAliases();
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
