// HL2 — add and close receivers WHILE EP6 IS FLOWING.
//
// This is the one test in the suite where the operator's "Add Panadapter" and
// pane-close run against a live stream, and that combination is the whole point.
//
// m_rx is RESHAPED on this thread by createPanadapter()/removePanadapter(): a
// push_back reallocates and an erase shifts. The EP6 fan-out used to iterate that
// same vector on the I/O thread, dereferencing m_rx[i].dsp for every arriving
// packet, so either reshape could pull the storage out from under it. The fan-out
// now works from its own list — see Hl2Backend::publishIoDsps() — and this test
// exercises the window where the two used to collide.
//
// A pass here does NOT prove the separation on its own: a use-after-free on a
// four-element vector usually reads memory the allocator has handed straight back,
// and nothing visible happens. What this test does is put the contended window on
// the wire so a sanitizer can see it — under -fsanitize=thread, the old fan-out
// reports the receiver vector as raced from here and the current one does not.
//
// Read that as a DIFFERENTIAL, never as a count: QtCore ships uninstrumented, so
// every Qt::BlockingQueuedConnection in the backend reports as a race whatever the
// receiver set is doing. hl2_backend_test, which predates this work, reports 57.
// HERMES §20.15.1 has the full account.
//
// Under a plain build, treat the checks below as what they are — churn does not
// corrupt the receiver set or lose a receiver's signals.
//
// The fake radio is UNCAPPED, unlike hl2_backend_test's: EP6 has to still be
// arriving at the instant the vector is reshaped, or there is no window to test.

#include "core/backends/hl2/Hl2Backend.h"
#include "core/backends/hl2/MetisProtocol.h"
#include "core/backends/IRadioBackend.h"
#include "TestSettingsProfile.h"
#include "TestDspBuildWait.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QHostAddress>
#include <QNetworkDatagram>
#include <QSignalSpy>
#include <QSet>
#include <QString>
#include <QTimer>
#include <QUdpSocket>

#include <cstdint>
#include <cstdio>

using namespace AetherSDR;
using namespace AetherSDR::hl2;

