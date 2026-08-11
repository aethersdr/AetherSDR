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
            sendScript();
        });
    }

    quint16 port() const { return m_server.serverPort(); }
    int connections() const { return m_connections; }
    QByteArray received() const { return m_received; }

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
    FakeDevice device(kRoster + kUpdates);
    GreenHeronModel model;
    model.connectToHost(QStringLiteral("127.0.0.1"), device.port());
    waitFor([&]() { return model.isConnected(); });

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
    waitFor([&]() { return model.isConnected(); });
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

void testDeliberateDisconnectClearsThePanel()
{
    FakeDevice device(kRoster + kUpdates);
    GreenHeronModel model;
    model.connectToHost(QStringLiteral("127.0.0.1"), device.port());
    waitFor([&]() { return model.isConnected(); });

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
    testDeliberateDisconnectClearsThePanel();
    testUnreachableHostReportsWithoutDying();
    testEmptyHostIsRejected();

    std::printf(g_failed ? "\n%d check(s) FAILED\n" : "\nAll checks passed\n", g_failed);
    return g_failed ? 1 : 0;
}
