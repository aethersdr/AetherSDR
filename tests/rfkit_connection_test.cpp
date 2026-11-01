// Transport-level behaviour of RfkitConnection, against a stub RF-KIT speaking
// HTTP/1.1 on loopback. RfkitProtocol has its own pure-codec test; this covers
// what only exists once there is a live HTTP exchange underneath: the connect
// handshake (GET /info), strictly serialized requests, command bodies and
// verbs, refusal of out-of-range antennas before the wire, a non-RF-KIT peer,
// an invalid address, and loss of the link after repeated transport failures.

#include "TestSettingsProfile.h"

#include "core/RfkitConnection.h"
#include "core/RfkitProtocol.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QPointer>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include <algorithm>
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

bool spin(std::function<bool()> done, int timeoutMs = 5000)
{
    QDeadlineTimer deadline(timeoutMs);
    while (!done() && !deadline.hasExpired()) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return done();
}

// A request body as the amp would read it. QJsonObject writes its keys sorted,
// so compare by value rather than by byte order.
QJsonObject jsonBody(const QByteArray& body)
{
    return QJsonDocument::fromJson(body).object();
}

void spinFor(int ms)
{
    QDeadlineTimer deadline(ms);
    while (!deadline.hasExpired()) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
}

// A stub RF-KIT. Answers "METHOD /path" from a route table, records every
// request, and tracks how many were outstanding at once (a reply is delayed so
// that overlap, if the client produced any, is observable).
class StubAmp : public QObject {
public:
    struct Reply {
        int status{200};
        QByteArray body;
    };

    StubAmp()
    {
        m_server.listen(QHostAddress::LocalHost, 0);
        QObject::connect(&m_server, &QTcpServer::newConnection, this, [this]() {
            while (QTcpSocket* socket = m_server.nextPendingConnection()) {
                m_sockets.append(socket);
                QObject::connect(socket, &QTcpSocket::readyRead, this, [this, socket]() { onReadyRead(socket); });
            }
        });
        resetRoutes();
    }

    bool listening() const { return m_server.isListening(); }
    quint16 port() const { return m_server.serverPort(); }
    void stopListening()
    {
        m_server.close();
        for (QTcpSocket* s : m_sockets) {
            if (s) { s->abort(); }
        }
    }

    void resetRoutes()
    {
        routes.clear();
        routes["GET /info"] = {200, R"({"device":"RFKIT","software_version":{"GUI":1,"controller":2},"custom_device_name":"Test"})"};
        routes["GET /power"] = {200,
            R"({"temperature":{"value":40,"unit":"C"},"voltage":{"value":50,"unit":"V"},)"
            R"("current":{"value":10,"unit":"A"},"forward":{"value":800,"max_value":1500,"unit":"W"},)"
            R"("reflected":{"value":20,"max_value":300,"unit":"W"},"swr":{"value":1.3,"max_value":10,"unit":""}})"};
        routes["GET /data"] = {200, R"({"band":{"value":20,"unit":"m"},"frequency":{"value":14200,"unit":"kHz"},"status":"OK"})"};
        routes["GET /tuner"] = {200, R"({"mode":"AUTO","setup":"CL"})"};
        routes["GET /operate-mode"] = {200, R"({"operate_mode":"STANDBY"})"};
        routes["GET /operational-interface"] = {200, R"({"operational_interface":"CAT"})"};
        routes["GET /antennas"] = {200,
            R"({"antennas":[{"type":"INTERNAL","number":1,"state":"ACTIVE"},{"type":"INTERNAL","number":2,"state":"AVAILABLE"}]})"};
        routes["PUT /operate-mode"] = {200, R"({"operate_mode":"OPERATE"})"};
        routes["PUT /antennas/active"] = {200, R"({"type":"INTERNAL","number":2})"};
        routes["POST /error/reset"] = {200, "{}"};
    }

    int count(const QString& key) const
    {
        int n = 0;
        for (const QString& r : requests) { if (r == key) { ++n; } }
        return n;
    }