static int g_failures = 0;
static void check(bool cond, const char* what)
{
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

static QByteArray fakeEp6(std::uint32_t seq)
{
    QByteArray p(static_cast<int>(kUsbPacketSize), 0);
    auto* b = reinterpret_cast<std::uint8_t*>(p.data());
    b[0] = 0xEF; b[1] = 0xFE; b[2] = 0x01; b[3] = 0x06;
    b[4] = static_cast<std::uint8_t>(seq >> 24); b[5] = static_cast<std::uint8_t>(seq >> 16);
    b[6] = static_cast<std::uint8_t>(seq >> 8);  b[7] = static_cast<std::uint8_t>(seq);
    b[8] = b[9] = b[10] = 0x7F;
    b[8 + kFrameSize] = b[9 + kFrameSize] = b[10 + kFrameSize] = 0x7F;
    return p;
}

static void spin(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

int main(int argc, char** argv)
{
    TestSettingsProfile settingsProfile(QStringLiteral("hl2-receiver-churn-test"));
    if (!settingsProfile.isValid()) {
        std::fprintf(stderr, "FAIL: could not create an isolated settings profile\n");
        return 1;
    }

    QCoreApplication app(argc, argv);
    qRegisterMetaType<SliceDelta>();

    // ---- an UNCAPPED fake HL2: EP6 for as long as the client asks for it ----
    QUdpSocket radio;
    check(radio.bind(QHostAddress::LocalHost, 0), "fake radio binds");
    const quint16 radioPort = radio.localPort();
    bool streaming = false;
    std::uint32_t seq = 0;
    QObject::connect(&radio, &QUdpSocket::readyRead, &radio, [&] {
        while (radio.hasPendingDatagrams()) {
            const QNetworkDatagram dg = radio.receiveDatagram();
            const QByteArray& d = dg.data();
            if (d.size() >= 4 && static_cast<std::uint8_t>(d[2]) == 0x04) {
                streaming = (static_cast<std::uint8_t>(d[3]) & 0x01) != 0;
                continue;
            }
            if (streaming)
                radio.writeDatagram(fakeEp6(seq++), dg.senderAddress(), dg.senderPort());
        }
    });

    Hl2Backend backend;

    // Which receivers the seam says exist, tracked the way the client tracks it:
    // this backend has no panAdded — a pan is ANNOUNCED by the first geometry
    // report naming it (RadioModel materialises the pane from that) and retired by
    // panRemoved. spectrumFrameReady is the separate question of whether a
    // receiver's DSP is actually producing; a receiver that exists but whose
    // spectrum never arrives is what a dropped or mis-indexed fan-out looks like.
    QSet<QString> pans;
    QSet<int> slicesSeen;
    QSet<int> spectrumFrom;
    QObject::connect(&backend, &IRadioBackend::panCenterBandwidthChanged, &backend,
                     [&](const QString& id, double, double) { pans.insert(id); });
    QObject::connect(&backend, &IRadioBackend::panRemoved, &backend,
                     [&](const QString& id) { pans.remove(id); });
    QObject::connect(&backend, &IRadioBackend::sliceChanged, &backend,
                     [&](int sliceId, const SliceDelta&) { slicesSeen.insert(sliceId); });
    QObject::connect(&backend, &IRadioBackend::spectrumFrameReady, &backend,
                     [&](int panId, const QByteArray&) { spectrumFrom.insert(panId); });

    QSignalSpy connectedSpy(&backend, &IRadioBackend::connected);

    RadioConnectRequest req;
    req.host = QStringLiteral("127.0.0.1");
    req.port = radioPort;
    backend.connectRadio(req);
    AetherSDR::test::awaitDspBuild("hl2_receiver_churn_test",
                                  [&] { return connectedSpy.count() >= 1; });
    spin(1200);   // unchanged settle budget; only the connect wait moved out of it
    check(connectedSpy.count() == 1, "connected() on the first EP6");
    check(backend.isConnected(), "link up before the churn starts");
    check(pans.size() == 1, "one pan to begin with");

    // ---- churn ----
    //
    // Three cycles rather than one. The first add/close pair exercises the grow
    // and shrink; the ones after it exercise them against a vector that has
    // already been reallocated once, which is when a stale iterator or a stale
    // capacity assumption shows up rather than on the first pass.
    for (int cycle = 0; cycle < 3; ++cycle) {
        const int before = pans.size();
        const bool added = backend.createPanadapter();
        check(added, "createPanadapter succeeds while EP6 is flowing");
        // Deliberately SHORT: the point is to reshape m_rx again while the
        // fan-out is still delivering into the set the previous call produced.
        spin(150);
        check(backend.isConnected(), "the link survives adding a receiver");
        check(pans.size() == before + 1, "the new pan is announced");

        // Close the one that was just opened — the highest pan id present, which
        // on this backend is the newest receiver.
        QString newest;
        for (const QString& id : pans)
            if (newest.isEmpty() || id > newest)
                newest = id;
        check(!newest.isEmpty(), "found the pan to close");
        const bool removed = backend.removePanadapter(newest);
        check(removed, "removePanadapter succeeds while EP6 is flowing");
        spin(150);
        check(backend.isConnected(), "the link survives closing a receiver");
        check(pans.size() == before, "the closed pan is retired");
    }

    // ---- and the surviving receiver is still a working receiver ----
    //
    // The assertion that matters after churn. Closing a receiver RENUMBERS every
    // DDC index after it, so a chain wired to an index rather than to a stable id
    // would go quiet here with nothing logged — the spectrum for receiver 0 simply
    // stops arriving.
    spectrumFrom.clear();
    spin(600);
    check(backend.isConnected(), "still connected after the churn");
    check(pans.size() == 1, "back to one pan");
    check(spectrumFrom.contains(0), "receiver 0 still produces spectrum after the churn");

    // ---- closing the last receiver is refused, not obeyed ----
    check(!backend.removePanadapter(*pans.cbegin()),
          "the last receiver cannot be closed");
    check(pans.size() == 1, "and it is still there");

    backend.disconnectRadio();
    spin(200);

    if (g_failures == 0)
        std::fprintf(stderr, "hl2_receiver_churn_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
