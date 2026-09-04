// Does the DSP read-back follow a control, against a real gateware
// implementation rather than a constructed object?
//
// hl2_dsp_readback_test proves the read-back reports the DSP and not the
// request, on an Hl2TxDsp built in isolation. What it cannot prove is that the
// path from an operator control down to that DSP is connected at all — and
// "the control does nothing" is precisely the defect §8's read-back item
// exists to expose. Only a connected backend shows it.
//
// SIMULATOR ONLY, and enforced rather than asserted. The identity check below
// is the same one hl2_tx_loopback_test uses: it probes a host and requires the
// responder's MAC to be hpsdrsim's synthetic AA:BB:CC:DD:88:FF, so pointing
// this at a real radio SKIPS instead of driving somebody's transmitter. It
// never keys — every control here is receive-side.
//
// Exits 77 when no simulator answers, which the SKIP_RETURN_CODE on its
// add_test turns into an honest ctest "Skipped" rather than a pass that
// measured nothing.


#include "core/backends/hl2/Hl2Backend.h"
#include "core/backends/hl2/MetisProtocol.h"
#include "TestDspBuildWait.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QTimer>
#include <QUdpSocket>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>
#include <QHostAddress>
#include <QVariantMap>

using namespace AetherSDR;
using AetherSDR::hl2::Hl2Backend;

constexpr int kSkipExit = 77;

// Why the probe failed, so a skip says which rather than reading as
// "the simulator is down" for every cause.
static const char* probeReason(int p)
{
    switch (p) {
    case 0:  return "nothing answered";
    case 1:  return "answered, but not a discovery reply";
    case 2:  return "answered, but the MAC is not hpsdrsim's — refusing to "
                    "drive what may be a real radio";
    default: return "unknown";
    }
}

static int g_failures = 0;
static void check(bool ok, const char* what)
{
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) {
        ++g_failures;
    }
}

