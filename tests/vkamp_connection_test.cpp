// Transport-level behaviour of VkampConnection, against a stub amp on
// loopback. VkampProtocol has its own pure-codec test; this covers the parts
// that only exist once there is a live socket underneath — the bypass/voltage
// safety interlock, the reset hold's exclusive claim on the wire, the
// rate limiter, hostname resolution for the UDP telemetry port, and the
// TX-gated telemetry expiry.
//
// Every timing constant here is read off VkampConnection's own private
// constants (kMinCommandIntervalMs 250, kTelemetryStallMs 5000); if those
// move, the waits below move with them.

#include "TestSettingsProfile.h"

#include "core/VkampConnection.h"
#include "core/VkampProtocol.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUdpSocket>

#include <cstdio>
#include <functional>

using namespace AetherSDR;

namespace {

int g_failed = 0;

void report(const char* name, bool ok)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", name);
    if (!ok) {
        ++g_failed;
    }
}

// Spins the event loop until `done()` or the deadline expires. Returns what
// done() last said, so callers can assert on it directly.
bool spin(std::function<bool()> done, int timeoutMs = 5000)
{
    QDeadlineTimer deadline(timeoutMs);
    while (!done() && !deadline.hasExpired()) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return done();
}

void spinFor(int ms)
{
    QDeadlineTimer deadline(ms);
    while (!deadline.hasExpired()) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
}

// A stub VK3AMP: TCP control/status on one port, UDP telemetry on another.
// Records every command byte pair it receives and replies with a status frame,
// exactly as the real amp does (request/response, never spontaneous).
class StubAmp : public QObject {
public:
    StubAmp()
    {
        m_server.listen(QHostAddress::LocalHost, 0);
        QObject::connect(&m_server, &QTcpServer::newConnection, this, [this]() {
            m_client = m_server.nextPendingConnection();
            QObject::connect(m_client, &QTcpSocket::readyRead, this, [this]() {
                m_buf += m_client->readAll();
                while (m_buf.size() >= 2) {
                    const QByteArray cmd = m_buf.left(2);
                    m_buf.remove(0, 2);
                    commands.append(cmd);
                    m_client->write(statusFrame());
                }
            });
        });

        m_udp.bind(QHostAddress::LocalHost, 0);
        QObject::connect(&m_udp, &QUdpSocket::readyRead, this, [this]() {
            while (m_udp.hasPendingDatagrams()) {
                QByteArray datagram;
                datagram.resize(static_cast<int>(m_udp.pendingDatagramSize()));
                QHostAddress from;
                quint16 fromPort = 0;
                m_udp.readDatagram(datagram.data(), datagram.size(), &from, &fromPort);
                ++udpTriggers;
                if (streaming) {
                    m_udp.writeDatagram(telemetryFrame, from, fromPort);
                }
            }
        });
    }

    quint16 tcpPort() const { return m_server.serverPort(); }
    quint16 udpPort() const { return m_udp.localPort(); }

    QByteArray statusFrame() const
    {
        return QStringLiteral("%1,%2,3,%3,0,0,0,%4")
            .arg(temp).arg(volts10).arg(antenna).arg(bypass ? 1 : 0).toLatin1();
    }

    // Pushes an unsolicited status frame, for tests that need the connection's
    // cached status to change without going through a command.
    void pushStatus()
    {
        if (m_client) { m_client->write(statusFrame()); }
    }

    QList<QByteArray> commands;
    int udpTriggers{0};
    bool streaming{false};
    QByteArray telemetryFrame{"401,30,94,50"};

    int temp{23};
    int volts10{577};
    int antenna{1};
    bool bypass{false};

private:
    QTcpServer m_server;
    QUdpSocket m_udp;
    QTcpSocket* m_client{nullptr};
    QByteArray m_buf;
};

}  // namespace

