// GreenHeronModel tests, driven by a stand-in Everyware server on localhost.
//
// Covers the things that only show up over a real socket: records split across
// segments, the keepalive, surviving a dropped connection, and — the one that
// is silently wrong if you get it backwards — lock slots being indexed by
// ANNOUNCEMENT order rather than display order.
//
// No hardware, no network beyond loopback.
//
// Run:  ./build/green_heron_model_test

#include "core/GreenHeronProtocol.h"
#include "models/GreenHeronModel.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>

using namespace AetherSDR;

namespace {

int g_failed = 0;

void report(const char* name, bool ok, const std::string& detail = {})
{
    std::printf("%s %-62s %s\n", ok ? "[ OK ]" : "[FAIL]", name, detail.c_str());
    if (!ok) {
        ++g_failed;
    }
}

// The roster the reference hardware announces — deliberately NOT in sorted
// order: 1, 3, 2, 4. That is the whole point of several tests below.
const QByteArray kRoster =
    QByteArrayLiteral("SWITCHADD\x1f" "1\x1f" "AS-84F-1\x1f" "Beam-15\x1d" "0\x1d" "0\x1d" "false\x1f" "Beam-20\x1d" "0\x1d" "0\x1d" "false\x1f" "OFF\x1d" "0\x1d" "0\x1d" "false\r\n")
    + QByteArrayLiteral("SWITCHADD\x1f" "1\x1f" "AS-84F-3\x1f" "Beam-15\x1d" "0\x1d" "0\x1d" "false\x1f" "Beam-20\x1d" "0\x1d" "0\x1d" "false\x1f" "OFF\x1d" "0\x1d" "0\x1d" "false\r\n")
    + QByteArrayLiteral("SWITCHADD\x1f" "1\x1f" "AS-84F-2\x1f" "Beam-15\x1d" "0\x1d" "0\x1d" "false\x1f" "Beam-20\x1d" "0\x1d" "0\x1d" "false\x1f" "OFF\x1d" "0\x1d" "0\x1d" "false\r\n")
    + QByteArrayLiteral("SWITCHADD\x1f" "1\x1f" "AS-84F-4\x1f" "Beam-15\x1d" "0\x1d" "0\x1d" "false\x1f" "Beam-20\x1d" "0\x1d" "0\x1d" "false\x1f" "OFF\x1d" "0\x1d" "0\x1d" "false\r\n");

const QByteArray kUpdates =
    QByteArrayLiteral("SWITCHUPDATE\x1f" "AS-84F-1\x1f" "OFF\x1f" "0\x1f" "-27\r\n")
    + QByteArrayLiteral("SWITCHUPDATE\x1f" "AS-84F-3\x1f" "Beam-15\x1f" "0\x1f" "-28\r\n");

// The discriminating case from the hardware session: AS-84F-3 holds Beam-15,
// and the device reports it in SLOT 1 — AS-84F-3 is announced second. Sorted
// order would predict slot 2, so this record tells the two orderings apart.
const QByteArray kLocks =
    QByteArrayLiteral("SWITCHLOCKS\x1f" "AS-84F-1\x1f" "OFF\x1f" "Beam-15\x1f" "OFF\x1f" "OFF\r\n");

// The rotator's records. ADD announces it once on connect; POINT carries the
// reported heading and arrives every ~0.97 s on the reference hardware.
const QByteArray kRotorAdd   = QByteArrayLiteral("ADD\x1f" "Rotor\r\n");
const QByteArray kRotorPoint = QByteArrayLiteral("POINT\x1f" "Rotor\x1f" "50.4\x1f" "0\x1f" "0\r\n");

// A stand-in for the Everyware server: accepts one connection, replays a
// script, and records whatever the client sends back.
class FakeDevice : public QObject {
public:
    explicit FakeDevice(QByteArray script, int chunkBytes = 0)
        : m_script(std::move(script))
        , m_chunk(chunkBytes)
    {
        m_server.listen(QHostAddress::LocalHost, 0);
        connect(&m_server, &QTcpServer::newConnection, this, [this]() {
            m_connection = m_server.nextPendingConnection();
            ++m_connections;
            connect(m_connection, &QTcpSocket::readyRead, this, [this]() {
                m_received += m_connection->readAll();
            });
            if (m_autoReplay) {
                if (m_replayDelayMs > 0) {
                    QTimer::singleShot(m_replayDelayMs, this,
                                       [this]() { sendScript(); });
                } else {
                    sendScript();
                }
            }
        });
    }