    QMap<QString, Reply> routes;
    QStringList requests;           // "METHOD /path" in arrival order
    QMap<QString, QByteArray> bodies;  // last body per "METHOD /path"
    int maxOutstanding{0};
    int replyDelayMs{15};
    bool truncateReplies{false};  // promise more body than is sent, then hang up

private:
    void onReadyRead(QTcpSocket* socket)
    {
        QByteArray& buf = m_buffers[socket];
        buf += socket->readAll();
        for (;;) {
            const int headerEnd = buf.indexOf("\r\n\r\n");
            if (headerEnd < 0) { return; }
            const QByteArray head = buf.left(headerEnd);
            int contentLength = 0;
            for (const QByteArray& line : head.split('\n')) {
                if (line.toLower().startsWith("content-length:")) {
                    contentLength = line.mid(15).trimmed().toInt();
                }
            }
            if (buf.size() < headerEnd + 4 + contentLength) { return; }
            const QByteArray body = buf.mid(headerEnd + 4, contentLength);
            buf.remove(0, headerEnd + 4 + contentLength);

            const QList<QByteArray> first = head.left(head.indexOf('\r')).split(' ');
            const QString key = QString::fromLatin1(first.value(0) + ' ' + first.value(1));
            requests.append(key);
            bodies[key] = body;
            ++m_outstanding;
            maxOutstanding = std::max(maxOutstanding, m_outstanding);

            const Reply reply = routes.value(key, Reply{404, R"({"status":"error","reason":"no such endpoint"})"});
            const QPointer<QTcpSocket> guard(socket);
            QTimer::singleShot(replyDelayMs, this, [this, guard, reply]() {
                --m_outstanding;
                if (!guard) { return; }
                const qsizetype promised = reply.body.size() + (truncateReplies ? 64 : 0);
                QByteArray out = "HTTP/1.1 " + QByteArray::number(reply.status) + " X\r\n"
                    "Content-Type: application/json\r\nContent-Length: "
                    + QByteArray::number(promised) + "\r\n\r\n" + reply.body;
                guard->write(out);
                if (truncateReplies) {
                    guard->disconnectFromHost();
                }
            });
        }
    }

    QTcpServer m_server;
    QList<QPointer<QTcpSocket>> m_sockets;
    QMap<QTcpSocket*, QByteArray> m_buffers;
    int m_outstanding{0};
};

}  // namespace