int main(int argc, char** argv)
{
    // Before QCoreApplication and before anything can touch AppSettings --
    // LogManager reaches for it on the first qCWarning. Per the CMake
    // settings contract in AGENTS.md.
    TestSettingsProfile profile(QStringLiteral("vkamp-connection-test"));

    QCoreApplication app(argc, argv);

    // ── Connect by HOST NAME, not a dotted quad ─────────────────────────────
    // Regression guard for the defect this shipped with: the telemetry socket
    // used QHostAddress(host), which parses only literal addresses, so a
    // hostname produced a null address and every datagram was dropped —
    // while connectToHost() resolved the same string happily, leaving the UI
    // showing a healthy connection with telemetry that never arrived. The
    // Peripherals field has no validator, so a hostname is ordinary input.
    {
        StubAmp amp;
        amp.streaming = true;
        VkampConnection conn;

        QSignalSpy connectedSpy(&conn, &VkampConnection::connected);
        QSignalSpy telemetrySpy(&conn, &VkampConnection::telemetryUpdated);
        QSignalSpy statusSpy(&conn, &VkampConnection::statusUpdated);

        conn.connectNetwork(QStringLiteral("localhost"), amp.tcpPort(), amp.udpPort());
        report("connects when the host field holds a name rather than an IP",
               spin([&] { return connectedSpy.count() > 0; }) && conn.isConnected());
        report("TCP status decodes over a hostname connection",
               spin([&] { return statusSpy.count() > 0; }));
        report("UDP telemetry ALSO flows over a hostname connection "
               "(QHostAddress(host) would have dropped every datagram)",
               spin([&] { return telemetrySpy.count() > 0; }));

        if (telemetrySpy.count() > 0) {
            const auto t = telemetrySpy.at(0).at(0).value<Vkamp::Telemetry>();
            report("the telemetry frame decodes end to end",
                   t.output == 401 && t.reflected == 30 && t.current == 94);
        }
        conn.disconnect();
    }

    // ── Telemetry expiry: the amp is silent whenever it isn't transmitting ──
    {
        StubAmp amp;
        amp.streaming = true;
        VkampConnection conn;
        QSignalSpy stalledSpy(&conn, &VkampConnection::telemetryStalled);
        QSignalSpy telemetrySpy(&conn, &VkampConnection::telemetryUpdated);

        conn.connectNetwork(QStringLiteral("127.0.0.1"), amp.tcpPort(), amp.udpPort());
        report("telemetry starts flowing while the amp is 'transmitting'",
               spin([&] { return telemetrySpy.count() > 0; }));
        report("no stall is reported while frames keep arriving",
               stalledSpy.count() == 0);

        // Transmission ends: the amp simply stops answering the trigger.
        amp.streaming = false;
        report("telemetryStalled() fires once the frames stop, so consumers can "
               "expire the last transmit reading instead of displaying it forever",
               spin([&] { return stalledSpy.count() > 0; }, 9000));
        report("the TCP link is still considered healthy through a telemetry stall",
               conn.isConnected());
        conn.disconnect();
    }

    // ── Voltage/bypass interlock (design doc Section 5) ─────────────────────
    {
        StubAmp amp;
        VkampConnection conn;
        QSignalSpy connectedSpy(&conn, &VkampConnection::connected);
        conn.connectNetwork(QStringLiteral("127.0.0.1"), amp.tcpPort(), amp.udpPort());
        spin([&] { return connectedSpy.count() > 0; });

        // Amp reports itself bypassed.
        amp.bypass = true;
        amp.volts10 = 63;  // the ~6.3V standby reading, neither rail target
        amp.pushStatus();
        report("the bypassed status reaches the connection",
               spin([&] { return conn.lastStatus().bypass; }));

        amp.commands.clear();
        conn.setVoltage(true);
        spinFor(400);
        report("a rail command is REFUSED while the amp is bypassed -- nothing "
               "reaches the wire (design doc Section 5: this was observed live "
               "pulling the supply toward ~0V)",
               !amp.commands.contains(QByteArray("41"))
                   && !amp.commands.contains(QByteArray("42")));

        // Amp comes out of bypass; the same command must now go through.
        amp.bypass = false;
        amp.volts10 = 577;
        amp.pushStatus();
        spin([&] { return !conn.lastStatus().bypass; });
        amp.commands.clear();
        conn.setVoltage(true);
        report("the same rail command IS sent once bypass clears",
               spin([&] { return amp.commands.contains(QByteArray("41")); }));
        conn.disconnect();
    }

    // ── Antenna range check ────────────────────────────────────────────────
    {
        StubAmp amp;
        VkampConnection conn;
        QSignalSpy connectedSpy(&conn, &VkampConnection::connected);
        conn.connectNetwork(QStringLiteral("127.0.0.1"), amp.tcpPort(), amp.udpPort());
        spin([&] { return connectedSpy.count() > 0; });

        amp.commands.clear();
        conn.selectAntenna(0);
        conn.selectAntenna(4);
        spinFor(400);
        report("out-of-range antenna ports never reach the wire", amp.commands.isEmpty());

        conn.selectAntenna(2);
        report("a valid antenna port is sent as \"32\"",
               spin([&] { return amp.commands.contains(QByteArray("32")); }));
        conn.disconnect();
    }

    // ── Rate limiter (kMinCommandIntervalMs) ───────────────────────────────
    // A live finding from the companion project: five commands inside ~20ms
    // sagged the reported supply voltage and two silently failed to apply.
    {
        StubAmp amp;
        VkampConnection conn;
        QSignalSpy connectedSpy(&conn, &VkampConnection::connected);
        conn.connectNetwork(QStringLiteral("127.0.0.1"), amp.tcpPort(), amp.udpPort());
        spin([&] { return connectedSpy.count() > 0; });

        spinFor(400);  // clear the connect poll out of the rate-limit window
        amp.commands.clear();
        for (int i = 0; i < 5; ++i) {
            conn.setCoolingOverride(i % 2 == 0);
        }
        spinFor(400);
        report("a five-command burst is throttled to one on the wire",
               amp.commands.size() == 1);

        spinFor(400);  // past kMinCommandIntervalMs
        conn.setCoolingOverride(true);
        report("a command sent after the interval goes through",
               spin([&] { return amp.commands.size() == 2; }));
        conn.disconnect();
    }

    // ── The reset hold owns the wire for its whole duration ────────────────
    {
        StubAmp amp;
        VkampConnection conn;
        QSignalSpy connectedSpy(&conn, &VkampConnection::connected);
        QSignalSpy progressSpy(&conn, &VkampConnection::resetProgress);
        conn.connectNetwork(QStringLiteral("127.0.0.1"), amp.tcpPort(), amp.udpPort());
        spin([&] { return connectedSpy.count() > 0; });
        spinFor(400);
        amp.commands.clear();

        conn.startReset();
        report("startReset() sends the first hold immediately rather than "
               "waiting out an interval",
               spin([&] { return amp.commands.contains(QByteArray("23")); }));
        report("the connection reports itself resetting", conn.isResetting());
        report("resetProgress() is emitted straight away so the UI can show a hold",
               progressSpy.count() > 0);

        // This protocol has no correlation ID (design doc Section 6): nothing
        // else may be in flight. The keepalive poll is gated the same way, but
        // a user click is the case a reviewer can actually reach — the applet
        // did not disable these buttons during a hold.
        //
        // Land the foreign commands in the GAP between two holds: holds go out
        // every kResetIntervalSeconds (1000ms) and sendCommand() independently
        // rate-limits to kMinCommandIntervalMs (250ms), so firing them right
        // after a hold would be swallowed by the rate limiter instead and the
        // assertion below would pass whether or not the reset gate exists.
        spinFor(500);
        conn.setBypass(true);
        spinFor(300);
        conn.setCoolingOverride(true);
        spinFor(300);
        conn.selectAntenna(3);
        spinFor(300);
        bool onlyHolds = true;
        for (const QByteArray& cmd : amp.commands) {
            if (cmd != QByteArray("23")) { onlyHolds = false; }
        }
        report("no foreign command can be injected into the \"23\" hold stream",
               onlyHolds && !amp.commands.isEmpty());

        conn.startReset();
        report("a second startReset() during a hold is a no-op", conn.isResetting());

        // A hold in progress must not survive teardown.
        conn.disconnect();
        report("disconnect() cancels an in-progress reset", !conn.isResetting());

        amp.commands.clear();
        spinFor(1500);
        report("no stray holds are sent after disconnect", amp.commands.isEmpty());
    }

    // ── Deliberate disconnect ──────────────────────────────────────────────
    {
        StubAmp amp;
        VkampConnection conn;
        QSignalSpy connectedSpy(&conn, &VkampConnection::connected);
        QSignalSpy disconnectedSpy(&conn, &VkampConnection::disconnected);
        conn.connectNetwork(QStringLiteral("127.0.0.1"), amp.tcpPort(), amp.udpPort());
        spin([&] { return connectedSpy.count() > 0; });

        conn.disconnect();
        report("disconnect() emits disconnected() itself rather than relying on "
               "the socket's own signal", disconnectedSpy.count() == 1);
        report("the connection reports itself down", !conn.isConnected());

        amp.commands.clear();
        conn.setBypass(true);
        spinFor(300);
        report("commands after a disconnect are dropped, not queued",
               amp.commands.isEmpty());
    }

    std::printf("\n%d VK3AMP connection test(s) failed.\n", g_failed);
    return g_failed == 0 ? 0 : 1;
}