    quint16 port() const { return m_server.serverPort(); }
    int connections() const { return m_connections; }
    QByteArray received() const { return m_received; }

    // Replay the script this long AFTER the connection is accepted, rather
    // than from inside the newConnection handler.
    //
    // This exists because of a platform divergence that a loopback test cannot
    // otherwise express. On Linux the whole roster is already in the socket
    // buffer by the time QTcpSocket::connected fires, so a test that waits on
    // isConnected() and then performs an isReady()-gated call passes anyway —
    // by accident. On macOS it is not, and the same test failed every run. The
    // ordering being modelled is the DEVICE's (TCP is up, the device has not
    // spoken yet), which is a real state on every platform; leaving it to
    // scheduling luck means only some platforms ever test it.
    void setReplayDelay(int ms) { m_replayDelayMs = ms; }

    // Accept the next connection but say nothing until the test calls send().
    // The real device replays its roster on reconnect; how LONG it takes to is
    // not something the client gets to assume, and that window is the whole
    // subject of testReconnectStaysStaleUntilAuthoritativeReplay().
    void setAutoReplay(bool on) { m_autoReplay = on; }

    void send(const QByteArray& data)
    {
        if (m_connection != nullptr) {
            m_connection->write(data);
            m_connection->flush();
        }
    }

    void drop()
    {
        if (m_connection != nullptr) {
            m_connection->abort();
            m_connection->deleteLater();
            m_connection = nullptr;
        }
    }

private:
    void sendScript()
    {
        if (m_chunk <= 0) {
            send(m_script);
            return;
        }
        // One byte (or a few) at a time — the worst case the network can
        // produce, and the one the framing code exists for.
        for (int offset = 0; offset < m_script.size(); offset += m_chunk) {
            send(m_script.mid(offset, m_chunk));
        }
    }

