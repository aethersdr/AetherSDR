// Transport-level behaviour of Kpa1500Connection against a stub amp on
// loopback: the UDP link coming up on the first reply and going down on
// silence, readback-confirmed control under datagram loss, per-datagram
// framing, the never-resent tune, keying refused on UDP, and the TCP path.
//
// Timing follows Kpa1500Connection's private constants (kUdpSilenceMs 3000,
// kControlRetryMs 600, kMaxControlAttempts 5, kConnectTimeoutMs 8000).

#include "TestSettingsProfile.h"

#include "core/Kpa1500Connection.h"
#include "core/Kpa1500Protocol.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QHostAddress>
#include <QNetworkDatagram>
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

// A stub KPA1500 serving the command set over UDP (and optionally TCP) on
// loopback, answering queries from its own state the way the amp does.
class StubAmp : public QObject {
public:
    StubAmp()
    {
        m_udp.bind(QHostAddress::LocalHost, 0);
        QObject::connect(&m_udp, &QUdpSocket::readyRead, this, [this]() {
            while (m_udp.hasPendingDatagrams()) {
                const QNetworkDatagram d = m_udp.receiveDatagram();
                handleUdp(d);
            }
        });
        m_tcp.listen(QHostAddress::LocalHost, 0);
        QObject::connect(&m_tcp, &QTcpServer::newConnection, this, [this]() {
            m_client = m_tcp.nextPendingConnection();
            QObject::connect(m_client, &QTcpSocket::readyRead, this, [this]() {
                m_tcpBuf += m_client->readAll();
                int end = 0;
                while ((end = m_tcpBuf.indexOf(';')) >= 0) {
                    const QByteArray frame = m_tcpBuf.left(end + 1);
                    m_tcpBuf.remove(0, end + 1);
                    const QByteArray reply = handleFrame(frame);
                    if (!reply.isEmpty()) {
                        m_client->write(reply);
                    }
                }
            });
        });
    }

    quint16 udpPort() const { return m_udp.localPort(); }
    quint16 tcpPort() const { return m_tcp.serverPort(); }

    // Behaviour knobs.
    bool silent = false;            // receive but never reply
    bool ignoreAtuSet = false;      // accept ^AIn; but never change state
    bool tuneStarts = true;         // ^FT; puts the amp into tuning
    int dropNextSets = 0;           // drop this many incoming SET datagrams

    // State.
    bool operate = false;
    bool atuInline = false;
    int antenna = 1;
    int fault = 0;
    bool tuning = false;
    int forward = 0;
    bool powerOn = true;
    int fanMin = 1;
    int frequency = 0;

    // Observations.
    QList<QByteArray> received;
    int count(const QByteArray& frame) const { return received.count(frame); }

    void sendRawDatagram(const QByteArray& bytes)
    {
        m_udp.writeDatagram(bytes, m_lastPeer, m_lastPeerPort);
    }

private:
    void handleUdp(const QNetworkDatagram& d)
    {
        m_lastPeer = d.senderAddress();
        m_lastPeerPort = static_cast<quint16>(d.senderPort());
        const QByteArray frame = d.data();
        const bool isSet = frame.size() > 4 && !frame.startsWith("^PW") && frame != "^TQ;";
        if (isSet && dropNextSets > 0) {
            --dropNextSets;
            return;
        }
        const QByteArray reply = handleFrame(frame);
        if (!reply.isEmpty() && !silent) {
            m_udp.writeDatagram(reply, m_lastPeer, m_lastPeerPort);
        }
    }

