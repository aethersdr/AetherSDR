// A TCI client that never sends aethersdr_ext_enable:true must receive exactly
// the bytes it would receive from a server without the extension set (RFC,
// "SunSDR-only clients get byte-identical traffic"). This file uses only
// TciServer API that predates the set, so the same source builds against a
// tree without it; `--print-transcript` prints the plain client's traffic so
// two builds can be diffed. Socket-free: the transport is injected.

#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/TciClient.h"
#include "core/TciProtocol.h"
#include "core/TciServer.h"
#include "models/DaxIqModel.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>

namespace AetherSDR {

// The existing friend accessor; this TU's own definition, as in
// dax_iq_rate_limits_test.cpp.
class TciServerReviewTest {
public:
    static void attach(TciServer& server, TciClient* client, RadioModel& model)
    {
        TciServer::ClientState state;
        state.socket = client;
        state.protocol = new TciProtocol(&model, &server.m_routingState, &server.m_trxMap);
        server.m_clients.append(state);
        QObject::connect(client, &TciClient::textMessageReceived,
                         &server, &TciServer::onTextMessage);
        QObject::connect(client, &TciClient::disconnected,
                         &server, &TciServer::onClientDisconnected);
    }

    // What sendInitBurst() hands the transport: one message per command.
    static QStringList initBurst(TciServer& server, TciClient* client)
    {
        QStringList messages;
        for (const TciServer::ClientState& state : server.m_clients) {
            if (state.socket == client) {
                const QStringList commands
                    = state.protocol->generateInitBurst().split(QLatin1Char(';'),
                                                                Qt::SkipEmptyParts);
                for (const QString& command : commands) {
                    messages << command + QLatin1Char(';');
                }
            }
        }
        return messages;
    }

    static void broadcastStatus(TciServer& server) { server.broadcastStatus(); }
};

} // namespace AetherSDR

using namespace AetherSDR;

namespace {

int g_failures = 0;

void check(bool ok, const char* what)
{
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) {
        ++g_failures;
    }
}

void spin(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

// One script, run twice. The peer sends the same plain commands both times;
// with `peerUsesExtensions` it also enables the set, uses it, sends malformed
// and unknown extension commands, disables it, and sees capability edges
// while enabled. Returns everything the plain client received, in order.
QStringList runScript(bool peerUsesExtensions)
{
    RadioModel model;
    QString error;
    model.automationApplySliceFixture(0, QStringLiteral("A"), &error);
    model.automationApplySliceFixture(1, QStringLiteral("B"), &error);
    auto server = std::make_unique<TciServer>(&model);
    for (SliceModel* slice : model.slices()) {
        server->wireSlice(slice->sliceId(), slice);
    }

    QStringList plainRx;
    auto* plain = new TciClient;
    QPointer<TciClient> peer(new TciClient);
    plain->textSink = [&plainRx](const QString& text) {
        plainRx << text;
        return static_cast<qint64>(text.size());
    };
    peer->textSink = [](const QString& text) { return static_cast<qint64>(text.size()); };
    TciServerReviewTest::attach(*server, plain, model);
    plainRx << TciServerReviewTest::initBurst(*server, plain);
    TciServerReviewTest::attach(*server, peer, model);

    const auto plainSends = [plain](const char* text) {
        emit plain->textMessageReceived(QString::fromLatin1(text));
    };
    const auto peerSends = [&peer](const char* text) {
        emit peer->textMessageReceived(QString::fromLatin1(text));
    };
    const auto peerExtension = [&](const char* text) {
        if (peerUsesExtensions) {
            peerSends(text);
        }
    };

    plainSends("audio_samplerate:48000;rx_sensors_enable:true;vfo:0,0;modulation:0;drive;");
    peerExtension("aethersdr_ext_enable:true;");
    peerSends("rx_sensors_enable:true;modulation:0;rx_mute:0;");
    peerExtension("aethersdr_tx_filter_band:0;aethersdr_tx_filter_band:0,100,2900;"
                  "aethersdr_hl2_ioboard_status;aethersdr_ext;aethersdr_ext:1;");
    peerExtension("aethersdr_ext_enable;aethersdr_ext_enable:maybe;AETHERSDR_EXT_ENABLE:TRUE;");
    emit model.capabilitiesChanged(true, model.backendCapabilities());
    TciServerReviewTest::broadcastStatus(*server);
    plainSends("aethersdr_ext_enable;aethersdr_rx_span:0;");
    model.slice(0)->setNb(true);
    spin(600);
    emit model.capabilitiesChanged(false, model.backendCapabilities());
    peerExtension("aethersdr_ext_enable:false;aethersdr_tx_filter_band:0;");
    emit model.capabilitiesChanged(true, model.backendCapabilities());
    peerExtension("aethersdr_ext_enable:1;");
    emit peer->disconnected();
    spin(50);
    plainSends("vfo:0,0;");
    TciServerReviewTest::broadcastStatus(*server);
    spin(50);

    server.reset();
    delete plain;
    delete peer.data();
    return plainRx;
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile settingsProfile(QStringLiteral("aether-tci-extension-zero-traffic-test"));
    DaxIqModel::setPactlRunnerForTest([](const QStringList&) { return quint32{0}; });
    QCoreApplication app(argc, argv);
    AppSettings::instance().load();
    const bool printTranscript = argc > 1 && std::strcmp(argv[1], "--print-transcript") == 0;

    check(settingsProfile.isValid(), "isolated settings profile");
    const QStringList quiet = runScript(false);
    const QStringList busy = runScript(true);

    if (printTranscript) {
        for (const QString& line : busy) {
            std::printf("plain< %s\n", qPrintable(line));
        }
    }

    check(!quiet.isEmpty() && quiet.first().startsWith(QLatin1String("vfo_limits:")),
          "plain client's transcript starts with the init burst");
    bool mentionsSet = false;
    for (const QString& line : quiet + busy) {
        mentionsSet = mentionsSet || line.contains(QLatin1String("aethersdr_"), Qt::CaseInsensitive);
    }
    check(!mentionsSet, "plain client never receives an aethersdr_ byte");
    check(busy == quiet, "plain client's traffic is identical whatever the peer does with the set");
    if (busy != quiet) {
        for (int i = 0; i < std::max(busy.size(), quiet.size()); ++i) {
            const QString a = i < quiet.size() ? quiet.at(i) : QStringLiteral("<none>");
            const QString b = i < busy.size() ? busy.at(i) : QStringLiteral("<none>");
            if (a != b) {
                std::printf("      first difference at %d: quiet=%s busy=%s\n", i,
                            qPrintable(a), qPrintable(b));
                break;
            }
        }
    }

    std::printf("%d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
