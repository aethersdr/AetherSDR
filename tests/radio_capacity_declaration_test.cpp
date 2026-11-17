#include "FlexRadioStatusTestSupport.h"
// #5594 item 3: the panadapter and slice capacity a Flex declares for itself.
//
// A radio states its capacity outright in the discovery packet. Observed on a
// FLEX-8600 running 4.2.20.41343, passively (the datagram is an unsolicited
// broadcast; nothing was connected):
//
//   max_panadapters=4  available_panadapters=4  max_slices=4  available_slices=4
//
// CAPACITY AND AVAILABILITY ARE DIFFERENT KEYS and this is the whole point of
// the change. `max_*` is what the hardware and licence allow and does not move;
// `available_*` — and the live `slices=`/`panadapters=` status — are the FREE
// counts, which fall as any client opens objects. FlexLib keeps all four apart
// (Discovery.cs:141/154/247/260, copied separately at API.cs:186-189).
//
// An earlier attempt derived capacity as (open objects + free ones) off the
// status plane. It failed three ways, all of which this route removes rather
// than fixes: the bounding helper was seeded at its own ceiling and returned it
// unconditionally; the open count came from a container holding only our own
// objects, so Multi-Flex undercounted; and `sub radio all` precedes `sub pan
// all`, so the first status arrives before any inventory exists at all. See
// #5603. Reading the declared capacity needs no inventory and no ordering.
//
// This pins the MODEL half — precedence, independence, fallback and radio swap.
// The parser half (that max_* is read and the adjacent available_* is not) lives
// in radio_discovery_test, which already has friend access to the parser.
//
// An inert transport suppresses all outbound dials. Capacity observations and
// disconnect/reconnect transitions still run through the production model.

#include "TestSettingsProfile.h"
#include "core/backends/flex/RadioConnection.h"
#include <QThread>

#include "core/RadioDiscovery.h"
#include "models/ModelCapabilities.h"
#include "models/RadioModel.h"

#include <QCoreApplication>
#include <QHostAddress>
#include <QMetaObject>
#include <QString>

#include <cstdio>

using namespace AetherSDR;

namespace AetherSDR {
class RadioConnectionSessionTestAccess {
public:
    class NoDialSocket final : public QTcpSocket {
    public:
        explicit NoDialSocket(QObject* parent) : QTcpSocket(parent) {}
        void connectToHost(const QString&, quint16, OpenMode = ReadWrite,
                           NetworkLayerProtocol = AnyIPProtocol) override {}
    };
    static void install(RadioConnection& wire)
    {
        QMetaObject::invokeMethod(&wire, [&wire] {
            wire.m_socket->disconnect(&wire);
            wire.m_socket->deleteLater();
            wire.m_socket = new NoDialSocket(&wire);
        }, Qt::BlockingQueuedConnection);
    }
};
}
class CapacityModel final : public RadioModel {
public:
    CapacityModel() { RadioConnectionSessionTestAccess::install(*connection()); }
};