    QByteArray handleFrame(const QByteArray& frame)
    {
        received.append(frame);
        const QByteArray body = frame.mid(1, frame.size() - 2);  // strip ^ and ;
        auto num = [](int v, int width) {
            return QByteArray::number(v).rightJustified(width, '0');
        };
        // Answer while asleep, as the reference documents.
        if (body == "ON")  { return powerOn ? QByteArray("^ON1;") : QByteArray("^ON0;"); }
        if (body == "RV")  { return QByteArray("^RV03.10;"); }
        if (body == "SN")  { return QByteArray("^SN00022;"); }
        if (body == "ON1" || body == "ON0") { powerOn = body == "ON1"; return {}; }
        if (!powerOn) { return {}; }
        if (body == "PWI") { return QByteArray("^PWI0040;"); }
        if (body == "VI")  { return QByteArray("^VI513 042;"); }
        if (body == "FC")  { return "^FC" + QByteArray::number(fanMin) + ";"; }
        if (body == "FS")  { return "^FS" + QByteArray::number(fanMin) + ";"; }
        if (body == "AS")  { return QByteArray("^AS00;"); }
        if (body == "DA")  { return QByteArray("^DA14074 AN1 Side ANT 340 nH (L06) 0 pf (C00) SWR 1.2;"); }
        if (body == "FR")  { return "^FR" + num(frequency, 5) + ";"; }
        if (body.size() == 3 && body.startsWith("FC")) { fanMin = body.right(1).toInt(); return {}; }
        if (body.size() == 7 && body.startsWith("FR")) { frequency = body.mid(2).toInt(); return {}; }
        if (body == "PWF") { return "^PWF" + num(forward, 4) + ";"; }
        if (body == "PWR") { return "^PWR" + num(forward / 50, 4) + ";"; }
        if (body == "SW")  { return QByteArray("^SW012;"); }
        if (body == "TM")  { return QByteArray("^TM041;"); }
        if (body == "BN")  { return QByteArray("^BN05;"); }
        if (body == "AM")  { return QByteArray("^AMI;"); }
        if (body == "TP")  { return tuning ? QByteArray("^TP1;") : QByteArray("^TP0;"); }
        if (body == "OS")  { return operate ? QByteArray("^OS1;") : QByteArray("^OS0;"); }
        if (body == "AI")  { return atuInline ? QByteArray("^AI1;") : QByteArray("^AI0;"); }
        if (body == "AN")  { return "^AN" + QByteArray::number(antenna) + ";"; }
        if (body == "FL")  { return "^FL" + num(fault, 2) + ";"; }
        if (body == "OS1") { operate = true; return {}; }
        if (body == "OS0") { operate = false; return {}; }
        if (body == "AI1" || body == "AI0") {
            if (!ignoreAtuSet) { atuInline = body == "AI1"; }
            return {};
        }
        if (body == "AN1" || body == "AN2") { antenna = body.right(1).toInt(); return {}; }
        if (body == "FLC") { fault = 0; return {}; }
        if (body == "FT")  { tuning = tuneStarts; return {}; }
        if (body == "FE")  { tuning = false; return {}; }
        return {};
    }

    QUdpSocket m_udp;
    QHostAddress m_lastPeer;
    quint16 m_lastPeerPort{0};
    QTcpServer m_tcp;
    QTcpSocket* m_client{nullptr};
    QByteArray m_tcpBuf;
};

