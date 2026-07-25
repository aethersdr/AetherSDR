// aetherd HL2 Phase 1b — Hl2Backend seam test. A capped fake HL2 on localhost
// lets the backend connect and produce a panadapter frame; verifies the
// IRadioBackend contract: capabilities (family=hl2, transmit availability),
// connected on first EP6, spectrumFrameReady wired to the data plane,
// sliceChanged on control intents, keying, invokeExtension's async-error stub,
// and disconnected on stop. (Audio demod itself is covered by hl2_rxdsp_test;
// the transmit gate's wire-level behaviour by hl2_tx_gate_test.)

#include "core/backends/IRadioBackend.h"
#include "core/backends/hl2/Hl2Backend.h"

#include "core/AutomationBridgeSettings.h"
#include "core/backends/hl2/MetisProtocol.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QHostAddress>
#include <QNetworkDatagram>
#include <QSignalSpy>
#include <QTimer>
#include <QUdpSocket>

#include <cstdint>
#include <cstdio>

using namespace AetherSDR;
using AetherSDR::hl2::Hl2Backend;
namespace hl2 = AetherSDR::hl2;

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
    QByteArray p(static_cast<int>(hl2::kUsbPacketSize), 0);
    auto* b = reinterpret_cast<std::uint8_t*>(p.data());
    b[0] = 0xEF; b[1] = 0xFE; b[2] = 0x01; b[3] = 0x06;
    b[4] = static_cast<std::uint8_t>(seq >> 24); b[5] = static_cast<std::uint8_t>(seq >> 16);
    b[6] = static_cast<std::uint8_t>(seq >> 8);  b[7] = static_cast<std::uint8_t>(seq);
    b[8] = b[9] = b[10] = 0x7F;
    b[8 + hl2::kFrameSize] = b[9 + hl2::kFrameSize] = b[10 + hl2::kFrameSize] = 0x7F;
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
    QCoreApplication app(argc, argv);
    qRegisterMetaType<SliceDelta>();

    // ---- capped fake HL2: a bounded number of EP6 so the WDSP demod stays quick ----
    QUdpSocket radio;
    check(radio.bind(QHostAddress::LocalHost, 0), "fake radio binds");
    const quint16 radioPort = radio.localPort();
    std::uint32_t seq = 0;
    constexpr std::uint32_t kCap = 14;   // ~1764 samples: crosses one 1024 spectrum frame
    QObject::connect(&radio, &QUdpSocket::readyRead, &radio, [&] {
        while (radio.hasPendingDatagrams()) {
            const QNetworkDatagram dg = radio.receiveDatagram();
            if (seq < kCap)
                radio.writeDatagram(fakeEp6(seq++), dg.senderAddress(), dg.senderPort());
        }
    });

    Hl2Backend backend;

    // ---- transmit availability follows the AUTOMATION gate ----
    //
    // An automation run defers to the bridge's own TX gate rather than to an
    // HL2-specific flag. Constructing a second backend under AETHER_AUTOMATION
    // with no permission must report RX-only and refuse to key.
    {
        qputenv("AETHER_AUTOMATION", "1");
        qunsetenv("AETHER_AUTOMATION_ALLOW_TX");
        Hl2Backend gated;
        const bool allowed = gated.capabilities().canTransmit;
        // The persisted operator toggle also opens this gate, and it is a real
        // user setting we must not stomp — so only assert the refusal when the
        // environment we control is the only source in play.
        if (!AutomationBridgeSettings::txAllowed()) {
            check(!allowed, "automation without permission reports RX-only");
        } else {
            std::fprintf(stderr,
                "note: operator TX toggle is ON, so the automation gate is open; "
                "skipping the refusal assertion\n");
        }
        qputenv("AETHER_AUTOMATION_ALLOW_TX", "1");
        Hl2Backend permitted;
        check(permitted.capabilities().canTransmit,
              "automation WITH permission may transmit");
        qunsetenv("AETHER_AUTOMATION");
        qunsetenv("AETHER_AUTOMATION_ALLOW_TX");
    }

    // ---- capabilities ----
    const RadioCapabilities caps = backend.capabilities();
    check(caps.family == QLatin1String("hl2"), "family is hl2");
    // canTransmit is no longer a constant. It reports transmit AVAILABILITY, so
    // the engine's TX guard above the seam sees an RX-only radio exactly when
    // this backend would refuse to key. This process has no AETHER_AUTOMATION
    // set, so it is an interactive run and may transmit.
    check(caps.canTransmit, "canTransmit is true for an interactive run");
    check(caps.maxSlices == 1, "one slice");
    check(caps.sampleRatesHz.contains(48000) && caps.sampleRatesHz.contains(384000), "sample rates");
    check(caps.extensionNamespaces.isEmpty(), "no extension namespaces advertised");

    QSignalSpy connectedSpy(&backend, &IRadioBackend::connected);
    QSignalSpy disconnectedSpy(&backend, &IRadioBackend::disconnected);
    QSignalSpy errSpy(&backend, &IRadioBackend::extensionError);
    int specCount = 0, sliceCount = 0;
    qsizetype lastSpecBytes = 0;
    QObject::connect(&backend, &IRadioBackend::spectrumFrameReady, &backend,
                     [&](int, const QByteArray& ba) { ++specCount; lastSpecBytes = ba.size(); });
    QObject::connect(&backend, &IRadioBackend::sliceChanged, &backend,
                     [&](int, const SliceDelta&) { ++sliceCount; });

    // ---- connect ----
    RadioConnectRequest req;
    req.host = QStringLiteral("127.0.0.1");
    req.port = radioPort;
    backend.connectRadio(req);
    // Initial slice/pan state is published from the linkUp handler, NOT here:
    // RadioModel::onConnected() stages every pre-existing model as previous-
    // session leftovers, so anything emitted before connected() is discarded.
    check(!connectedSpy.count(), "connect leaves connected() pending until the first EP6");

    // Stay inside kSilenceTimeoutMs (2 s): the fake radio stops after kCap
    // frames, and the EP6 silence watchdog legitimately drops the link after it.
    spin(1200);   // capped ping-pong delivers EP6 + at least one spectrum frame

    check(connectedSpy.count() == 1, "connected() on the first EP6");
    check(backend.isConnected(), "isConnected() true");
    check(sliceCount >= 1, "initial slice state published once the link is up");
    check(specCount >= 1, "spectrumFrameReady wired through the seam");
    check(lastSpecBytes == static_cast<qsizetype>(1024 * sizeof(float)),
          "spectrum payload is fftSize float32");

    // ---- control intents each emit a slice delta ----
    const int sliceBefore = sliceCount;
    backend.setSliceFrequency(0, 14'100'000.0);
    backend.setSliceMode(0, QStringLiteral("LSB"));
    backend.setSliceFilter(0, 300, 2700);
    check(sliceCount >= sliceBefore + 3, "freq/mode/filter each emit sliceChanged");

    // ---- keying does not disturb the link ----
    // Whether this actually keys depends on the transmit gate above; what
    // matters here is that asking does not upset the connection either way.
    backend.setKeying(true);
    check(backend.isConnected(), "setKeying(true) does not disrupt the link");
    backend.setKeying(false);
    check(backend.isConnected(), "setKeying(false) does not disrupt the link");

    // ---- invokeExtension honors the async contract ----
    backend.invokeExtension(QStringLiteral("hl2"), QStringLiteral("noop"), 42, {});
    check(errSpy.count() == 1, "awaited invokeExtension -> one extensionError");
    check(errSpy.first().at(0).toULongLong() == 42u, "extensionError carries the requestId");

    // ---- EP6 silence watchdog: the fake radio stopped at kCap, so once the
    // silence window elapses the link must be reported down instead of sitting
    // in a permanently "connected" state. ----
    spin(2600);   // > kSilenceTimeoutMs since the last EP6
    check(disconnectedSpy.count() == 1, "EP6 silence trips the watchdog -> disconnected()");
    check(!backend.isConnected(), "isConnected() false after the silence watchdog");

    // ---- disconnect ----
    backend.disconnectRadio();
    spin(50);
    // Already down via the watchdog; disconnectRadio() must not double-report.
    check(disconnectedSpy.count() == 1, "disconnect does not re-emit disconnected()");
    check(!backend.isConnected(), "isConnected() false after disconnect");

    // ---- F4 (#4448): a connect to a radio that never answers must surface a
    // connectionError (from MetisClient::connectFailed), not wedge silently. ----
    {
        Hl2Backend deadBackend;
        QSignalSpy errorSpy(&deadBackend, &IRadioBackend::connectionError);
        QSignalSpy connSpy(&deadBackend, &IRadioBackend::connected);
        RadioConnectRequest deadReq;
        deadReq.host = QStringLiteral("127.0.0.1");
        deadReq.port = 1;   // nothing answers HPSDR here -> no EP6 ever arrives
        deadBackend.connectRadio(deadReq);
        // The connect watchdog fires after ~2 s of no EP6.
        for (int i = 0; i < 60 && errorSpy.isEmpty(); ++i)
            spin(100);
        check(errorSpy.count() >= 1, "F4: no-EP6 connect emits connectionError");
        check(connSpy.count() == 0, "F4: never reports connected() on a dead link");
        check(!deadBackend.isConnected(), "F4: isConnected() false after connect failure");
    }

    if (g_failures == 0)
        std::fprintf(stderr, "hl2_backend_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
