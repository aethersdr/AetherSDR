// aetherd HL2 Phase 1b — Hl2Backend seam test. A capped fake HL2 on localhost
// lets the backend connect and produce a panadapter frame; verifies the
// IRadioBackend contract: capabilities (family=hl2, transmit availability),
// connected on first EP6, spectrumFrameReady wired to the data plane,
// sliceChanged on control intents, keying, invokeExtension's async-error stub,
// and disconnected on stop. (Audio demod itself is covered by hl2_rxdsp_test;
// the transmit gate's wire-level behaviour by hl2_tx_gate_test.)

#include "core/backends/IRadioBackend.h"
#include "TestSettingsProfile.h"
#include "TestDspBuildWait.h"

#include "core/backends/hl2/Hl2Backend.h"

#include "core/AppSettings.h"
#include "core/AutomationBridgeSettings.h"
#include "core/backends/hl2/Hl2Settings.h"
#include "core/backends/hl2/MetisProtocol.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QHostAddress>
#include <QNetworkDatagram>
#include <QScopeGuard>
#include <QStringList>
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

// Decode the TX drive level out of an EP2 packet, if this one happens to be
// carrying the drive C&C bank (the round robin rotates through several).
// Returns -1 when the packet carries some other bank.
//
// Same decode as hl2_tx_gate_test: C0 sits at SYNC(3) into each 512-byte frame,
// the MOX bit is masked off the address, and the drive byte is C1 at +4.
static int ep2DriveLevel(const QByteArray& dg)
{
    if (dg.size() < static_cast<int>(hl2::kUsbPacketSize))
        return -1;
    const auto* b = reinterpret_cast<const std::uint8_t*>(dg.constData());
    if (b[0] != 0xEF || b[1] != 0xFE || b[2] != 0x01 || b[3] != 0x02)
        return -1;   // not EP2
    const std::size_t frameStarts[2] = {8, 8 + hl2::kFrameSize};
    for (const std::size_t fs : frameStarts) {
        if ((b[fs + 3] & ~hl2::kC0MoxBit) == hl2::kC0TxDrive)
            return b[fs + 4];
    }
    return -1;
}