static void spin(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

// The rx-wdsp entry for receiver 0, or an empty map.
static QVariantMap rxChain(const Hl2Backend& b)
{
    for (const QVariant& v : b.dspChains()) {
        const QVariantMap m = v.toMap();
        if (m.value(QStringLiteral("chain")).toString() == QLatin1String("rx-wdsp")
            && m.value(QStringLiteral("receiver")).toInt() == 0) {
            return m;
        }
    }
    return {};
}

enum class Probe { NoReply, Unreadable, NotSimulator, Simulator };

// hpsdrsim builds its discovery reply with a synthetic MAC written byte by byte
// in its source: AA BB CC DD <radio-type> FF. No HPSDR board ships an
// AA:BB:CC:DD OUI, so this is a dependable "simulator, not a radio"
// fingerprint.
//
// THIS test never keys — every control it drives is receive-side — so the gate
// is not here to keep a transmitter off the air. It is here so the test does
// not silently CONNECT to somebody's radio and drive its receiver settings
// while reporting a pass about a simulator. Same fingerprint, different reason,
// and worth stating rather than inheriting the loopback test's wording.
//
// It replaces a bare "did anything answer at 192.168.1.12". That address is not
// the simulator's by construction; it is only where one happened to run once. On
// another network it is somebody's actual radio. It was also, on the author's
// LAN, silently reaching a simulator on a DIFFERENT machine — which is where the
// "fails non-deterministically" reputation came from, since the answers depended
// on what that other machine was doing at the time.
//
// Which hpsdrsim: the g0orx/pihpsdr build docs/HERMES.md §7 pins as the fixture. It
// writes those bytes at hpsdrsim.c:628-633. Current upstream (dl1ycf/pihpsdr)
// uses a different synthetic MAC, so a simulator built from that one reads as
// NotSimulator and this test skips rather than running against it.
static Probe findSimulator(const QString& host, AetherSDR::hl2::DiscoveryReply* out)
{
    const QHostAddress target(host);
    QUdpSocket s;
    if (!s.bind(QHostAddress(QHostAddress::AnyIPv4), 0)) {
        return Probe::NoReply;
    }
    const auto req = AetherSDR::hl2::discoveryRequest();
    s.writeDatagram(reinterpret_cast<const char*>(req.data()),
                    static_cast<qint64>(req.size()), target,
                    AetherSDR::hl2::kMetisPort);

    // Read until the deadline instead of believing the first datagram, and
    // ignore anything that did not come from the host we probed. Taking the
    // first packet is a narrow version of this test's own root cause: one stray
    // sender consumes the window, and the answer that decides whether we may key
    // never gets looked at.
    QElapsedTimer clock;
    clock.start();
    bool answeredUnparseable = false;
    while (true) {
        const qint64 remaining = 1500 - clock.elapsed();
        if (remaining <= 0 || !s.waitForReadyRead(static_cast<int>(remaining))) {
            break;
        }
        while (s.hasPendingDatagrams()) {
            QByteArray dg(2048, 0);
            QHostAddress from;
            const qint64 got = s.readDatagram(dg.data(), dg.size(), &from);
            if (got <= 0) {
                continue;
            }
            if (!from.isEqual(target, QHostAddress::TolerantConversion)) {
                continue;
            }
            const auto reply = AetherSDR::hl2::parseDiscoveryReply(
                std::span<const std::uint8_t>(
                    reinterpret_cast<const std::uint8_t*>(dg.constData()),
                    static_cast<std::size_t>(got)));
            // Answered, but not as a discovery reply. Kept distinct from the
            // MAC mismatch below so the skip message can say which it was —
            // reporting an all-zero MAC for a packet that never parsed sends
            // whoever reads it looking for a device that does not exist.
            if (!reply) {
                answeredUnparseable = true;
                continue;
            }
            if (out) {
                *out = *reply;
            }

            const auto& mac = reply->mac;
            const bool synthetic = mac[0] == 0xAA && mac[1] == 0xBB && mac[2] == 0xCC
                                && mac[3] == 0xDD && mac[5] == 0xFF;
            return synthetic ? Probe::Simulator : Probe::NotSimulator;
        }
    }
    return answeredUnparseable ? Probe::Unreadable : Probe::NoReply;
}

// The IQ sample rate this test runs the receiver at, and therefore the span the
// spectrum frames cover. Declared once because every expected-bin computation
// derives from it.
constexpr int kIqRateHz = 48000;

// hpsdrsim's synthetic receive scene: two tones it generates itself into
// toneItab/toneQtab, at offsets fixed relative to the ADC rather than the NCO,
// present whether or not we are keyed.
//
// It builds them as I = sin(theta), Q = cos(theta) — so I + jQ = j*exp(-j*theta),
// a NEGATIVE frequency on the wire, and a receive path that conjugates correctly
// draws them ABOVE centre. That is the anchor: these tones reach the spectrum
// without passing through the transmitter, so their side of centre pins the
// RECEIVE end's handedness on its own. With it pinned, the transmit assertions
// below can no longer be satisfied by a matched pair of errors.
constexpr double kSceneToneLowHz = 800.0;
constexpr double kSceneToneHighHz = 4000.0;

// Bin of a baseband offset in the displayed spectrum, which is fftshifted (DC at
// the centre bin) and in the analytic convention (above the carrier is above
// centre). The ONE place that sign convention is written down.
static int binForOffset(double offsetHz, int n)
{
    return n / 2 + static_cast<int>(
        std::lround(offsetHz * n / static_cast<double>(kIqRateHz)));
}

// Strongest level within +/-halfWidth bins of centreBin. Offsets rarely land on
// an exact bin — 800 Hz is 17.07 bins at this rate — so a bare lookup reads the
// shoulder of the tone rather than its peak.
static float peakNear(const std::vector<float>& spec, int centreBin, int halfWidth)
{
    const int n = static_cast<int>(spec.size());
    const int lo = std::max(0, centreBin - halfWidth);
    const int hi = std::min(n - 1, centreBin + halfWidth);
    float best = -300.0f;
    for (int i = lo; i <= hi; ++i) {
        best = std::max(best, spec[static_cast<std::size_t>(i)]);
    }
    return best;
}


int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    const QString simHost = qEnvironmentVariableIsSet("AETHER_HL2_SIM_HOST")
        ? qEnvironmentVariable("AETHER_HL2_SIM_HOST")
        : QStringLiteral("127.0.0.1");
    if (QHostAddress(simHost).isNull()) {
        std::fprintf(stderr,
            "hl2_dsp_readback_sim_test: SKIPPED — AETHER_HL2_SIM_HOST=\"%s\" is "
            "not an IP literal.\n", qPrintable(simHost));
        return kSkipExit;
    }

    AetherSDR::hl2::DiscoveryReply reply{};
    const Probe probe = findSimulator(simHost, &reply);
    if (probe != Probe::Simulator) {
        std::fprintf(stderr,
            "hl2_dsp_readback_sim_test: SKIPPED — no hpsdrsim at %s (%s). "
            "Build it per docs/HERMES.md §7.\n",
            qPrintable(simHost), probeReason(static_cast<int>(probe)));
        return kSkipExit;
    }

    Hl2Backend backend;
    RadioConnectRequest req;
    req.host = simHost;
    backend.connectRadio(req);
    AetherSDR::test::awaitDspBuild("hl2_dsp_readback_sim_test",
                                   [&] { return backend.isConnected(); });
    spin(2000);
    check(backend.isConnected(), "connected to the simulator");
    if (!backend.isConnected()) {
        return 1;
    }

    // The chain must exist and say which level it is reporting, or nothing
    // below means anything.
    const QVariantMap base = rxChain(backend);
    check(!base.isEmpty(), "the read-back reports an rx-wdsp chain");
    check(base.value(QStringLiteral("level")).toString()
              == QLatin1String("channel-config"),
          "and names the level it is reporting");
    if (base.isEmpty()) {
        return 1;
    }

    // ---- passband: drive it, require the read-back to follow ---------------
    // Driven through the same IRadioBackend seam the bridge's `slice filter`
    // reaches, so what passes here is what the operator's control does.
    struct Band { int lo, hi; };
    for (const Band b : {Band{400, 2400}, Band{200, 3200}}) {
        backend.setSliceFilter(0, b.lo, b.hi);
        spin(600);
        const QVariantMap m = rxChain(backend);
        const int lo = m.value(QStringLiteral("filterLowHz")).toInt();
        const int hi = m.value(QStringLiteral("filterHighHz")).toInt();
        check(lo == b.lo && hi == b.hi,
              "a driven passband reaches the DSP and is read back");
        if (lo != b.lo || hi != b.hi) {
            std::fprintf(stderr, "      asked %d..%d, read back %d..%d\n",
                         b.lo, b.hi, lo, hi);
        }
    }

    // ---- AGC ---------------------------------------------------------------
    for (const char* mode : {"fast", "slow", "med"}) {
        backend.setSliceAgc(0, QString::fromLatin1(mode), 65);
        spin(600);
        const QString got = rxChain(backend)
                                .value(QStringLiteral("agcMode")).toString();
        check(got == QLatin1String(mode),
              "a driven AGC mode reaches the DSP and is read back");
        if (got != QLatin1String(mode)) {
            std::fprintf(stderr, "      asked %s, read back %s\n",
                         mode, qPrintable(got));
        }
    }

    // ---- mode --------------------------------------------------------------
    // Each mode carries its own passband, so the read-back must change shape
    // and not merely change a number. LSB's pair is NEGATIVE — the
    // carrier-relative convention — which an echo of the request could not
    // produce, since the request carried no sign.
    backend.setSliceMode(0, QStringLiteral("USB"));
    spin(700);
    const QVariantMap usb = rxChain(backend);
    backend.setSliceMode(0, QStringLiteral("LSB"));
    spin(700);
    const QVariantMap lsb = rxChain(backend);
    check(usb.value(QStringLiteral("filterHighHz")).toInt() > 0,
          "USB reads back a positive passband");
    check(lsb.value(QStringLiteral("filterLowHz")).toInt() < 0,
          "LSB reads back a NEGATIVE passband — the DSP's sign, not the request's");
    check(usb.value(QStringLiteral("filterLowHz")).toInt()
              != lsb.value(QStringLiteral("filterLowHz")).toInt(),
          "a mode change moves the read-back");

    // ---- and it must not move on its own -----------------------------------
    const QVariantMap a = rxChain(backend);
    spin(700);
    const QVariantMap b2 = rxChain(backend);
    check(a.value(QStringLiteral("filterLowHz")) == b2.value(QStringLiteral("filterLowHz"))
              && a.value(QStringLiteral("agcMode")) == b2.value(QStringLiteral("agcMode")),
          "the read-back is stable when nothing is driven");

    backend.disconnectRadio();
    spin(300);

    if (g_failures == 0) {
        std::printf("\nALL PASS\n");
        return 0;
    }
    std::printf("\nFAILURES PRESENT\n");
    return 1;
}