void connectUdp(Kpa1500Connection& conn, StubAmp& amp)
{
    conn.setTransport(Kpa1500::Transport::Udp);
    conn.connectNetwork(QStringLiteral("127.0.0.1"), amp.udpPort());
}

}  // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("kpa1500-connection-test"));
    QCoreApplication app(argc, argv);

    // ── UDP: link up on first reply, telemetry flows ───────────────────────
    {
        StubAmp amp;
        amp.forward = 1200;
        Kpa1500Connection conn;
        QSignalSpy connectedSpy(&conn, &Kpa1500Connection::connected);
        Kpa1500::Status last;
        QObject::connect(&conn, &Kpa1500Connection::statusUpdated,
                         [&last](const Kpa1500::Status& s) { last = s; });
        connectUdp(conn, amp);

        report("UDP: connected() fires once the amp answers",
               spin([&] { return connectedSpy.count() == 1; }));
        report("UDP: description names the transport",
               conn.description().endsWith(QStringLiteral("(UDP)")));
        report("UDP: forward power arrives",
               spin([&] { return last.forwardWatts && *last.forwardWatts == 1200.0f; }));
        report("UDP: SWR arrives in tenths",
               spin([&] { return last.swr && qFuzzyCompare(*last.swr, 1.2f); }));
        report("UDP: slow readbacks arrive (band, ATU mode)",
               spin([&] { return last.band && *last.band == 5 && last.atuMode; }));
        report("UDP: every query goes out as its own datagram",
               std::all_of(amp.received.cbegin(), amp.received.cend(), [](const QByteArray& f) {
                   return f.startsWith('^') && f.endsWith(';') && f.count(';') == 1;
               }));
    }

    // ── UDP: an amp that never answers fails the connect ───────────────────
    {
        StubAmp amp;
        amp.silent = true;
        Kpa1500Connection conn;
        QSignalSpy connectedSpy(&conn, &Kpa1500Connection::connected);
        QSignalSpy failedSpy(&conn, &Kpa1500Connection::connectionFailed);
        connectUdp(conn, amp);
        report("UDP: no reply -> connectionFailed within the connect timeout",
               spin([&] { return failedSpy.count() == 1; }, 10000));
        report("UDP: no reply -> never reported connected",
               connectedSpy.isEmpty() && !conn.isConnected());
        report("UDP: no-reply error names the port",
               !failedSpy.isEmpty()
                   && failedSpy.first().first().toString().contains(
                       QString::number(amp.udpPort())));
    }

    // ── UDP: control confirmed despite lost SET datagrams ──────────────────
    {
        StubAmp amp;
        Kpa1500Connection conn;
        QSignalSpy connectedSpy(&conn, &Kpa1500Connection::connected);
        QSignalSpy unconfirmedSpy(&conn, &Kpa1500Connection::controlUnconfirmed);
        connectUdp(conn, amp);
        spin([&] { return connectedSpy.count() == 1; });

        amp.dropNextSets = 2;
        conn.setOperate(true);
        report("UDP: OPERATE lands after two lost datagrams",
               spin([&] { return amp.operate; }, 5000));
        spinFor(1500);
        report("UDP: a confirmed OPERATE raises no notice", unconfirmedSpy.isEmpty());
        const int setsAfterConfirm = amp.count("^OS1;");
        spinFor(1500);
        report("UDP: OPERATE stops being resent once confirmed",
               amp.count("^OS1;") == setsAfterConfirm);

        conn.selectAntenna(2);
        report("UDP: antenna select confirmed", spin([&] { return amp.antenna == 2; }));
        amp.fault = 0x21;
        conn.clearFault();
        report("UDP: fault clear confirmed", spin([&] { return amp.fault == 0; }));
        spinFor(800);
        report("UDP: no notices for confirmed controls", unconfirmedSpy.isEmpty());
    }

    // ── UDP: a control the amp never applies is reported ───────────────────
    {
        StubAmp amp;
        amp.ignoreAtuSet = true;
        Kpa1500Connection conn;
        QSignalSpy connectedSpy(&conn, &Kpa1500Connection::connected);
        QSignalSpy unconfirmedSpy(&conn, &Kpa1500Connection::controlUnconfirmed);
        connectUdp(conn, amp);
        spin([&] { return connectedSpy.count() == 1; });

        conn.setAtuInline(true);
        report("UDP: unapplied ATU in-line -> controlUnconfirmed",
               spin([&] { return unconfirmedSpy.count() == 1; }, 6000));
        report("UDP: notice names the control",
               !unconfirmedSpy.isEmpty()
                   && unconfirmedSpy.first().first().toString() == QStringLiteral("ATU in-line"));
        report("UDP: the SET is retried up to the attempt limit, no further",
               amp.count("^AI1;") == 5);
    }

    // ── UDP: the latest request supersedes a pending one ───────────────────
    {
        StubAmp amp;
        Kpa1500Connection conn;
        QSignalSpy connectedSpy(&conn, &Kpa1500Connection::connected);
        QSignalSpy unconfirmedSpy(&conn, &Kpa1500Connection::controlUnconfirmed);
        connectUdp(conn, amp);
        spin([&] { return connectedSpy.count() == 1; });

        amp.dropNextSets = 1;
        conn.setOperate(true);
        conn.setOperate(false);
        spinFor(3500);
        report("UDP: last OPERATE/STANDBY request wins", !amp.operate);
        report("UDP: the superseded request raises no notice", unconfirmedSpy.isEmpty());
    }

    // ── UDP: ^FT is never resent ───────────────────────────────────────────
    {
        StubAmp amp;
        amp.tuneStarts = false;
        Kpa1500Connection conn;
        QSignalSpy connectedSpy(&conn, &Kpa1500Connection::connected);
        QSignalSpy unconfirmedSpy(&conn, &Kpa1500Connection::controlUnconfirmed);
        connectUdp(conn, amp);
        spin([&] { return connectedSpy.count() == 1; });

        conn.startTune();
        report("UDP: unconfirmed tune -> notice",
               spin([&] { return unconfirmedSpy.count() == 1; }, 6000));
        report("UDP: ^FT sent exactly once", amp.count("^FT;") == 1);
    }

    // ── UDP: per-datagram framing ──────────────────────────────────────────
    {
        StubAmp amp;
        Kpa1500Connection conn;
        QSignalSpy connectedSpy(&conn, &Kpa1500Connection::connected);
        Kpa1500::Status last;
        QObject::connect(&conn, &Kpa1500Connection::statusUpdated,
                         [&last](const Kpa1500::Status& s) { last = s; });
        connectUdp(conn, amp);
        spin([&] { return connectedSpy.count() == 1; });
        spinFor(300);

        amp.silent = true;  // only the hand-made datagrams below reach the client
        spinFor(300);
        amp.sendRawDatagram("^TM0");
        amp.sendRawDatagram("99;");
        spinFor(300);
        report("UDP: a frame split across datagrams is not spliced",
               last.tempC && *last.tempC == 41);
        amp.sendRawDatagram("junk^TM050;^BN07;");
        report("UDP: several frames in one datagram all apply",
               spin([&] { return last.tempC == 50 && last.band == 7; }, 2000));
    }

    // ── UDP: silence takes the link down; auto-reconnect brings it back ────
    {
        StubAmp amp;
        Kpa1500Connection conn;
        conn.setAutoReconnect(true);
        QSignalSpy connectedSpy(&conn, &Kpa1500Connection::connected);
        QSignalSpy disconnectedSpy(&conn, &Kpa1500Connection::disconnected);
        connectUdp(conn, amp);
        spin([&] { return connectedSpy.count() == 1; });

        amp.silent = true;
        report("UDP: silence -> disconnected()",
               spin([&] { return disconnectedSpy.count() == 1; }, 6000));
        report("UDP: silence -> isConnected() false", !conn.isConnected());
        amp.silent = false;
        report("UDP: auto-reconnect re-establishes the link",
               spin([&] { return connectedSpy.count() == 2; }, 10000));
    }

    // ── UDP: network keying refused ────────────────────────────────────────
    {
        StubAmp amp;
        Kpa1500Connection conn;
        QSignalSpy connectedSpy(&conn, &Kpa1500Connection::connected);
        connectUdp(conn, amp);
        spin([&] { return connectedSpy.count() == 1; });
        conn.key();
        spinFor(600);
        bool anyTx = false;
        for (const QByteArray& f : amp.received) {
            if (f.startsWith("^TX")) {
                anyTx = true;
            }
        }
        report("UDP: key() puts no ^TX on the wire", !anyTx);
    }

    // ── UDP: disconnect() is clean and reports once ────────────────────────
    {
        StubAmp amp;
        Kpa1500Connection conn;
        QSignalSpy connectedSpy(&conn, &Kpa1500Connection::connected);
        QSignalSpy disconnectedSpy(&conn, &Kpa1500Connection::disconnected);
        connectUdp(conn, amp);
        spin([&] { return connectedSpy.count() == 1; });
        conn.disconnect();
        const int before = amp.received.size();
        spinFor(800);
        report("UDP: disconnect() emits disconnected() once", disconnectedSpy.count() == 1);
        report("UDP: no polling after disconnect()", amp.received.size() == before);
    }

    // ── Remote-control readouts and controls ───────────────────────────────
    {
        StubAmp amp;
        Kpa1500Connection conn;
        QSignalSpy connectedSpy(&conn, &Kpa1500Connection::connected);
        QSignalSpy disconnectedSpy(&conn, &Kpa1500Connection::disconnected);
        QSignalSpy unconfirmedSpy(&conn, &Kpa1500Connection::controlUnconfirmed);
        Kpa1500::Status last;
        QObject::connect(&conn, &Kpa1500Connection::statusUpdated,
                         [&last](const Kpa1500::Status& s) { last = s; });
        connectUdp(conn, amp);
        spin([&] { return connectedSpy.count() == 1; });

        report("identity: firmware and serial read at connect",
               spin([&] { return last.firmware && last.serialNumber; }));
        report("readouts: drive power, PA volts and amps",
               spin([&] { return last.inputWatts == 40.0f && last.paAmps == 42 && last.paVolts; }));
        report("readouts: fan, ATU summary, power state",
               spin([&] { return last.fanSpeed && last.atuSetting && last.powerOn == true; },
                    6000));

        amp.dropNextSets = 1;
        conn.setFanMinimum(4);
        report("fan minimum set and confirmed despite a lost datagram",
               spin([&] { return amp.fanMin == 4 && last.fanMinimum == 4; }, 5000));

        conn.setPower(false);
        report("power off puts the amp to sleep", spin([&] { return last.powerOn == false; }, 5000));
        amp.received.clear();
        spinFor(3800);
        report("asleep: link stays up past the silence timeout",
               conn.isConnected() && disconnectedSpy.isEmpty());
        report("asleep: only ^ON; is polled",
               !amp.received.isEmpty()
                   && std::all_of(amp.received.cbegin(), amp.received.cend(),
                                  [](const QByteArray& f) { return f == "^ON;"; }));
        conn.setPower(true);
        report("power on wakes the amp and the meters resume",
               spin([&] { return amp.powerOn && amp.count("^PWF;") > 0; }, 5000));
        spinFor(700);
        report("no notices for confirmed remote controls", unconfirmedSpy.isEmpty());
    }

    // ── TX frequency tracking ──────────────────────────────────────────────
    {
        StubAmp amp;
        Kpa1500Connection conn;
        QSignalSpy connectedSpy(&conn, &Kpa1500Connection::connected);
        QSignalSpy unconfirmedSpy(&conn, &Kpa1500Connection::controlUnconfirmed);
        auto frSets = [&amp]() {
            int n = 0;
            for (const QByteArray& f : amp.received) {
                if (f.startsWith("^FR") && f.size() > 4) { ++n; }
            }
            return n;
        };
        connectUdp(conn, amp);
        spin([&] { return connectedSpy.count() == 1; });

        conn.setTxFrequency(14074, false);
        spinFor(700);
        report("tracking off: no ^FR sent", frSets() == 0);

        conn.setFollowTx(true);
        report("tracking on: TX slice frequency reaches the amp",
               spin([&] { return amp.frequency == 14074; }));

        // A knob spin: 20 updates in ~100 ms coalesce, the last one lands.
        const int before = frSets();
        for (int i = 1; i <= 20; ++i) {
            conn.setTxFrequency(14074 + i, false);
            spinFor(5);
        }
        report("tuning: final frequency lands", spin([&] { return amp.frequency == 14094; }));
        report("tuning: updates are coalesced (not one ^FR per step)",
               frSets() - before <= 3);

        conn.setTxFrequency(7100, true);
        spinFor(900);
        report("transmitting: band change is held", amp.frequency == 14094);
        conn.setTxFrequency(7100, false);
        report("transmit ends: held frequency goes out", spin([&] { return amp.frequency == 7100; }));

        conn.setTxFrequency(144200, false);
        spinFor(700);
        report("a slice outside 160-6 m is not tracked", amp.frequency == 7100);

        // A ^FR lost just before keying must not be resent under RF.
        amp.dropNextSets = 1;
        conn.setTxFrequency(21074, false);
        spinFor(40);
        conn.setTxFrequency(21074, true);
        spinFor(1500);
        report("transmitting: an unconfirmed ^FR is not resent", amp.frequency == 7100);
        conn.setTxFrequency(21074, false);
        report("...and is sent once transmit ends", spin([&] { return amp.frequency == 21074; }));
        report("tracking raises no notices", unconfirmedSpy.isEmpty());
    }

    // ── TCP path unchanged ─────────────────────────────────────────────────
    {
        StubAmp amp;
        amp.forward = 900;
        Kpa1500Connection conn;
        QSignalSpy connectedSpy(&conn, &Kpa1500Connection::connected);
        QSignalSpy unconfirmedSpy(&conn, &Kpa1500Connection::controlUnconfirmed);
        Kpa1500::Status last;
        QObject::connect(&conn, &Kpa1500Connection::statusUpdated,
                         [&last](const Kpa1500::Status& s) { last = s; });
        conn.connectNetwork(QStringLiteral("127.0.0.1"), amp.tcpPort());
        report("TCP: connected() by default transport",
               spin([&] { return connectedSpy.count() == 1; }));
        report("TCP: description has no transport suffix",
               !conn.description().contains(QStringLiteral("UDP")));
        report("TCP: forward power arrives",
               spin([&] { return last.forwardWatts && *last.forwardWatts == 900.0f; }));
        conn.setOperate(true);
        report("TCP: OPERATE sent once", spin([&] { return amp.operate; }));
        spinFor(1500);
        report("TCP: OPERATE is not resent", amp.count("^OS1;") == 1);
        report("TCP: no confirmation notices", unconfirmedSpy.isEmpty());
    }

    if (g_failed == 0) {
        std::printf("\nAll KPA1500 connection tests passed.\n");
    } else {
        std::printf("\n%d KPA1500 connection test(s) FAILED.\n", g_failed);
    }
    return g_failed == 0 ? 0 : 1;
}