int main(int argc, char** argv)
{
    // Before QCoreApplication and before anything can touch AppSettings --
    // LogManager reaches for it on the first qCWarning. Per the CMake settings
    // contract in AGENTS.md.
    TestSettingsProfile profile(QStringLiteral("rfkit-connection-test"));

    QCoreApplication app(argc, argv);

    // Fail fast rather than burn the timeout when loopback cannot be bound
    // (SKIP_RETURN_CODE 77 in tests.cmake).
    {
        StubAmp probe;
        if (!probe.listening()) {
            std::printf("[SKIP] cannot bind a loopback TCP port\n");
            return 77;
        }
    }

    // ── Handshake, initial refresh, polling, serialization ────────────────
    {
        StubAmp amp;
        RfkitConnection conn;
        QSignalSpy connectedSpy(&conn, &RfkitConnection::connected);
        QSignalSpy statusSpy(&conn, &RfkitConnection::statusUpdated);

        conn.connectNetwork(QStringLiteral("127.0.0.1"), amp.port());
        report("connects once GET /info returns an RF-KIT document",
               spin([&] { return connectedSpy.count() > 0; }) && conn.isConnected());
        report("the first request is the GET /info handshake",
               !amp.requests.isEmpty() && amp.requests.first() == "GET /info");
        report("description() names the target", conn.description() == QString("127.0.0.1:%1").arg(amp.port()));

        report("the initial refresh fills the snapshot",
               spin([&] {
                   if (statusSpy.isEmpty()) { return false; }
                   const auto s = statusSpy.last().at(0).value<Rfkit::Status>();
                   return s.info && s.power && s.data && s.tuner && s.operate && s.interfaceState && s.antennas;
               }));
        if (!statusSpy.isEmpty()) {
            const auto s = statusSpy.last().at(0).value<Rfkit::Status>();
            report("the snapshot carries decoded values",
                   s.info && s.info->customName == "Test" && s.power && s.power->forward.value == 800
                       && s.operate && *s.operate == Rfkit::OperateMode::Standby
                       && s.antennas && s.antennas->size() == 2);
        }

        report("polling continues after the initial refresh",
               spin([&] { return amp.count("GET /power") >= 4; }));
        report("the slow endpoints are polled too",
               spin([&] { return amp.count("GET /tuner") >= 2 && amp.count("GET /antennas") >= 2; }));
        report("requests are strictly serialized: never two outstanding at once",
               amp.maxOutstanding == 1);
        conn.disconnect();
    }

    // ── Commands: verb, path, body ─────────────────────────────────────────
    {
        StubAmp amp;
        RfkitConnection conn;
        QSignalSpy connectedSpy(&conn, &RfkitConnection::connected);
        QSignalSpy statusSpy(&conn, &RfkitConnection::statusUpdated);
        conn.connectNetwork(QStringLiteral("127.0.0.1"), amp.port());
        spin([&] { return connectedSpy.count() > 0; });
        spinFor(300);

        conn.setOperate(true);
        report("setOperate(true) sends PUT /operate-mode",
               spin([&] { return amp.count("PUT /operate-mode") == 1; }));
        report("the OPERATE body is exact",
               amp.bodies.value("PUT /operate-mode") == QByteArray(R"({"operate_mode":"OPERATE"})"));
        report("the reply to the PUT updates the snapshot",
               spin([&] {
                   const auto s = statusSpy.last().at(0).value<Rfkit::Status>();
                   return s.operate && *s.operate == Rfkit::OperateMode::Operate;
               }));

        conn.setOperate(false);
        spin([&] { return amp.count("PUT /operate-mode") == 2; });
        report("the STANDBY body is exact",
               amp.bodies.value("PUT /operate-mode") == QByteArray(R"({"operate_mode":"STANDBY"})"));

        const int antennaReads = amp.count("GET /antennas");
        conn.selectAntenna({Rfkit::AntennaType::Internal, 2});
        report("selectAntenna sends PUT /antennas/active",
               spin([&] { return amp.count("PUT /antennas/active") == 1; }));
        report("the antenna body is exact",
               jsonBody(amp.bodies.value("PUT /antennas/active"))
                   == QJsonObject{{QStringLiteral("type"), QStringLiteral("INTERNAL")}, {QStringLiteral("number"), 2}});
        report("an antenna change re-reads the antenna list",
               spin([&] { return amp.count("GET /antennas") > antennaReads; }));

        // Out of range: refused before the wire.
        conn.selectAntenna({Rfkit::AntennaType::Internal, 9});
        conn.selectAntenna({Rfkit::AntennaType::External, 1});
        spinFor(300);
        report("out-of-range antennas never reach the wire",
               amp.count("PUT /antennas/active") == 1);

        // The external selection is sent without a number.
        conn.selectAntenna({Rfkit::AntennaType::External, 0});
        report("the external antenna is selected with a number-less body",
               spin([&] { return amp.count("PUT /antennas/active") == 2; })
                   && amp.bodies.value("PUT /antennas/active") == QByteArray(R"({"type":"EXTERNAL"})"));

        conn.resetError();
        report("resetError sends POST /error/reset",
               spin([&] { return amp.count("POST /error/reset") == 1; }));
        const int dataReads = amp.count("GET /data");
        report("resetting an error re-reads state it can have changed",
               spin([&] { return amp.count("GET /data") > dataReads; }));

        report("every command still honored serialization", amp.maxOutstanding == 1);
        conn.disconnect();
    }

    // ── Commands queued behind a slow reply reach the amp in click order ──
    {
        StubAmp amp;
        RfkitConnection conn;
        QSignalSpy connectedSpy(&conn, &RfkitConnection::connected);
        conn.connectNetwork(QStringLiteral("127.0.0.1"), amp.port());
        spin([&] { return connectedSpy.count() > 0; });
        spinFor(300);

        amp.replyDelayMs = 300;
        const auto seen = amp.requests.size();
        spin([&] { return amp.requests.size() > seen; });  // the stub now holds one request
        conn.selectAntenna({Rfkit::AntennaType::Internal, 2});
        conn.selectAntenna({Rfkit::AntennaType::Internal, 3});
        report("two queued antenna commands both reach the wire",
               spin([&] { return amp.count("PUT /antennas/active") == 2; }));
        report("the last antenna clicked is the last one sent",
               jsonBody(amp.bodies.value("PUT /antennas/active"))
                   == QJsonObject{{QStringLiteral("type"), QStringLiteral("INTERNAL")}, {QStringLiteral("number"), 3}});
        conn.disconnect();
    }

    // ── An accepted switch re-reads the list even if its reply won't parse ──
    {
        StubAmp amp;
        amp.routes["PUT /antennas/active"] = {200, R"({"status":"ok"})"};
        RfkitConnection conn;
        QSignalSpy connectedSpy(&conn, &RfkitConnection::connected);
        conn.connectNetwork(QStringLiteral("127.0.0.1"), amp.port());
        spin([&] { return connectedSpy.count() > 0; });
        spinFor(300);

        // With the re-read queued, GET /antennas is among the next four
        // requests: the PUT can only sit ahead of the rest of one poll round.
        // Without it, that happens only when the slow-poll rotation lands on
        // /antennas, so four clicks a round or more apart cannot all pass by luck.
        bool everyClickReread = true;
        for (int click = 0; click < 4; ++click) {
            const int puts = amp.count("PUT /antennas/active");
            conn.selectAntenna({Rfkit::AntennaType::Internal, 2});
            spin([&] { return amp.count("PUT /antennas/active") > puts; });
            const qsizetype at = amp.requests.lastIndexOf(QStringLiteral("PUT /antennas/active"));
            spin([&] { return amp.requests.size() > at + 4; });
            everyClickReread = everyClickReread
                && amp.requests.mid(at + 1, 4).contains(QStringLiteral("GET /antennas"));
            spinFor(60 * (click + 1));
        }
        report("an accepted antenna switch re-reads the list even when its reply is unparseable",
               everyClickReread);
        conn.disconnect();
    }

    // ── A link that keeps dying mid-body is declared lost ─────────────────
    {
        StubAmp amp;
        RfkitConnection conn;
        QSignalSpy connectedSpy(&conn, &RfkitConnection::connected);
        QSignalSpy disconnectedSpy(&conn, &RfkitConnection::disconnected);
        conn.connectNetwork(QStringLiteral("127.0.0.1"), amp.port());
        spin([&] { return connectedSpy.count() > 0; });
        spinFor(300);

        amp.truncateReplies = true;  // 200 OK headers, then the transfer breaks
        report("replies that break after the headers count as link loss",
               spin([&] { return disconnectedSpy.count() > 0; }, 8000));
        conn.disconnect();
    }

    // ── A refused command (409) does not drop the link ────────────────────
    {
        StubAmp amp;
        amp.routes["PUT /operate-mode"] = {409, R"({"status":"error","reason":"Function currently disabled"})"};
        RfkitConnection conn;
        QSignalSpy connectedSpy(&conn, &RfkitConnection::connected);
        QSignalSpy disconnectedSpy(&conn, &RfkitConnection::disconnected);
        conn.connectNetwork(QStringLiteral("127.0.0.1"), amp.port());
        spin([&] { return connectedSpy.count() > 0; });
        spinFor(200);

        conn.setOperate(true);
        spin([&] { return amp.count("PUT /operate-mode") == 1; });
        spinFor(500);
        report("a 409 on a command is logged and the link stays up",
               conn.isConnected() && disconnectedSpy.isEmpty());
        report("polling continues after a refused command", amp.count("GET /power") >= 2);
        conn.disconnect();
    }

    // ── A peer that is not an RF-KIT ───────────────────────────────────────
    {
        StubAmp amp;
        amp.routes["GET /info"] = {404, "not found"};
        RfkitConnection conn;
        QSignalSpy connectedSpy(&conn, &RfkitConnection::connected);
        QSignalSpy failedSpy(&conn, &RfkitConnection::connectionFailed);
        conn.connectNetwork(QStringLiteral("127.0.0.1"), amp.port());
        report("a 404 on the handshake fails the connection",
               spin([&] { return failedSpy.count() > 0; }) && connectedSpy.isEmpty() && !conn.isConnected());
        report("no poll traffic follows a failed handshake", amp.count("GET /power") == 0);
    }
    {
        StubAmp amp;
        amp.routes["GET /info"] = {200, "<html>router login</html>"};
        RfkitConnection conn;
        QSignalSpy connectedSpy(&conn, &RfkitConnection::connected);
        QSignalSpy failedSpy(&conn, &RfkitConnection::connectionFailed);
        conn.connectNetwork(QStringLiteral("127.0.0.1"), amp.port());
        report("a 200 that is not an RF-KIT document fails the connection",
               spin([&] { return failedSpy.count() > 0; }) && connectedSpy.isEmpty() && !conn.isConnected());
    }
    {
        // Principle VII: a peer that streams more than any endpoint can send.
        StubAmp amp;
        amp.routes["GET /info"] = {200, QByteArray(Rfkit::kMaxBodyBytes * 3, 'x')};
        RfkitConnection conn;
        QSignalSpy connectedSpy(&conn, &RfkitConnection::connected);
        QSignalSpy failedSpy(&conn, &RfkitConnection::connectionFailed);
        conn.connectNetwork(QStringLiteral("127.0.0.1"), amp.port());
        report("an oversized response fails the connection",
               spin([&] { return failedSpy.count() > 0; }) && connectedSpy.isEmpty());
    }

    // ── Invalid or unreachable addresses ───────────────────────────────────
    {
        RfkitConnection conn;
        QSignalSpy failedSpy(&conn, &RfkitConnection::connectionFailed);
        conn.connectNetwork(QString(), 8080);
        report("an empty address is refused immediately", failedSpy.count() == 1 && !conn.isConnected());
        conn.connectNetwork(QStringLiteral("host/with/slashes"), 8080);
        report("an address that is not a bare host is refused", failedSpy.count() == 2);
        conn.connectNetwork(QStringLiteral("user@host"), 8080);
        report("an address with userinfo is refused", failedSpy.count() == 3);
        conn.connectNetwork(QStringLiteral("127.0.0.1"), 0);
        report("port 0 is refused", failedSpy.count() == 4);
    }
    {
        // Nothing listening: a refused connection reports failure, not a hang.
        quint16 closedPort = 0;
        {
            QTcpServer probe;
            probe.listen(QHostAddress::LocalHost, 0);
            closedPort = probe.serverPort();
        }
        RfkitConnection conn;
        QSignalSpy failedSpy(&conn, &RfkitConnection::connectionFailed);
        conn.connectNetwork(QStringLiteral("127.0.0.1"), closedPort);
        report("a connection refused is reported as a failure",
               spin([&] { return failedSpy.count() > 0; }) && !conn.isConnected());
    }

    // ── Loss of the link ────────────────────────────────────────────────────
    {
        StubAmp amp;
        RfkitConnection conn;
        QSignalSpy connectedSpy(&conn, &RfkitConnection::connected);
        QSignalSpy disconnectedSpy(&conn, &RfkitConnection::disconnected);
        conn.connectNetwork(QStringLiteral("127.0.0.1"), amp.port());
        spin([&] { return connectedSpy.count() > 0; });
        spinFor(300);

        amp.stopListening();
        report("repeated transport failures take the link down (disconnected emitted once)",
               spin([&] { return disconnectedSpy.count() > 0; }, 8000));
        spinFor(300);
        report("disconnected is emitted exactly once", disconnectedSpy.count() == 1 && !conn.isConnected());
    }

    // ── HTTP error statuses are not transport failures ─────────────────────
    {
        StubAmp amp;
        RfkitConnection conn;
        conn.setAutoReconnect(true);
        QSignalSpy connectedSpy(&conn, &RfkitConnection::connected);
        QSignalSpy disconnectedSpy(&conn, &RfkitConnection::disconnected);
        conn.connectNetwork(QStringLiteral("127.0.0.1"), amp.port());
        spin([&] { return connectedSpy.count() > 0; });
        // The device answers every poll with a 500. It is reachable and
        // talking HTTP, so the link must not be declared down.
        amp.routes["GET /power"] = {500, "{}"};
        spinFor(800);
        report("HTTP error statuses are not transport failures: the link stays up",
               conn.isConnected() && disconnectedSpy.isEmpty());
        conn.disconnect();
    }

    // ── Deliberate disconnect ───────────────────────────────────────────────
    {
        StubAmp amp;
        RfkitConnection conn;
        conn.setAutoReconnect(true);
        QSignalSpy connectedSpy(&conn, &RfkitConnection::connected);
        QSignalSpy disconnectedSpy(&conn, &RfkitConnection::disconnected);
        conn.connectNetwork(QStringLiteral("127.0.0.1"), amp.port());
        spin([&] { return connectedSpy.count() > 0; });

        conn.disconnect();
        report("disconnect() emits disconnected() itself", disconnectedSpy.count() == 1);
        report("the connection reports itself down", !conn.isConnected());

        // A request already written before the disconnect can still be read by
        // the stub afterwards; let it land before counting.
        spinFor(300);
        const int before = amp.requests.size();
        conn.setOperate(true);
        conn.resetError();
        conn.selectAntenna({Rfkit::AntennaType::Internal, 1});
        spinFor(500);
        report("commands after a disconnect are dropped, not queued", amp.requests.size() == before);
        report("a deliberate disconnect does not auto-reconnect", connectedSpy.count() == 1 && !conn.isConnected());
    }

    // ── Reconnecting over a live link ───────────────────────────────────────
    {
        StubAmp first;
        StubAmp second;
        RfkitConnection conn;
        QSignalSpy connectedSpy(&conn, &RfkitConnection::connected);
        QSignalSpy disconnectedSpy(&conn, &RfkitConnection::disconnected);
        conn.connectNetwork(QStringLiteral("127.0.0.1"), first.port());
        spin([&] { return connectedSpy.count() > 0; });
        conn.connectNetwork(QStringLiteral("127.0.0.1"), second.port());
        report("connecting to a new target tears down the old session first",
               disconnectedSpy.count() == 1);
        report("and the new session comes up",
               spin([&] { return connectedSpy.count() == 2; }) && conn.isConnected());
        const int firstBefore = first.requests.size();
        spinFor(500);
        report("the old target is no longer polled", first.requests.size() == firstBefore);
        conn.disconnect();
    }

    std::printf("\n%d RF-KIT connection test(s) failed.\n", g_failed);
    return g_failed == 0 ? 0 : 1;
}