static int g_failures = 0;
static void check(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", what);
    if (!ok) ++g_failures;
}

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("radio-capacity-declaration"));
    if (!profile.isValid()) { return 1; }
    QCoreApplication app(argc, argv);

    // ---- the declared capacity beats the model table ----
    {
        // A FLEX-6700 is 8 in the FlexLib-sourced table. This one declares 3 —
        // the shape a reduced licence produces, and a value the table can never
        // express. Every assertion here fails if the table is consulted first.
        CapacityModel m;
        RadioInfo info;
        info.model = QStringLiteral("FLEX-6700");
        info.address = QHostAddress(QStringLiteral("192.0.2.2"));   // TEST-NET-1
        info.maxPanadapters = 3;
        info.maxSlices = 3;
        m.connectToRadio(info);

        check(capabilitiesFor(QStringLiteral("FLEX-6700")).maxSlices == 8,
              "the model table would have said 8");
        check(m.maxPanadapters() == 3,
              "the declared panadapter capacity wins over the model table");
        check(m.maxSlices() == 3,
              "the declared slice capacity wins over the model table");

        // THE LEG THIS CHANGE JUSTIFIES ITSELF BY, and which nothing covered
        // until #5603's third review: RadioResourceAdapter serializes
        // backendCapabilities() onto the control protocol, so the descriptor
        // has to carry the declared number too — not just the accessors the GUI
        // reads. Deleting the publishRadioReportedCapacity() call leaves every
        // other assertion in this file green.
        check(m.backendCapabilities().maxPanadapters == 3,
              "the declared panadapter capacity reaches the capability descriptor");
        check(m.backendCapabilities().maxSlices == 3,
              "the declared slice capacity reaches the capability descriptor");
    }

    // ---- pan and slice capacity are independent ----
    {
        // Nothing guarantees the two stay equal; the old code assumed they did.
        CapacityModel m;
        RadioInfo info;
        info.model = QStringLiteral("FLEX-6700");
        info.address = QHostAddress(QStringLiteral("192.0.2.2"));   // TEST-NET-1
        info.maxSlices = 8;
        info.maxPanadapters = 2;
        m.connectToRadio(info);
        check(m.maxSlices() == 8 && m.maxPanadapters() == 2,
              "a radio may declare different slice and panadapter capacities");
    }

    // ---- no declaration falls back to the table ----
    {
        CapacityModel m;
        RadioInfo info;
        info.model = QStringLiteral("FLEX-6700");
        info.address = QHostAddress(QStringLiteral("192.0.2.2"));   // TEST-NET-1   // maxSlices/maxPanadapters stay 0
        m.connectToRadio(info);
        check(m.maxPanadapters() == 8 && m.maxSlices() == 8,
              "a radio that declares nothing falls back to the model table");
    }

    // ---- a radio swap does not inherit the previous radio's capacity ----
    {
        CapacityModel m;
        RadioInfo big;
        big.model = QStringLiteral("FLEX-6700");
        big.address = QHostAddress(QStringLiteral("192.0.2.2"));   // TEST-NET-1
        big.maxPanadapters = 8;
        big.maxSlices = 8;
        m.connectToRadio(big);
        check(m.maxPanadapters() == 8, "first radio declares 8");

        // Connecting by IP builds a RadioInfo with no discovery keys at all.
        // The previous radio's 8 must not survive into a 2-panadapter radio.
        RadioInfo byIp;
        byIp.model = QStringLiteral("FLEX-6400");
        byIp.address = QHostAddress(QStringLiteral("192.0.2.3"));   // TEST-NET-1
        m.connectToRadio(byIp);
        check(m.maxPanadapters() == capabilitiesFor(QStringLiteral("FLEX-6400")).maxSlices,
              "a connect that declares nothing falls back to the new radio's "
              "table rather than inheriting the previous radio's capacity");
    }

    // ---- a connect path that bypasses connectToRadio() cannot inherit ----
    //
    // connectToRadio() is NOT the only connect path: connectViaWan() takes no
    // RadioInfo, and the LAN auto-reconnect timer drives the connection
    // directly. RadioModel.cpp:7705-7710 records the same three-path lesson for
    // m_nickname (#4260). The declared capacity is therefore cleared on the
    // DISCONNECT side, which closes all three at once — and the descriptor is
    // republished on the connected edge, which all three reach.
    //
    // Without the clear this is worse than stale: the precedence guards refuse
    // the model-table correction that used to repair it, so a leftover 8 would
    // offer creates a FLEX-6400 must refuse.
    {
        CapacityModel m;
        RadioInfo big;
        big.model = QStringLiteral("FLEX-6700");
        big.address = QHostAddress(QStringLiteral("192.0.2.2"));   // TEST-NET-1
        big.maxPanadapters = 8;
        big.maxSlices = 8;
        m.connectToRadio(big);
        check(m.maxPanadapters() == 8, "first radio declares 8");

        // Retire the attempt through the backend owner so reconnect can admit
        // a replacement and the model receives the normal disconnect edge.
        m.forceDisconnect();
        check(m.maxPanadapters() == capabilitiesFor(QString()).maxSlices,
              "the declaration does not survive the disconnect");

        // A smaller radio arrives on a path that never calls connectToRadio():
        // its model lands on the status plane instead. The model table must be
        // allowed to answer again.
        FlexRadioStatusTestPeer::beginConnect(m);
        observeRadioStatus(m, QStringLiteral("radio"),
                              {{QStringLiteral("model"), QStringLiteral("FLEX-6400")}});
        check(m.maxPanadapters() == capabilitiesFor(QStringLiteral("FLEX-6400")).maxSlices,
              "after a disconnect a model= status restores the model-table "
              "answer instead of the previous radio's declaration");
        check(m.maxSlices() == capabilitiesFor(QStringLiteral("FLEX-6400")).maxSlices,
              "and the slice capacity follows the same rule");
    }

    // ---- a LAN auto-reconnect keeps the licence ceiling ----
    //
    // #5603 review (@NF0T): a reduced-licence radio that rides out a network
    // blip must not silently revert to the model table's higher number. The
    // disconnect-side clear is right — two of the three connect paths never
    // re-seed — but the auto-reconnect timer reconnects to m_lastInfo.address,
    // so it is the same radio and m_lastInfo still carries its declaration.
    //
    // Driven through the timer's own restore rather than a synthetic setter, so
    // the case fails if that restore is removed.
    {
        CapacityModel m;
        RadioInfo licensed;
        licensed.model = QStringLiteral("FLEX-6700");          // table says 8
        licensed.address = QHostAddress(QStringLiteral("192.0.2.2"));   // TEST-NET-1
        licensed.maxSlices = 3;                                 // reduced licence
        licensed.maxPanadapters = 3;
        m.connectToRadio(licensed);
        check(m.maxPanadapters() == 3 && m.maxSlices() == 3,
              "the licensed capacity is in effect");

        // The link drops. The declaration is cleared, as it must be.
        m.forceDisconnect();
        check(m.maxPanadapters() != 3,
              "the declaration is cleared on the way down");

        // The auto-reconnect timer fires for the same radio.
        m.triggerAutoReconnectForTest();
        check(m.maxPanadapters() == 3,
              "an auto-reconnect to the same radio restores its licensed "
              "panadapter capacity rather than the model table's 8");
        check(m.maxSlices() == 3,
              "and its licensed slice capacity");
    }

    // ---- a LAN auto-reconnect restores the version label too ----
    //
    // On an HL2 the label ("Gateware") is half of the supported identity. The
    // restore runs before the transport is chosen, so a Flex RadioInfo reaches
    // it without the HL2 backend and its UDP socket.
    {
        CapacityModel m;
        RadioInfo labelled;
        labelled.model = QStringLiteral("FLEX-6700");
        labelled.address = QHostAddress(QStringLiteral("192.0.2.2"));   // TEST-NET-1
        labelled.version = QStringLiteral("74.2");
        labelled.versionLabel = QStringLiteral("Gateware");
        m.connectToRadio(labelled);
        check(m.versionLabel() == QStringLiteral("Gateware"),
              "the version label is in effect");

        m.forceDisconnect();
        check(m.versionLabel().isEmpty(),
              "the version label is cleared on the way down");

        m.triggerAutoReconnectForTest();
        check(m.version() == QStringLiteral("74.2"),
              "an auto-reconnect restores the version");
        check(m.versionLabel() == QStringLiteral("Gateware"),
              "and the version label with it");
    }

    if (g_failures == 0)
        std::printf("radio_capacity_declaration_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