    QTcpServer m_server;
    QTcpSocket* m_connection{nullptr};
    QByteArray m_script;
    QByteArray m_received;
    int m_chunk{0};
    int m_connections{0};
    int m_replayDelayMs{0};
    bool m_autoReplay{true};
};

// Pump the event loop until `predicate` holds or the deadline passes.
bool waitFor(const std::function<bool()>& predicate, int timeoutMs = 5000)
{
    QDeadlineTimer deadline(timeoutMs);
    while (!deadline.hasExpired()) {
        if (predicate()) {
            return true;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    return predicate();
}

void testConnectsAndParses()
{
    FakeDevice device(kRoster + kUpdates);
    GreenHeronModel model;
    model.connectToHost(QStringLiteral("127.0.0.1"), device.port());

    const bool ready = waitFor([&]() {
        return model.switchState(QStringLiteral("AS-84F-3")).selected
               == QLatin1String("Beam-15");
    });
    report("connects and applies the roster + updates", ready);
    report("connected state reported", model.isConnected());

    const QStringList ports = model.switchState(QStringLiteral("AS-84F-1")).ports;
    report("port roster kept in announcement order",
           ports == QStringList({"Beam-15", "Beam-20", "OFF"}),
           ports.join(QLatin1Char(',')).toStdString());
}

void testDisplayOrderIsSortedAnnouncedOrderIsNot()
{
    FakeDevice device(kRoster);
    GreenHeronModel model;
    model.connectToHost(QStringLiteral("127.0.0.1"), device.port());

    const bool ready = waitFor([&]() { return model.announcedOrder().size() == 4; });
    // Hoisted: report()'s arguments are evaluated in an unspecified order, so
    // a detail string read inline could be sampled before waitFor() ran.
    const QString announced = model.announcedOrder().join(QLatin1Char(','));
    report("all four switches announced", ready, announced.toStdString());

    // Announcement order is what the device sent, unsorted, and is the index
    // basis for lock slots.
    report("announced order preserves arrival, unsorted",
           model.announcedOrder()
               == QStringList({"AS-84F-1", "AS-84F-3", "AS-84F-2", "AS-84F-4"}),
           model.announcedOrder().join(QLatin1Char(',')).toStdString());

    // Display order is sorted so the columns do not jump around. It is a
    // different thing and must never be used for lock slots.
    report("display order is sorted",
           model.displayOrder()
               == QStringList({"AS-84F-1", "AS-84F-2", "AS-84F-3", "AS-84F-4"}),
           model.displayOrder().join(QLatin1Char(',')).toStdString());
}

void testLockSlotsIndexByAnnouncementOrder()
{
    FakeDevice device(kRoster + kUpdates + kLocks);
    GreenHeronModel model;
    model.connectToHost(QStringLiteral("127.0.0.1"), device.port());

    const bool ready = waitFor([&]() {
        return !model.switchState(QStringLiteral("AS-84F-1")).locks.isEmpty();
    });
    report("lock record applied", ready);

    const QMap<QString, QString> held =
        model.locksBySwitch(QStringLiteral("AS-84F-1"));

    // Slot 1 → the SECOND switch announced → AS-84F-3. Indexing the sorted
    // display order instead would name AS-84F-2 here: no crash, nothing
    // logged, and the grid confidently labels the wrong switch.
    report("slot 1 resolves to the second switch ANNOUNCED, not the second sorted",
           held.value(QStringLiteral("Beam-15")) == QLatin1String("AS-84F-3"),
           held.value(QStringLiteral("Beam-15")).toStdString());
    report("OFF slots are not treated as holding an antenna",
           !held.contains(QStringLiteral("OFF")));
    report("a switch does not report itself as the holder",
           model.locksBySwitch(QStringLiteral("AS-84F-3"))
               .value(QStringLiteral("Beam-15"))
               .isEmpty());
}

void testRecordsSplitOneByteAtATimeStillParse()
{
    FakeDevice device(kRoster + kUpdates, /*chunkBytes=*/1);
    GreenHeronModel model;
    model.connectToHost(QStringLiteral("127.0.0.1"), device.port());

    // Wait on the last field to arrive, not the first: SWITCHADD registers the
    // switch well before SWITCHUPDATE fills in the selection.
    const bool ready = waitFor([&]() {
        return model.switchState(QStringLiteral("AS-84F-3")).selected
               == QLatin1String("Beam-15");
    }, 10000);
    report("a byte-at-a-time stream still parses", ready);
    report("roster survived the byte-at-a-time stream",
           model.switchState(QStringLiteral("AS-84F-1")).ports
               == QStringList({"Beam-15", "Beam-20", "OFF"}));
}

void testSelectIsFireAndForgetAndNeverAssumed()
{
    // Replayed after the link comes up, so the window this test has to wait
    // out is the device's silence rather than whatever the platform's socket
    // buffering happens to do.
    FakeDevice device(kRoster + kUpdates);
    device.setReplayDelay(200);
    GreenHeronModel model;
    model.connectToHost(QStringLiteral("127.0.0.1"), device.port());
    // isReady(), not isConnected(): selectPort() moves real relays and refuses
    // a merely-connected model, by design. And SWITCHADD — the record that
    // opens that gate — carries no selection, so isReady() alone is still not
    // this test's precondition: the SWITCHUPDATE naming OFF has to have landed
    // too, or the "local state is NOT updated" check below reads an empty
    // string rather than the OFF it means to hold constant.
    //
    // On a Linux loopback both are in the socket buffer by the time `connected`
    // fires; on macOS they are not. Gating on the wrong predicate here failed
    // 5/5 on macOS and never on the Linux gate.
    const bool ready = waitFor([&]() {
        return model.isReady()
               && model.switchState(QStringLiteral("AS-84F-1")).selected
                      == QLatin1String("OFF");
    });
    report("the roster and its updates landed", ready);

    const bool sent = model.selectPort(QStringLiteral("AS-84F-1"),
                                       QStringLiteral("Beam-20"));
    report("select reports success while connected", sent);

    const bool arrived = waitFor([&]() {
        return device.received().contains(QByteArrayLiteral("SET_SWITCH"));
    });
    const QByteArray onTheWire = device.received();
    report("SET_SWITCH reaches the device",
           arrived
               && onTheWire.contains(
                   QByteArrayLiteral("SET_SWITCH\x1f" "AS-84F-1\x1f" "Beam-20\r\n")),
           onTheWire.toStdString());

    // The device is the only authority on where the relays are. A command we
    // sent must not move the local model — a relay that fails to move has to
    // show up as a button that does not light, not as a UI that lies.
    report("local state is NOT updated from the command we sent",
           model.switchState(QStringLiteral("AS-84F-1")).selected
               == QLatin1String("OFF"),
           model.switchState(QStringLiteral("AS-84F-1")).selected.toStdString());

    // ...and it does update when the device confirms.
    device.send(QByteArrayLiteral("SWITCHUPDATE\x1f" "AS-84F-1\x1f" "Beam-20\x1f" "0\x1f" "-27\r\n"));
    const bool confirmed = waitFor([&]() {
        return model.switchState(QStringLiteral("AS-84F-1")).selected
               == QLatin1String("Beam-20");
    });
    report("the device's confirmation is what moves the model", confirmed);
}

// Record order on the wire is the device's business, not ours: a SWITCHUPDATE
// can precede the SWITCHADD that names the switch's antennas. That state is
// real and the device may never resend it, so it is kept — but a switch with no
// known antennas is not offerable, so it stays out of displayOrder() until its
// roster lands.
void testUpdateBeforeAddIsHeldButNotOffered()
{
    FakeDevice device(QByteArrayLiteral(
        "SWITCHUPDATE\x1f" "AS-84F-9\x1f" "Beam-40\x1f" "0\x1f" "-31\r\n"));
    GreenHeronModel model;
    model.connectToHost(QStringLiteral("127.0.0.1"), device.port());

    const bool held = waitFor([&]() {
        return model.switchState(QStringLiteral("AS-84F-9")).selected
               == QLatin1String("Beam-40");
    });
    report("state from an update before its add is kept", held);
    report("but a switch with no antennas is not offered as a choice",
           model.displayOrder().isEmpty(),
           model.displayOrder().join(QLatin1Char(',')).toStdString());

    // The SWITCHADD lands: the switch becomes offerable AND the earlier
    // selection is still there — filling in the roster must not wipe it.
    device.send(QByteArrayLiteral(
        "SWITCHADD\x1f" "1\x1f" "AS-84F-9\x1f" "Beam-40\x1d" "0\x1d" "0\x1d" "false\x1f" "OFF\x1d" "0\x1d" "0\x1d" "false\r\n"));
    const bool offered = waitFor([&]() {
        return model.displayOrder() == QStringList{QStringLiteral("AS-84F-9")};
    });
    report("it becomes a choice once its roster arrives", offered,
           model.displayOrder().join(QLatin1Char(',')).toStdString());
    report("and the selection made before the roster survived",
           model.switchState(QStringLiteral("AS-84F-9")).selected
               == QLatin1String("Beam-40"),
           model.switchState(QStringLiteral("AS-84F-9")).selected.toStdString());

    // The attribution invariant the whole feature rests on: anything offerable
    // has an announcement index, so a lock slot can always be resolved to it.
    for (const QString& name : model.displayOrder()) {
        report("an offered switch has an announcement index",
               model.announcedOrder().contains(name), name.toStdString());
    }
}

void testSelectRefusedWhenNotConnected()
{
    GreenHeronModel model;
    report("select refused with no connection",
           !model.selectPort(QStringLiteral("AS-84F-1"), QStringLiteral("OFF")));
}

void testKeepAliveIsTheNulByte()
{
    // Captured from the vendor client: a single NUL, not a newline, every
    // 5.000 s. The interval is too long to wait out in a test, so assert the
    // constants and that nothing else is transmitted unprompted.
    report("keepalive byte is NUL", GreenHeron::kKeepAliveByte == '\0');
    report("keepalive interval is 5 s", GreenHeron::kKeepAliveIntervalMs == 5000);

    // The device transmits first; there is no client hello.
    FakeDevice device(kRoster);
    GreenHeronModel model;
    model.connectToHost(QStringLiteral("127.0.0.1"), device.port());
    report("the link came up", waitFor([&]() { return model.isConnected(); }));
    waitFor([&]() { return false; }, 300);   // let any stray write land
    report("the client sends nothing before the device speaks",
           device.received().isEmpty(), device.received().toStdString());
}

void testDropKeepsLastStateVisibleThenReconnects()
{
    FakeDevice device(kRoster + kUpdates);
    GreenHeronModel model;
    model.connectToHost(QStringLiteral("127.0.0.1"), device.port());
    waitFor([&]() {
        return model.switchState(QStringLiteral("AS-84F-3")).selected
               == QLatin1String("Beam-15");
    });

    device.drop();

    // State stays on screen, flagged stale, rather than blanking on a blip.
    report("a dropped link goes stale", waitFor([&]() { return model.isStale(); }));
    report("the last known selection is still readable while stale",
           model.switchState(QStringLiteral("AS-84F-3")).selected
               == QLatin1String("Beam-15"));

    const bool reconnected =
        waitFor([&]() { return device.connections() >= 2 && model.isConnected(); },
                10000);
    report("it reconnects on its own", reconnected,
           std::to_string(device.connections()));
}

void testReconnectStaysStaleUntilAuthoritativeReplay()
{
    FakeDevice device(kRoster + kUpdates);
    GreenHeronModel model;
    model.connectToHost(QStringLiteral("127.0.0.1"), device.port());
    waitFor([&]() {
        return model.switchState(QStringLiteral("AS-84F-3")).selected
               == QLatin1String("Beam-15");
    });
    report("a fresh connection that has replayed is ready", model.isReady());

    // Connection 2 is accepted but deliberately silent. TCP is up; the device
    // has not yet said a word about where the relays are.
    device.setAutoReplay(false);
    device.drop();

    const bool reconnected =
        waitFor([&]() { return device.connections() >= 2 && model.isConnected(); },
                10000);
    report("TCP comes back up before any replay", reconnected,
           std::to_string(device.connections()));

    // THE WINDOW. The roster on screen predates the drop; nothing on this
    // connection has confirmed it. Every end-state assertion in this file
    // passes here, which is exactly why the bug survived the first review.
    report("the retained panel stays stale until authoritative replay",
           model.isStale());
    report("a reconnect that has not replayed is not ready", !model.isReady());
    report("a stale pre-replay selection is refused",
           !model.selectPort(QStringLiteral("AS-84F-1"), QStringLiteral("Beam-20")));

    // The device finally speaks. Now — and not before — the panel is live.
    device.send(kRoster + kUpdates);
    report("authoritative replay clears the gate",
           waitFor([&]() { return model.isReady(); }));
    report("replay also clears stale", !model.isStale());
    report("a selection after replay is accepted",
           model.selectPort(QStringLiteral("AS-84F-1"), QStringLiteral("Beam-20")));
}

// ── Rotators ────────────────────────────────────────────────────────────────

void testPointIsWhatEstablishesARotator()
{
    FakeDevice device(kRoster + kUpdates + kRotorAdd + kRotorPoint);
    GreenHeronModel model;
    model.connectToHost(QStringLiteral("127.0.0.1"), device.port());

    const bool seen = waitFor([&]() { return !model.rotorNames().isEmpty(); });
    report("POINT establishes a rotator", seen,
           model.rotorNames().join(QLatin1Char(',')).toStdString());
    const GreenHeronRotorState rotor = model.rotorState(QStringLiteral("Rotor"));
    report("the reported heading is stored", std::abs(rotor.heading - 50.4) < 1e-9,
           std::to_string(rotor.heading));
    report("POINT field 3 is carried, not interpreted",
           rotor.status == QLatin1String("0"), rotor.status.toStdString());
    report("a live rotator reads live", model.isRotorLive(QStringLiteral("Rotor")));
}

void testTheAnnouncementArrivesBeforeTheFirstHeading()
{
    // The order the real device uses, and the one that made `announced` dead
    // on hardware while every loopback test passed: ADD lands within ~100 ms
    // of connect and the first POINT about a second later, so a DeviceAdd
    // branch that annotates the rotor table directly annotates nothing.
    FakeDevice device(kRoster + kRotorAdd + kUpdates);
    device.setAutoReplay(false);
    GreenHeronModel model;
    model.connectToHost(QStringLiteral("127.0.0.1"), device.port());
    // autoReplay is off, so there is nothing to wait for past TCP itself —
    // checked anyway because device.send() on a connection that never arrived
    // is a silent no-op, and every assertion below would then pass for the
    // wrong reason.
    report("the link came up", waitFor([&]() { return model.isConnected(); }));

    device.send(kRotorAdd);
    waitFor([&]() { return false; }, 200);
    report("ADD alone still creates no rotator", model.rotorNames().isEmpty());

    device.send(kRotorPoint);
    const bool seen = waitFor([&]() { return !model.rotorNames().isEmpty(); });
    report("the later POINT creates it", seen);
    report("and it remembers the earlier announcement",
           model.rotorState(QStringLiteral("Rotor")).announced);
}

void testDeviceAddAloneIsNotARotator()
{
    // ADD is a generic device announcement — the field is whatever the
    // operator named the device, and only POINT proves it reports a heading.
    // A device that announces itself and never points is not something this
    // tile can drive, and offering a Turn button for it would be a lie.
    FakeDevice device(kRoster + kUpdates + kRotorAdd);
    GreenHeronModel model;
    model.connectToHost(QStringLiteral("127.0.0.1"), device.port());

    const bool switchesLanded =
        waitFor([&]() { return model.announcedOrder().size() == 4; });
    report("the switch roster landed", switchesLanded);
    report("ADD alone does not create a rotator", model.rotorNames().isEmpty(),
           model.rotorNames().join(QLatin1Char(',')).toStdString());
    report("turning a rotator that never pointed is refused",
           !model.turnTo(QStringLiteral("Rotor"), 90.0));
}

void testPointDoesNotOpenTheSwitchReplayGate()
{
    // The two halves share a socket and nothing else. A heading says nothing
    // about where the relays are, so a rotator's POINT must not be what makes
    // a retained, pre-drop antenna roster clickable again.
    FakeDevice device(kRotorPoint);
    GreenHeronModel model;
    model.connectToHost(QStringLiteral("127.0.0.1"), device.port());

    const bool rotorSeen = waitFor([&]() { return !model.rotorNames().isEmpty(); });
    report("the rotator arrived", rotorSeen);
    report("POINT alone does not make the switch panel ready", !model.isReady());
    report("a rotator that has pointed can still be turned",
           model.turnTo(QStringLiteral("Rotor"), 90.0));
}

void testTurnIsFireAndForgetAndNeverAssumed()
{
    FakeDevice device(kRoster + kUpdates + kRotorAdd + kRotorPoint);
    GreenHeronModel model;
    model.connectToHost(QStringLiteral("127.0.0.1"), device.port());
    // turnTo() is gated on liveness the way selectPort() is gated on
    // isReady(), so the POINT has to have landed before the command goes out.
    report("the rotator arrived",
           waitFor([&]() { return !model.rotorNames().isEmpty(); }));

    const bool sent = model.turnTo(QStringLiteral("Rotor"), 89.0);
    report("turnTo reports the command went out", sent);

    const bool onTheWire = waitFor([&]() {
        return device.received().contains(QByteArrayLiteral("TURN"));
    });
    report("TURN reached the device", onTheWire);
    report("the bytes are the captured command",
           device.received().contains(QByteArrayLiteral("TURN\x1f" "Rotor\x1f" "89.0\r")),
           device.received().toHex(' ').toStdString());

    // The whole point: a commanded 89.0 settled a couple of degrees short and
    // then dithered, so the model must still be showing 50.4 — what the device
    // last reported — and not the number we asked for.
    report("the commanded heading did NOT move the model",
           std::abs(model.rotorState(QStringLiteral("Rotor")).heading - 50.4) < 1e-9,
           std::to_string(model.rotorState(QStringLiteral("Rotor")).heading));

    // ...and the device's own report is what does move it.
    device.send(QByteArrayLiteral("POINT\x1f" "Rotor\x1f" "86.7\x1f" "0\x1f" "0\r\n"));
    const bool moved = waitFor([&]() {
        return std::abs(model.rotorState(QStringLiteral("Rotor")).heading - 86.7) < 1e-9;
    });
    report("the device's POINT is what moves it", moved);
}

void testTurnRefusesAnUnencodableHeading()
{
    FakeDevice device(kRoster + kUpdates + kRotorPoint);
    GreenHeronModel model;
    model.connectToHost(QStringLiteral("127.0.0.1"), device.port());
    // Without this the refusal below could be the liveness gate turning down
    // an absent rotator rather than the encoder turning down 400.0 — the
    // assertion would pass while testing nothing.
    report("the rotator arrived",
           waitFor([&]() { return !model.rotorNames().isEmpty(); }));

    report("a heading past 360 is refused",
           !model.turnTo(QStringLiteral("Rotor"), 400.0));
    report("nothing was transmitted for it",
           !device.received().contains(QByteArrayLiteral("TURN")),
           device.received().toHex(' ').toStdString());
}

void testRotorStateBelongsToTheSocketItArrivedOn()
{
    // A heading from the connection before last is exactly the phantom the
    // silence timeout exists to kill, and it would arrive with a Turn button
    // already enabled. Rotator state is therefore dropped for every socket,
    // which is what makes turnTo()'s liveness check a statement about the
    // CURRENT connection.
    FakeDevice device(kRoster + kUpdates + kRotorPoint);
    GreenHeronModel model;
    model.connectToHost(QStringLiteral("127.0.0.1"), device.port());
    // "gone after the drop" is only evidence if it was there before it.
    report("the rotator arrived",
           waitFor([&]() { return !model.rotorNames().isEmpty(); }));

    device.setAutoReplay(false);
    device.drop();

    const bool gone = waitFor([&]() { return model.rotorNames().isEmpty(); });
    report("a reconnect drops the previous socket's rotator", gone,
           model.rotorNames().join(QLatin1Char(',')).toStdString());
    report("and refuses to turn it", !model.turnTo(QStringLiteral("Rotor"), 90.0));

    // The switch panel deliberately behaves the other way round: it keeps the
    // last roster visible and flags it stale. That asymmetry is intentional.
    report("the switch roster is still shown, flagged stale",
           !model.switchState(QStringLiteral("AS-84F-1")).ports.isEmpty());
}

void testARotatorThatGoesQuietStopsBeingOffered()
{
    // The headline safety property, and the one no other signal can supply.
    // Powering a rotator's controller off is NOT a socket event: ADD and POINT
    // simply stop while SWITCHUPDATE and SWITCHLOCKS carry on down the same
    // connection. Nothing on the wire ever contradicts the last heading, so a
    // client without a silence timeout leaves one on screen indefinitely
    // beside a Turn button that would aim a controller that is off.
    //
    // This waits out the real GreenHeron::kRotorSilentAfterMs rather than
    // reaching into the model, because a timeout that can be shortened for a
    // test is a timeout the test is no longer measuring.
    FakeDevice device(kRoster + kUpdates + kRotorPoint);
    GreenHeronModel model;
    model.connectToHost(QStringLiteral("127.0.0.1"), device.port());
    waitFor([&]() { return !model.rotorNames().isEmpty(); });
    report("the rotator was reporting to begin with",
           model.isRotorLive(QStringLiteral("Rotor")));

    // The controller goes off. The socket does not, and the switch half keeps
    // running — which is precisely why silence is the only evidence.
    device.send(kUpdates);
    const bool silent = waitFor(
        [&]() { return !model.isRotorLive(QStringLiteral("Rotor")); },
        GreenHeron::kRotorSilentAfterMs + 5000);
    report("a rotator that stops reporting goes silent", silent);
    report("and is no longer offered", model.rotorNames().isEmpty(),
           model.rotorNames().join(QLatin1Char(',')).toStdString());
    report("and cannot be turned", !model.turnTo(QStringLiteral("Rotor"), 90.0));
    report("while the connection itself is still up and healthy",
           model.isConnected() && model.isReady());
}

void testDeliberateDisconnectClearsThePanel()
{
    FakeDevice device(kRoster + kUpdates);
    GreenHeronModel model;
    model.connectToHost(QStringLiteral("127.0.0.1"), device.port());
    // The roster, not the link: disconnectFromHost() is not gated, so waiting
    // on isConnected() would not have failed here — it would have made all
    // three assertions below vacuous. "A deliberate disconnect clears the
    // roster" is not evidence of clearing unless there was a roster to clear.
    report("the roster landed before the disconnect",
           waitFor([&]() { return !model.announcedOrder().isEmpty(); }));

    model.disconnectFromHost();
    report("a deliberate disconnect is not stale", !model.isStale());
    report("a deliberate disconnect clears the roster",
           model.displayOrder().isEmpty() && model.announcedOrder().isEmpty());
    report("a deliberate disconnect stops wanting the link", !model.isWanted());
}

void testUnreachableHostReportsWithoutDying()
{
    GreenHeronModel model;
    QString reported;
    QObject::connect(&model, &GreenHeronModel::errorOccurred,
                     [&reported](const QString& message) { reported = message; });
    // Port 1 on loopback: nothing listens there.
    model.connectToHost(QStringLiteral("127.0.0.1"), 1);
    const bool errored = waitFor([&]() { return !reported.isEmpty(); });
    report("an unreachable host reports an error", errored, reported.toStdString());
    report("an unreachable host does not read as connected", !model.isConnected());
    model.disconnectFromHost();
}

void testEmptyHostIsRejected()
{
    GreenHeronModel model;
    QString reported;
    QObject::connect(&model, &GreenHeronModel::errorOccurred,
                     [&reported](const QString& message) { reported = message; });
    model.connectToHost(QStringLiteral("   "));
    report("an empty host is refused rather than dialled",
           !reported.isEmpty() && !model.isWanted(), reported.toStdString());
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    testConnectsAndParses();
    testDisplayOrderIsSortedAnnouncedOrderIsNot();
    testLockSlotsIndexByAnnouncementOrder();
    testRecordsSplitOneByteAtATimeStillParse();
    testSelectIsFireAndForgetAndNeverAssumed();
    testUpdateBeforeAddIsHeldButNotOffered();
    testSelectRefusedWhenNotConnected();
    testKeepAliveIsTheNulByte();
    testDropKeepsLastStateVisibleThenReconnects();
    testReconnectStaysStaleUntilAuthoritativeReplay();
    testPointIsWhatEstablishesARotator();
    testDeviceAddAloneIsNotARotator();
    testTheAnnouncementArrivesBeforeTheFirstHeading();
    testPointDoesNotOpenTheSwitchReplayGate();
    testTurnIsFireAndForgetAndNeverAssumed();
    testTurnRefusesAnUnencodableHeading();
    testRotorStateBelongsToTheSocketItArrivedOn();
    testARotatorThatGoesQuietStopsBeingOffered();
    testDeliberateDisconnectClearsThePanel();
    testUnreachableHostReportsWithoutDying();
    testEmptyHostIsRejected();

    std::printf(g_failed ? "\n%d check(s) FAILED\n" : "\nAll checks passed\n", g_failed);
    return g_failed ? 1 : 0;
}
