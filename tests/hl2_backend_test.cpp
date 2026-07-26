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
    // Pan geometry, captured from before connect: the span and the span LIMITS
    // are both pushed from the linkUp handler, so a spy created later would miss
    // the report the GUI depends on to clamp its zoom.
    QSignalSpy spanSpy(&backend, &IRadioBackend::panCenterBandwidthChanged);
    QSignalSpy limitsSpy(&backend, &IRadioBackend::panBandwidthLimitsChanged);
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

    // ---- panadapter span: the widest window by default, and honest limits ----
    //
    // The span an HL2 delivers IS its IQ sample rate, so these two facts are the
    // whole of the zoom contract. Before this the backend defaulted to 48 kHz and
    // reported no limits at all, so the GUI clamped against a FlexLib model table
    // that falls through to 5.4 MHz for an unrecognised model — the operator got a
    // 48 kHz window they could zoom fourteen times wider than the data, and the
    // spectrum that was never sampled rendered as black bars.
    check(!spanSpy.isEmpty(), "pan geometry published on connect");
    if (!spanSpy.isEmpty()) {
        // Asserted on the FIRST report, which is the one that decides what the
        // operator sees when the radio comes up.
        const double firstSpanMhz = spanSpy.first().at(2).toDouble();
        check(qFuzzyCompare(firstSpanMhz, 0.384),
              "connect comes up on the WIDEST span the hardware offers (384 kHz)");
    }
    check(limitsSpy.count() >= 1, "span limits reported on connect");
    if (!limitsSpy.isEmpty()) {
        check(qFuzzyCompare(limitsSpy.first().at(1).toDouble(), 0.048)
                  && qFuzzyCompare(limitsSpy.first().at(2).toDouble(), 0.384),
              "reported limits are the real rate range, 48 kHz .. 384 kHz");
    }

    // ---- a span request snaps to a rate the DDC can actually run ----
    //
    // There is no continuous zoom on this radio: four rates, and a request lands
    // on the nearest by RATIO (zoom is multiplicative, and the rates are
    // octave-spaced, so linear-nearest would bias every request toward the wider
    // neighbour). The backend reports back what it TOOK, never what was asked —
    // that echo is what stops the view widening past the data.
    struct SpanCase {
        double requestMhz;
        double expectMhz;
        const char* what;
    };
    const SpanCase cases[] = {
        {0.384, 0.384, "the widest request stays at 384 kHz"},
        {0.192, 0.192, "an exact rate is taken exactly"},
        {0.100, 0.096, "100 kHz snaps DOWN to 96 kHz, not up to 192 kHz"},
        // THE case that pins ratio-nearest rather than linear-nearest, and the
        // only one in this table that can tell them apart. Between 96 and 192 kHz
        // the geometric mean is 135.8 kHz and the arithmetic mean is 144 kHz, so
        // 140 kHz falls on opposite sides of the two rules: by ratio it belongs to
        // 192 kHz, by linear distance to 96 kHz. Every other row here agrees under
        // both rules, so without this one the log() could be deleted and the suite
        // would stay green.
        {0.140, 0.192, "140 kHz snaps UP to 192 kHz — nearest by RATIO, not by "
                       "linear distance"},
        {0.048, 0.048, "the narrowest request reaches 48 kHz"},
        // The old Flex-table fallback. It must not widen the receiver past what it
        // has: the request is honoured only as far as 384 kHz.
        {5.400, 0.384, "a 5.4 MHz request clamps to the widest real rate"},
        {0.000001, 0.048, "an absurdly narrow request floors at 48 kHz"},
    };
    for (const auto& c : cases) {
        spanSpy.clear();
        backend.setPanBandwidth(QStringLiteral("hl2"), c.requestMhz * 1.0e6);
        spin(60);
        // Every request re-publishes, including one that changes nothing —
        // otherwise a zoom the hardware cannot honour would leave the display
        // sitting on the operator's requested span with no correction coming.
        check(!spanSpy.isEmpty(), c.what);
        if (spanSpy.isEmpty())
            continue;
        const double gotMhz = spanSpy.last().at(2).toDouble();
        check(qFuzzyCompare(gotMhz, c.expectMhz), c.what);
        if (!qFuzzyCompare(gotMhz, c.expectMhz)) {
            std::fprintf(stderr, "  requested %.6f MHz, expected %.6f, got %.6f\n",
                         c.requestMhz, c.expectMhz, gotMhz);
        }
    }

    // A rate change must not drag the tuned signal with it. The slice was left at
    // 14.100 MHz above; widening and narrowing the window around it has to leave
    // it exactly there.
    backend.setSliceFrequency(0, 14'100'000.0);
    backend.setPanBandwidth(QStringLiteral("hl2"), 384000.0);
    spin(60);
    backend.setPanBandwidth(QStringLiteral("hl2"), 48000.0);
    spin(60);
    QSignalSpy sliceSpy(&backend, &IRadioBackend::sliceChanged);
    backend.setPanBandwidth(QStringLiteral("hl2"), 192000.0);
    spin(60);
    check(!sliceSpy.isEmpty(), "a span change re-publishes the slice");
    if (!sliceSpy.isEmpty()) {
        const SliceDelta d = sliceSpy.last().at(1).value<SliceDelta>();
        check(d.frequency && qFuzzyCompare(*d.frequency, 14.1),
              "the slice stays on frequency across span changes");
    }

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
