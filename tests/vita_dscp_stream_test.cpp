#include "core/backends/flex/PanadapterStream.h"
#include "core/backends/flex/RadioConnection.h"
#include "TestSettingsProfile.h"

#include <QCoreApplication>
#include <QNetworkDatagram>
#include <QUdpSocket>
#include <cstddef>
#include <cstdio>
#include <vector>

namespace AetherSDR {
// Injecting apply proves denial handling, not native qWAVE resource teardown.
// Reset-before-close ordering remains a source-reviewed lifecycle invariant.
class VitaDscpStreamTestAccess {
public:
    static void injectDeniedMarker(PanadapterStream& stream, std::vector<std::vector<quint16>>& calls,
                                    bool& boundBeforeMark)
    {
        stream.m_dscpApplyForTest = [&stream, &calls, &boundBeforeMark](qintptr descriptor,
                const QHostAddress&, std::initializer_list<quint16> ports) {
            boundBeforeMark = boundBeforeMark && descriptor != -1 && stream.isRunning();
            calls.emplace_back(ports);
            return VitaDscpResult{false, QStringLiteral("injected OS policy denial")};
        };
    }
    static void sendToSink(PanadapterStream& stream, quint16 port, const QByteArray& payload)
    {
        stream.m_radioAddress = QHostAddress::LocalHost;
        stream.m_radioPort = port;
        stream.sendToRadio(payload);
    }
};
} // namespace AetherSDR

using namespace AetherSDR;

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("vita-dscp-stream"));
    if (!profile.isValid()) {
        return 77;
    }
    QCoreApplication app(argc, argv);
    QUdpSocket sink;
    if (!sink.bind(QHostAddress::LocalHost, 0)) {
        std::fprintf(stderr, "SKIP: cannot bind loopback UDP sink\n");
        return 77;
    }
    RadioConnection connection; // Never initialized or connected; no firmware peer.
    PanadapterStream stream;
    stream.init();
    std::vector<std::vector<quint16>> calls;
    bool boundBeforeMark = true;
    VitaDscpStreamTestAccess::injectDeniedMarker(stream, calls, boundBeforeMark);

    for (int path = 0; path < 3; ++path) {
        const bool started = path == 0 ? stream.start(&connection)
            : path == 1 ? stream.rebindToEphemeralPort(&connection)
                        : stream.startWan(QHostAddress::LocalHost, sink.localPort());
        if (!started) {
            // The marker runs only after a successful bind. Its denial must
            // fail the test if startup aborts, even if the socket was closed.
            if (calls.size() == static_cast<std::size_t>(path + 1)) {
                std::fprintf(stderr, "FAIL: QoS denial aborted an already-bound stream\n");
                stream.stop();
                return 1;
            }
            std::fprintf(stderr, "SKIP: cannot bind production UDP socket\n");
            stream.stop();
            return 77;
        }
        if (!stream.isRunning() || calls.size() != static_cast<std::size_t>(path + 1)) {
            std::fprintf(stderr, "FAIL: every bind must request marking and survive its denial\n");
            return 1;
        }
        const QByteArray payload = QByteArrayLiteral("stream after denied EF") + QByteArray::number(path);
        VitaDscpStreamTestAccess::sendToSink(stream, sink.localPort(), payload);
        if (!sink.waitForReadyRead(1000) || sink.receiveDatagram().data() != payload) {
            std::fprintf(stderr, "FAIL: denied marking interrupted the production UDP writer\n");
            return 1;
        }
    }
    stream.stop();
    if (!boundBeforeMark || calls[0] != std::vector<quint16>({4991, 4992})
        || calls[1] != calls[0] || calls[2] != std::vector<quint16>({sink.localPort()})
        || stream.isRunning()) {
        std::fprintf(stderr, "FAIL: bind ordering, LAN endpoints, WAN port, or normal teardown\n");
        return 1;
    }
    std::printf("LAN, collision rebind and WAN keep carrying UDP after marking is denied\n");
    return 0;
}
