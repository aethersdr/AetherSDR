// A TCI client that never sends aethersdr_ext_enable:true must receive exactly
// the bytes it would receive from a server without the extension set (RFC #7181).
// Two clients are opened as onClientOpened() opens them and every text frame is
// read at the I/O worker's last step, init burst included. Socket-free. Without
// the set (main) the file still builds; `--print-transcript` prints the bytes.

#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/TciClient.h"
#include "core/TciIoWorker.h"
#include "core/TciProtocol.h"
#include "core/TciServer.h"
#include "models/DaxIqModel.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#if __has_include("core/TciExtensionSet.h")
#include "core/TciExtensionSet.h"
#define HAVE_TCI_EXTENSION_SET 1
#endif

#include <QCoreApplication>
#include <QEventLoop>
#include <QHash>
#include <QHostAddress>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>

namespace AetherSDR {

// The existing friend accessor; this TU's own definition, as in
// dax_iq_rate_limits_test.cpp.
class TciServerReviewTest {
public:
    // Every text frame the worker would write, by client id.
    static void tapText(TciServer& server, std::function<void(quint64, const QString&)> tap)
    {
        server.m_io->m_textSend = std::move(tap);
    }

    // Opens a client the way the worker's clientOpened signal does; no socket.
    static TciClient* open(TciServer& server, std::shared_ptr<TciClientLifetime> lifetime,
                           quint16 port)
    {
        server.m_running.store(true);
        server.onClientOpened(std::move(lifetime), QHostAddress(QStringLiteral("192.0.2.1")), port);
        return server.m_clients.isEmpty() ? nullptr : server.m_clients.last().socket;
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

struct Transcripts {
    QStringList plain;
    QStringList peer;
};

// A stand-in command group: a read answers, a set broadcasts.
struct Probe {
    bool offered{true};
#ifdef HAVE_TCI_EXTENSION_SET
    TciExtensionSet::Group group()
    {
        TciExtensionSet::Group g;
        g.token = QStringLiteral("probe");
        g.commands = {QStringLiteral("aethersdr_probe")};
        g.offered = [this] { return offered; };
        g.stateLines = [] { return QStringList{QStringLiteral("aethersdr_probe:7;")}; };
        g.handle = [](const QString&, const QStringList& args) {
            TciExtensionSet::Result result;
            if (args.isEmpty()) {
                result.reply = QStringLiteral("aethersdr_probe:7;");
            } else {
                result.broadcast = QStringLiteral("aethersdr_probe:%1;").arg(args.first());
            }
            return result;
        };
        return g;
    }
#endif
};

// One script, run twice. The peer sends the same plain commands both times;
// with `peerUsesExtensions` it also enables the set, uses it, malforms it and
// disables it, across capability edges. Returns what each client received.
Transcripts runScript(bool peerUsesExtensions)
{
    RadioModel model;
    QString error;
    model.automationApplySliceFixture(0, QStringLiteral("A"), &error);
    model.automationApplySliceFixture(1, QStringLiteral("B"), &error);
    auto server = std::make_unique<TciServer>(&model);
    for (SliceModel* slice : model.slices()) {
        server->wireSlice(slice->sliceId(), slice);
    }
    Probe probe;
#ifdef HAVE_TCI_EXTENSION_SET
    server->registerExtensionGroup(probe.group());
#endif

    Transcripts rx;
    QHash<quint64, QStringList*> byId;
    TciServerReviewTest::tapText(*server, [&byId](quint64 id, const QString& text) {
        if (QStringList* sink = byId.value(id)) {
            *sink << text;
        }
    });
    // Routed before open(): the init burst is written inside it.
    auto plainLife = std::make_shared<TciClientLifetime>();
    auto peerLife = std::make_shared<TciClientLifetime>();
    byId.insert(plainLife->id, &rx.plain);
    byId.insert(peerLife->id, &rx.peer);
    TciClient* plain = TciServerReviewTest::open(*server, plainLife, 50001);
    TciClient* peer = TciServerReviewTest::open(*server, peerLife, 50002);

    const auto plainSends = [plain](const char* text) {
        emit plain->textMessageReceived(QString::fromLatin1(text));
    };
    const auto peerSends = [peer](const char* text) {
        emit peer->textMessageReceived(QString::fromLatin1(text));
    };
    const auto peerExtension = [&](const char* text) {
        if (peerUsesExtensions) {
            peerSends(text);
        }
    };
    const auto capabilityEdge = [&](bool offered) {
        probe.offered = offered;
        emit model.capabilitiesChanged(true, model.backendCapabilities());
    };

    plainSends("audio_samplerate:48000;rx_sensors_enable:true;vfo:0,0;modulation:0;drive;");
    peerExtension("aethersdr_ext_enable:true;");
    peerSends("rx_sensors_enable:true;modulation:0;rx_mute:0;cw_macros_speed:25;");
    peerExtension("aethersdr_probe;aethersdr_probe:5;aethersdr_unknown:1;"
                  "aethersdr_ext;aethersdr_ext:1;");
    peerExtension("aethersdr_ext_enable;aethersdr_ext_enable:maybe;");
    capabilityEdge(true);
    capabilityEdge(false);
    TciServerReviewTest::broadcastStatus(*server);
    plainSends("aethersdr_ext_enable;aethersdr_probe:3;aethersdr_rx_span:0;");
    model.slice(0)->setNb(true);
    spin(600);
    capabilityEdge(true);
    peerExtension("aethersdr_ext_enable:false;aethersdr_probe:4;");
    capabilityEdge(false);
    probe.offered = true;
    peerExtension("AETHERSDR_EXT_ENABLE:1;aethersdr_probe:6;");
    emit peer->disconnected();
    spin(50);
    plainSends("vfo:0,0;");
    TciServerReviewTest::broadcastStatus(*server);
    spin(50);

    server.reset();
    return rx;
}

QStringList extensionLines(const QStringList& lines)
{
    QStringList out;
    for (const QString& line : lines) {
        if (line.contains(QLatin1String("aethersdr_"), Qt::CaseInsensitive)) {
            out << line;
        }
    }
    return out;
}

bool hasLineStarting(const QStringList& lines, const char* prefix)
{
    return std::any_of(lines.cbegin(), lines.cend(), [prefix](const QString& line) {
        return line.startsWith(QLatin1String(prefix));
    });
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
    const Transcripts quiet = runScript(false);
    const Transcripts busy = runScript(true);

    if (printTranscript) {
        for (const QString& line : busy.plain) {
            std::printf("plain< %s\n", qPrintable(line));
        }
    }

    // Positive controls: the paths that can reach the plain client did.
    check(!quiet.plain.isEmpty() && quiet.plain.first().startsWith(QLatin1String("vfo_limits:")),
          "plain client's transcript starts with the init burst as the worker writes it");
    check(hasLineStarting(busy.plain, "cw_macros_speed:25;"),
          "plain client receives the peer's pending notification");
    check(hasLineStarting(busy.plain, "rx_nb_enable:"),
          "plain client receives a state-change broadcast");

    check(extensionLines(quiet.plain + busy.plain).isEmpty(),
          "plain client never receives an aethersdr_ byte");
    check(busy.plain == quiet.plain,
          "plain client's traffic is identical whatever the peer does with the set");
    if (busy.plain != quiet.plain) {
        for (int i = 0; i < std::max(busy.plain.size(), quiet.plain.size()); ++i) {
            const QString a = i < quiet.plain.size() ? quiet.plain.at(i) : QStringLiteral("<none>");
            const QString b = i < busy.plain.size() ? busy.plain.at(i) : QStringLiteral("<none>");
            if (a != b) {
                std::printf("      first difference at %d: quiet=%s busy=%s\n", i,
                            qPrintable(a), qPrintable(b));
                break;
            }
        }
    }

#ifdef HAVE_TCI_EXTENSION_SET
    // The enabled peer shows the same path carried the set to it.
    const QStringList expected = {
        QStringLiteral("aethersdr_ext:1,probe;"), QStringLiteral("aethersdr_probe:7;"),
        QStringLiteral("aethersdr_probe:7;"), QStringLiteral("aethersdr_probe:5;"),
        QStringLiteral("aethersdr_ext:1;"),
        QStringLiteral("aethersdr_ext:1,probe;"), QStringLiteral("aethersdr_probe:7;"),
        QStringLiteral("aethersdr_ext:1,probe;"), QStringLiteral("aethersdr_probe:7;"),
        QStringLiteral("aethersdr_probe:6;"),
    };
    const QStringList peerGot = extensionLines(busy.peer);
    check(peerGot == expected, "the enabled peer receives every extension line, in order");
    if (peerGot != expected) {
        for (const QString& line : peerGot) {
            std::printf("      peer< %s\n", qPrintable(line));
        }
    }
    check(!busy.peer.isEmpty() && busy.peer.first().startsWith(QLatin1String("vfo_limits:")),
          "the peer's init burst reaches it through the worker too");
    check(extensionLines(quiet.peer).isEmpty(), "a peer that never enables receives none");
#endif

    std::printf("%d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