int main(int argc, char** argv)
{
    // Redirect settings into a private temporary profile BEFORE QCoreApplication
    // and before the first AppSettings touch.
    //
    // These tests read and write settings the backend now persists (the owned
    // "Hl2" span object, and LowBandwidthConnect for the span ceiling). They used
    // to do that in the operator's live file with scope guards to put it back,
    // which is unsafe twice over: AppSettings::save() rewrites the whole file from
    // an in-memory snapshot, so it can drop keys the running application wrote in
    // the meantime, and any abort between the mutation and the guard leaves the
    // operator's real configuration altered. (#4470)
    TestSettingsProfile settingsProfile(QStringLiteral("hl2-backend-test"));
    if (!settingsProfile.isValid()) {
        std::fprintf(stderr, "FAIL: could not create an isolated settings profile\n");
        return 1;
    }

    QCoreApplication app(argc, argv);
    qRegisterMetaType<SliceDelta>();
    // QSignalSpy stores a notchChanged argument by metatype, so the notch
    // session-scope case below reads an empty QVariant without this.
    qRegisterMetaType<NotchDelta>();

    // The backend RESTORES the operator's remembered span at connect, so the
    // default-span assertion below depends on persisted state. The isolated
    // profile starts empty, so this measures the actual default with no
    // save/restore dance against anyone's real configuration.
    check(!Hl2Settings::spanMhz(),
          "isolated profile starts with no remembered span");

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
    // "hl2" since manual frequency calibration landed (freqcal.get / .set /
    // .set_live). This field is the handshake a client pre-checks before it
    // issues invokeExtension(), so it has to name every namespace the backend
    // actually answers — an empty list here while the verbs work would report
    // the opposite of the truth. The unknown-verb path is still an error; see
    // the invokeExtension case below.
    check(caps.extensionNamespaces == QVector<QString>{QStringLiteral("hl2")},
          "advertises the hl2 extension namespace");

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
    // The passband the FIRST slice report carries, and the order in which the pan
    // is announced versus described. Both are connect-time-only facts, so they
    // have to be captured from before connectRadio().
    int firstFilterLow = -1, firstFilterHigh = -1;
    QStringList panEventOrder;
    QObject::connect(&backend, &IRadioBackend::sliceChanged, &backend,
                     [&](int, const SliceDelta& d) {
                         if (sliceCount == 0) {
                             if (d.filterLow)  firstFilterLow  = *d.filterLow;
                             if (d.filterHigh) firstFilterHigh = *d.filterHigh;
                         }
                         ++sliceCount;
                     });
    QObject::connect(&backend, &IRadioBackend::panCenterBandwidthChanged, &backend,
                     [&](const QString&, double, double) {
                         if (!panEventOrder.contains(QStringLiteral("geometry")))
                             panEventOrder << QStringLiteral("geometry");
                     });
    QObject::connect(&backend, &IRadioBackend::panBandwidthLimitsChanged, &backend,
                     [&](const QString&, double, double) {
                         if (!panEventOrder.contains(QStringLiteral("limits")))
                             panEventOrder << QStringLiteral("limits");
                     });

    // ---- connect ----
    RadioConnectRequest req;
    req.host = QStringLiteral("127.0.0.1");
    req.port = radioPort;
    backend.connectRadio(req);
    // Initial slice/pan state is published from the linkUp handler, NOT here:
    // RadioModel::onConnected() stages every pre-existing model as previous-
    // session leftovers, so anything emitted before connected() is discarded.
    check(!connectedSpy.count(), "connect leaves connected() pending until the first EP6");

    // The DSP has to finish opening before the wire starts; wait on that rather
    // than on a clock. Once EP6 flows, stay inside kSilenceTimeoutMs (2 s): the
    // fake radio stops after kCap frames and the silence watchdog legitimately
    // drops the link after it.
    AetherSDR::test::awaitDspBuild("hl2_backend_test",
                                  [&] { return connectedSpy.count() >= 1; });
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

    // ---- panadapter span: the CHEAP window by default, and honest limits ----
    //
    // The span an HL2 delivers IS its IQ sample rate, so it is not a free
    // display choice: the widest costs ~8x the narrowest in both directions —
    // 25.2 vs 3.1 Mbps of sustained UDP, and 8x the samples through WDSP's
    // decimation front end. With no remembered span the backend must therefore
    // come up NARROW, so nobody pays for a view they did not ask for.
    check(!spanSpy.isEmpty(), "pan geometry published on connect");
    if (!spanSpy.isEmpty()) {
        // The FIRST report is the one that decides what the operator sees when
        // the radio comes up.
        const double firstSpanMhz = spanSpy.first().at(2).toDouble();
        check(qFuzzyCompare(firstSpanMhz, 0.048),
              "with nothing remembered, connect comes up on the CHEAP 48 kHz span");
    }
    check(limitsSpy.count() >= 1, "span limits reported on connect");
    if (!limitsSpy.isEmpty()) {
        // The zoom clamp's source. Without this the GUI falls back to a FlexLib
        // model table that resolves to 5.4 MHz for an unrecognised model, and
        // the operator could zoom 14x past the data — the black bars.
        check(qFuzzyCompare(limitsSpy.first().at(1).toDouble(), 0.048)
                  && qFuzzyCompare(limitsSpy.first().at(2).toDouble(), 0.384),
              "reported limits are the real rate range, 48 kHz .. 384 kHz");
    }

    // ---- the pan is ANNOUNCED before it is DESCRIBED ----
    //
    // RadioModel materialises the HL2's PanadapterModel from
    // panCenterBandwidthChanged; its panBandwidthLimitsChanged handler is
    // `if (!pan) return;` with no materialisation. onConnected() clears
    // m_panadapters and m_activePanId synchronously inside emit connected(), so
    // limits reported before the geometry are dropped for the whole session and
    // nothing re-emits them — leaving the 5.4 MHz FlexLib fallback and the
    // black-bar over-zoom. Ordering is the invariant, so assert the ordering.
    check(panEventOrder == QStringList({QStringLiteral("geometry"),
                                        QStringLiteral("limits")}),
          "pan geometry is reported BEFORE the pan's zoom limits");

    // ---- a fresh connect derives the passband from the MODE ----
    //
    // The member defaults are 150..3000, which is DIGU's passband and no other
    // mode's. Publishing them verbatim on a default-USB connect told the UI
    // DIGU's filter while the mode indicator read USB, and the radio was the
    // side that was right. USB is 100..2900.
    check(firstFilterLow == 100 && firstFilterHigh == 2900,
          "first slice report carries USB's passband (100..2900), not the 150..3000 defaults");

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
    // Each row is a DISCRETE operator gesture, so it must clear the zoom-sweep
    // throttle (kBandwidthThrottleMs = 150 ms) — otherwise consecutive rows are
    // coalesced into one and the snapping under test never runs. The throttle
    // itself is exercised separately below.
    for (const auto& c : cases) {
        spanSpy.clear();
        backend.setPanBandwidth(QStringLiteral("hl2-0"), c.requestMhz * 1.0e6);
        spin(220);
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

    // ---- a zoom SWEEP is coalesced, not applied step by step ----
    //
    // Every span change is a blocking WDSP reconfigure plus a settings write, and
    // it runs on the thread that paces EP2 — the stream the gateware watchdog
    // halts if it stops arriving. A drag delivers ~30 requests a second, and a
    // sweep across the range crosses every intermediate rate, so applying each
    // one meant paying for rebuilds whose results were discarded before anyone
    // saw them.
    //
    // Contract: the FIRST request applies immediately (a discrete zoom step must
    // not feel laggy), everything inside the cooldown is superseded, and the last
    // one wins. What must NOT happen is one rebuild per request.
    {
        backend.setPanBandwidth(QStringLiteral("hl2-0"), 48000.0);
        spin(220);                            // settle, so the sweep starts clean

        QSignalSpy sweepSpy(&backend, &IRadioBackend::panCenterBandwidthChanged);
        // A drag: eight requests well inside one cooldown, ending on 384 kHz.
        for (const double mhz : {0.048, 0.060, 0.096, 0.120, 0.192, 0.240, 0.300, 0.384}) {
            backend.setPanBandwidth(QStringLiteral("hl2-0"), mhz * 1.0e6);
            spin(5);
        }
        const int duringSweep = sweepSpy.count();
        spin(400);                            // let the trailing edge fire

        check(duringSweep < 8,
              "a zoom sweep is coalesced rather than applied request-by-request");
        check(!sweepSpy.isEmpty(), "the sweep still produces a span report");
        if (!sweepSpy.isEmpty()) {
            const double settledMhz = sweepSpy.last().at(2).toDouble();
            check(qFuzzyCompare(settledMhz, 0.384),
                  "the sweep settles on the LAST requested span, not an "
                  "intermediate one");
            if (!qFuzzyCompare(settledMhz, 0.384)) {
                std::fprintf(stderr, "  sweep settled at %.6f MHz (want 0.384), "
                                     "%d reports during the sweep\n",
                             settledMhz, duringSweep);
            }
        }
    }

    // A rate change must not drag the tuned signal with it. The slice was left at
    // 14.100 MHz above; widening and narrowing the window around it has to leave
    // it exactly there.
    backend.setSliceFrequency(0, 14'100'000.0);
    backend.setPanBandwidth(QStringLiteral("hl2-0"), 384000.0);
    spin(220);
    backend.setPanBandwidth(QStringLiteral("hl2-0"), 48000.0);
    spin(220);
    QSignalSpy sliceSpy(&backend, &IRadioBackend::sliceChanged);
    backend.setPanBandwidth(QStringLiteral("hl2-0"), 192000.0);
    spin(220);
    check(!sliceSpy.isEmpty(), "a span change re-publishes the slice");
    if (!sliceSpy.isEmpty()) {
        const SliceDelta d = sliceSpy.last().at(1).value<SliceDelta>();
        check(d.frequency && qFuzzyCompare(*d.frequency, 14.1),
              "the slice stays on frequency across span changes");
    }

    // ---- the chosen span PERSISTS ----
    //
    // This is what makes the cheap default acceptable. Defaulting narrow with
    // no memory would put the operator back where this whole change started —
    // a 48 kHz window on every launch — so the wide view has to be chosen once
    // and kept. The cost is then opted into rather than imposed.
    //
    // Stored as an owned nested object under the "Hl2" root key, per
    // Constitution Principle V, not as a loose flat key.
    {
        // The last successful setPanBandwidth above was 192 kHz.
        check(qFuzzyCompare(Hl2Settings::spanMhz(), 0.192),
              "the applied span is written to the owned Hl2 settings object");

        const QString raw =
            AppSettings::instance().value(QStringLiteral("Hl2"), QString{}).toString();
        check(raw.contains(QLatin1String("spanMhz")),
              "persisted as a nested object under the single \"Hl2\" root key");
        // Principle V is about the SHAPE, so assert there is no flat key too:
        // a loose "Hl2SpanMhz" would satisfy a naive round-trip test and still
        // violate the invariant.
        check(AppSettings::instance()
                  .value(QStringLiteral("Hl2SpanMhz"), QString{})
                  .toString()
                  .isEmpty(),
              "no loose flat key was created alongside the object");

        // And a fresh backend restores it, which is the half that actually
        // reaches the operator on the next launch.
        //
        // Its OWN fake radio, deliberately: reviving the shared one would also
        // answer the first backend's still-running EP2 pacer, bringing that
        // link back up and making its disconnect assertions count a second
        // cycle.
        QUdpSocket radio2;
        check(radio2.bind(QHostAddress::LocalHost, 0), "second fake radio binds");
        std::uint32_t seq2 = 0;
        QObject::connect(&radio2, &QUdpSocket::readyRead, &radio2, [&] {
            while (radio2.hasPendingDatagrams()) {
                const QNetworkDatagram dg = radio2.receiveDatagram();
                if (seq2 < kCap)
                    radio2.writeDatagram(fakeEp6(seq2++), dg.senderAddress(),
                                         dg.senderPort());
            }
        });

        Hl2Settings::setSpanMhz(0.096);
        Hl2Backend restored;
        QSignalSpy restoredSpan(&restored,
                                &IRadioBackend::panCenterBandwidthChanged);
        RadioConnectRequest rr;
        rr.host = QStringLiteral("127.0.0.1");
        rr.port = radio2.localPort();
        restored.connectRadio(rr);
        spin(1200);
        check(!restoredSpan.isEmpty(), "restored backend published pan geometry");
        if (!restoredSpan.isEmpty()) {
            check(qFuzzyCompare(restoredSpan.first().at(2).toDouble(), 0.096),
                  "a fresh connect comes up on the REMEMBERED span, not the default");
        }
        restored.disconnectRadio();
        spin(100);
    }

    // ---- "Use low bandwidth mode" caps the widest span on offer ----
    //
    // On this radio the span IS the data rate, so the widest is 25.2 Mbps of
    // sustained UDP at 3048 packets/second. An operator who has ticked low
    // bandwidth has told us the link cannot carry that, and offering it anyway
    // would produce a connection that drops rather than a display that is wide.
    //
    // The ceiling must apply to the ADVERTISED limits as well as to requests,
    // or the zoom control would let them drag into a span the backend then
    // silently refuses — the display claiming a width the data never had, which
    // is the same class of lie as the black bars.
    {
        // Isolated profile — set it and leave it; nothing outside this process
        // reads this file.
        const QString kLowBw = QStringLiteral("LowBandwidthConnect");
        auto& s = AppSettings::instance();
        s.setValue(kLowBw, QStringLiteral("True"));
        s.save();
        check(Hl2Settings::lowBandwidth(), "low bandwidth mode reads back as set");

        QUdpSocket radio3;
        check(radio3.bind(QHostAddress::LocalHost, 0), "third fake radio binds");
        std::uint32_t seq3 = 0;
        QObject::connect(&radio3, &QUdpSocket::readyRead, &radio3, [&] {
            while (radio3.hasPendingDatagrams()) {
                const QNetworkDatagram dg = radio3.receiveDatagram();
                if (seq3 < kCap)
                    radio3.writeDatagram(fakeEp6(seq3++), dg.senderAddress(),
                                         dg.senderPort());
            }
        });

        Hl2Backend capped;
        QSignalSpy cappedLimits(&capped,
                                &IRadioBackend::panBandwidthLimitsChanged);
        QSignalSpy cappedSpan(&capped, &IRadioBackend::panCenterBandwidthChanged);
        RadioConnectRequest cr;
        cr.host = QStringLiteral("127.0.0.1");
        cr.port = radio3.localPort();
        capped.connectRadio(cr);
        spin(1200);

        check(!cappedLimits.isEmpty(), "capped backend reported span limits");
        if (!cappedLimits.isEmpty()) {
            check(qFuzzyCompare(cappedLimits.first().at(2).toDouble(), 0.096),
                  "low bandwidth caps the advertised max span at 96 kHz");
        }

        // A request for the widest span must land on the ceiling, not above it.
        cappedSpan.clear();
        capped.setPanBandwidth(QStringLiteral("hl2-0"), 384000.0);
        spin(60);
        check(!cappedSpan.isEmpty(), "capped backend republished its span");
        if (!cappedSpan.isEmpty()) {
            check(qFuzzyCompare(cappedSpan.last().at(2).toDouble(), 0.096),
                  "a 384 kHz request is held at the 96 kHz low-bandwidth ceiling");
        }
        capped.disconnectRadio();
        spin(100);
    }

    // ---- disconnect ----
    backend.disconnectRadio();
    spin(50);
    // Already down via the watchdog; disconnectRadio() must not double-report.
    check(disconnectedSpy.count() == 1, "disconnect does not re-emit disconnected()");
    check(!backend.isConnected(), "isConnected() false after disconnect");

    // ---- #4549: TUNE keys at TUNE power, and ANY unkey restores RF power ----
    //
    // The tune carrier's amplitude is a fixed full-scale constant, so the drive
    // register is the only thing that sets tune power. Read it off the WIRE
    // rather than from a flag: the register is what the radio actually obeys.
    //
    // This runs against its own UNCAPPED fake radio, not the shared one above.
    // EP2 is paced only while the link is alive, and the shared radio stops
    // answering after kCap frames so the silence-watchdog assertions can fire —
    // which starves the very stream these checks read. Its own radio also keeps
    // the drive traffic out of the earlier assertions.
    //
    // Sampling rule: the drive bank is a ONE-SHOT (MetisClient::setTxDriveLevel
    // pushes onto m_oneShot; the steady round robin carries freq/gain/ADC only),
    // so each call puts exactly one drive packet into a FIFO. Every check clears
    // lastDrive and THEN acts and spins, so it reads a settled queue.
    if (caps.canTransmit) {
        QUdpSocket tuneRadio;
        check(tuneRadio.bind(QHostAddress::LocalHost, 0), "#4549: tune fake radio binds");
        std::uint32_t tuneSeq = 0;
        int lastDrive = -1;
        QObject::connect(&tuneRadio, &QUdpSocket::readyRead, &tuneRadio, [&] {
            while (tuneRadio.hasPendingDatagrams()) {
                const QNetworkDatagram dg = tuneRadio.receiveDatagram();
                const int drive = ep2DriveLevel(dg.data());
                if (drive >= 0)
                    lastDrive = drive;
                // Always answer: EP2 keeps flowing only while the link is up.
                tuneRadio.writeDatagram(fakeEp6(tuneSeq++), dg.senderAddress(), dg.senderPort());
            }
        });

        Hl2Backend tuner;
        RadioConnectRequest tuneReq;
        tuneReq.host = QStringLiteral("127.0.0.1");
        tuneReq.port = tuneRadio.localPort();
        tuner.connectRadio(tuneReq);
        spin(300);
        check(tuner.isConnected(), "#4549: tune backend connects");

        const auto driveFor = [](int percent) { return percent * hl2::kTxDriveMax / 100; };
        // Settle on a known RF power, distinguishable from tune power.
        lastDrive = -1;
        tuner.setTxPower(100);
        spin(200);
        check(lastDrive == driveFor(100),
              "#4549: RF power 100 reaches the drive register");

        // TUNE at 10% must DROP the drive, not inherit the RF slider. Before the
        // fix the register still held 255 and the tune went out at FULL power.
        lastDrive = -1;
        tuner.setTune(true, 10);
        spin(200);
        check(lastDrive == driveFor(10),
              "#4549: TUNE drives at TUNE power, not the RF Power slider");

        // The unkey that does NOT go through setTune(): the automation TX
        // watchdog and the key verb (RadioModel.cpp:2696) and the MOX/PTT
        // coordinator (RadioModel.cpp:674) all call setKeying(false) directly.
        // That clears m_tuning, so restoring in setTune()'s release branch left
        // these paths at TUNE power with setTxPower() no longer holding off —
        // the radio stayed at 10% until the slider next moved, and the next
        // voice transmission went out at tune power.
        lastDrive = -1;
        tuner.setKeying(false);
        spin(200);
        check(lastDrive == driveFor(100),
              "#4549: an unkey that BYPASSES setTune() still restores RF power");

        // The ordinary path — releasing the TUNE toggle — restores too.
        tuner.setTune(true, 10);
        spin(200);
        lastDrive = -1;
        tuner.setTune(false, 10);
        spin(200);
        check(lastDrive == driveFor(100),
              "#4549: releasing TUNE restores RF power");

        // A power change made mid-tune is the operator's intent for after the
        // carrier drops: remembered, but not applied while the carrier is up.
        tuner.setTune(true, 10);
        spin(200);
        lastDrive = -1;
        tuner.setTxPower(40);
        spin(200);
        check(lastDrive == -1,
              "#4549: a mid-tune power change does not disturb the tune carrier");
        lastDrive = -1;
        tuner.setTune(false, 10);
        spin(200);
        check(lastDrive == driveFor(40),
              "#4549: the unkey restores the power set DURING the tune");

        // ---- #4912: health reports the APPLIED drive, not the request ------
        //
        // Nothing reported the drive the radio was actually given. `get
        // transmit` has rfPower and `get radio` has txPower, but both read
        // TransmitModel — the operator's ask — so a drive that never reached
        // the radio read back as though it had. That is the exact failure the
        // HL2 health section exists to catch, in the one area where it is
        // safety-adjacent.
        //
        // Asserted against `lastDrive`, the value taken off the WIRE by the
        // same reader the checks above use. A readback that agrees only with
        // the model would prove nothing — agreement with the wire is the claim.
        lastDrive = -1;
        tuner.setTxPower(60);
        spin(200);
        {
            const auto h = tuner.healthSnapshot();
            check(lastDrive == driveFor(60), "#4912: RF power 60 reaches the wire");
            check(h.values.value(QStringLiteral("rfPowerPercent")).toInt() == 60,
                  "#4912: health reports the requested drive percent");
            check(h.values.contains(QStringLiteral("txDriveRegister"))
                      && h.values.value(QStringLiteral("txDriveRegister")).toInt()
                             == lastDrive,
                  "#4912: health reports the raw drive register sent to the radio");
            check(h.values.contains(QStringLiteral("txDriveGated"))
                      && !h.values.value(QStringLiteral("txDriveGated")).toBool(),
                  "#4912: drive is not reported as gated in a TX-capable session");
        }

        tuner.disconnectRadio();
        spin(50);
    }

    // ---- #4912: a TX-BLOCKED session says so, instead of looking normal -----
    //
    // applyDrive() forces the register to 0 when the transmit gate is closed
    // while m_rfPowerPercent keeps the operator's value, so the two legitimately
    // disagree — and until now nothing reported the disagreement. A script
    // reading only the request would conclude the radio was driven at 100%.
    //
    // The gate is decided per connect from AETHER_AUTOMATION + ALLOW_TX, so a
    // second backend connected with the bridge's variable set (and ALLOW_TX
    // absent) exercises the blocked path without disturbing anything above.
    {
        const bool hadAutomation = qEnvironmentVariableIsSet("AETHER_AUTOMATION");
        const bool hadAllowTx = qEnvironmentVariableIsSet("AETHER_AUTOMATION_ALLOW_TX");
        check(!hadAllowTx, "#4912: ALLOW_TX is absent, so the gated case is reachable");
        qputenv("AETHER_AUTOMATION", "1");
        const auto restoreEnv = qScopeGuard([hadAutomation] {
            if (!hadAutomation)
                qunsetenv("AETHER_AUTOMATION");
        });

        QUdpSocket gatedRadio;
        check(gatedRadio.bind(QHostAddress::LocalHost, 0), "#4912: gated fake radio binds");
        std::uint32_t gatedSeq = 0;
        int gatedDrive = -1;
        QObject::connect(&gatedRadio, &QUdpSocket::readyRead, &gatedRadio, [&] {
            while (gatedRadio.hasPendingDatagrams()) {
                const QNetworkDatagram dg = gatedRadio.receiveDatagram();
                const int drive = ep2DriveLevel(dg.data());
                if (drive >= 0)
                    gatedDrive = drive;
                gatedRadio.writeDatagram(fakeEp6(gatedSeq++), dg.senderAddress(),
                                         dg.senderPort());
            }
        });

        Hl2Backend gated;
        RadioConnectRequest gatedReq;
        gatedReq.host = QStringLiteral("127.0.0.1");
        gatedReq.port = gatedRadio.localPort();
        gated.connectRadio(gatedReq);
        spin(300);
        check(gated.isConnected(), "#4912: gated backend connects");

        // BEFORE touching drive at all — the case a latched flag got wrong (#4918
        // review). finishDspSetup() seeds the register with a direct
        // setTxDriveLevel(0) that bypasses applyDrive(), so nothing observes the
        // gate acting; meanwhile m_rfPowerPercent still reads its default 100.
        // A flag set only inside applyDrive() therefore denied responsibility for
        // the very divergence the row exists to explain.
        {
            const auto h0 = gated.healthSnapshot();
            check(h0.values.value(QStringLiteral("txDriveGated")).toBool(),
                  "#4918: the gate is reported before drive is ever commanded");
            check(h0.values.value(QStringLiteral("rfPowerPercent")).toInt() == 100,
                  "#4918: and the requested percent is still its default 100");
        }

        gatedDrive = -1;
        gated.setTxPower(100);
        spin(200);
        const auto h = gated.healthSnapshot();
        check(gatedDrive == 0, "#4912: the closed gate holds the wire drive at 0");
        check(h.values.value(QStringLiteral("rfPowerPercent")).toInt() == 100,
              "#4912: the operator's requested percent is still reported");
        check(h.values.value(QStringLiteral("txDriveRegister")).toInt() == 0,
              "#4912: the written register is reported as 0, matching the wire");
        check(h.values.value(QStringLiteral("txDriveGated")).toBool(),
              "#4912: the gate is named, so requested-vs-applied is explainable");

        gated.disconnectRadio();
        spin(50);
    }

    // ---- notch records are SESSION state, and ids are never recycled (#4780) --
    //
    // The half of the notch lifecycle that lives ABOVE Hl2RxDsp, and the one
    // hl2_notch_seed_test cannot reach: that test pins seeding as idempotent
    // against a single DSP chain, and never disconnects, so deleting
    // m_notches.clear() from connectRadio() leaves it green.
    //
    // What that line prevents: RadioModel::onDisconnected() empties TnfModel,
    // but a same-family reconnect rebuilds no backend, so records kept here
    // would be replayed into the fresh WDSP chains by seedNotches() with
    // nothing on screen naming them — nulls in the audio at frequencies the UI
    // does not mention, and no id to remove them by. Asserted through the seam
    // rather than by reaching into m_notches: a record that survived is a
    // record still addressable, so setNotch/removeNotch on last session's id
    // would report back.
    {
        QUdpSocket radio4;
        check(radio4.bind(QHostAddress::LocalHost, 0), "fourth fake radio binds");
        std::uint32_t seq4 = 0;
        QObject::connect(&radio4, &QUdpSocket::readyRead, &radio4, [&] {
            while (radio4.hasPendingDatagrams()) {
                const QNetworkDatagram dg = radio4.receiveDatagram();
                if (seq4 < kCap)
                    radio4.writeDatagram(fakeEp6(seq4++), dg.senderAddress(),
                                         dg.senderPort());
            }
        });

        Hl2Backend notcher;
        QSignalSpy notchedSpy(&notcher, &IRadioBackend::notchChanged);
        QSignalSpy unnotchedSpy(&notcher, &IRadioBackend::notchRemoved);
        QSignalSpy notchConnected(&notcher, &IRadioBackend::connected);
        RadioConnectRequest nr;
        nr.host = QStringLiteral("127.0.0.1");
        nr.port = radio4.localPort();
        notcher.connectRadio(nr);
        AetherSDR::test::awaitDspBuild("hl2_backend_test",
                                      [&] { return notchConnected.count() >= 1; });
        spin(300);
        check(notchConnected.count() >= 1, "notch-session backend came up");

        // Placed against a live chain, so the id the backend mints is the one a
        // real session would carry.
        notcher.createNotch(7'041'000.0, 200.0);
        check(notchedSpy.count() == 1, "createNotch reports the id the backend minted");
        const int firstId = notchedSpy.isEmpty() ? -1 : notchedSpy.first().at(0).toInt();

        notcher.disconnectRadio();
        spin(150);
        seq4 = 0;               // let the same fake answer a second session
        notcher.connectRadio(nr);
        // Awaited rather than merely spun past. The second session opens its own
        // WDSP chains on the I/O thread; leaving that build in flight would put
        // the planner under whatever assertion runs next, which then fails for a
        // reason it does not name.
        AetherSDR::test::awaitDspBuild("hl2_backend_test",
                                      [&] { return notchConnected.count() >= 2; });
        spin(200);
        check(notchConnected.count() >= 2, "the notch-session backend reconnected");

        notchedSpy.clear();
        unnotchedSpy.clear();
        NotchDelta move;
        move.centerHz = 7'042'000.0;
        notcher.setNotch(firstId, move);
        check(notchedSpy.isEmpty(),
              "a notch from the previous session is still editable after reconnect");
        notcher.removeNotch(firstId);
        check(unnotchedSpy.isEmpty(),
              "a notch from the previous session is still removable after reconnect");

        // And the counter deliberately does NOT reset with the records: a
        // recycled id would let a stale reference address a different notch.
        notchedSpy.clear();
        notcher.createNotch(7'050'000.0, 200.0);
        check(notchedSpy.count() == 1, "the new session can still place a notch");
        if (!notchedSpy.isEmpty()) {
            check(notchedSpy.first().at(0).toInt() > firstId,
                  "the new session handed out an id the old one already used");
        }

        notcher.disconnectRadio();
        spin(100);
    }

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
