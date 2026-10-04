// HL2 RIT / XIT (#5386, #6105), per receiver: RIT moves only its own receiver's
// receive shift (and its NCO when the offset leaves the usable window); XIT is
// held per receiver and moves the TX NCO register (C&C addr 0x01) only while its
// receiver owns transmit, never aliased onto RIT; the published slice frequency
// stays the dial, and the held RIT/XIT is published back. Socket-free and unkeyed: a constructed backend,
// no connectRadio(), registers read from MetisClient's C&C banks, MOX asserted clear.

#include "TestSettingsProfile.h"
#include "core/backends/IRadioBackend.h"
#include "core/backends/hl2/Hl2Backend.h"
#include "core/backends/hl2/MetisClient.h"
#include "core/backends/hl2/MetisProtocol.h"

#include <QCoreApplication>
#include <QEvent>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <optional>

namespace AetherSDR::hl2 {

struct MetisClientTestAccess {
    static std::uint32_t hzOf(const Cc& cc)
    {
        return (static_cast<std::uint32_t>(cc[1]) << 24)
             | (static_cast<std::uint32_t>(cc[2]) << 16)
             | (static_cast<std::uint32_t>(cc[3]) << 8)
             |  static_cast<std::uint32_t>(cc[4]);
    }
    static std::uint32_t txRegisterHz(const MetisClient& c) { return hzOf(c.m_ccTxFreq); }
    static std::uint32_t rxRegisterHz(const MetisClient& c, int i)
    {
        return hzOf(c.m_ccRxFreq.at(static_cast<std::size_t>(i)));
    }
    static bool mox(const MetisClient& c) { return c.m_mox; }
};

struct Hl2RitXitTestAccess {
    // A second receiver's STATE, without a DSP or a transport — the part of
    // createPanadapter() RIT scoping depends on. MetisClient still runs one
    // receiver, so only the backend-side state of the second is read.
    static void twoReceivers(Hl2Backend& b)
    {
        b.m_ids.reset(2);
        b.m_rx.assign(2, Hl2Backend::Receiver{});
    }
    static double shiftHz(const Hl2Backend& b, int ddc) { return b.rxShiftHz(*b.rx(ddc)); }
    static double ncoHz(const Hl2Backend& b, int ddc) { return b.rx(ddc)->ncoHz; }
    static double sliceHz(const Hl2Backend& b, int ddc) { return b.rx(ddc)->sliceFreqHz; }
    static QString panIdOf(const Hl2Backend& b, int ddc) { return b.m_ids.byDdc(ddc)->panId; }
    static void buildReceivers(Hl2Backend& b, int count) { b.buildReceivers(count); }
    static int receiverCount(const Hl2Backend& b) { return static_cast<int>(b.m_rx.size()); }
    static bool ritOn(const Hl2Backend& b, int ddc) { return b.rx(ddc)->ritOn; }
    static int ritHz(const Hl2Backend& b, int ddc) { return b.rx(ddc)->ritHz; }
    static bool xitOn(const Hl2Backend& b, int ddc) { return b.rx(ddc)->xitOn; }
    static int xitHz(const Hl2Backend& b, int ddc) { return b.rx(ddc)->xitHz; }
    static bool ncoMovedForRit(const Hl2Backend& b, int ddc) { return b.rx(ddc)->ncoMovedForRit; }

    // Drain the queued register writes on the I/O thread, then read.
    template <typename F>
    static auto onMetis(Hl2Backend& b, F f)
    {
        decltype(f(*b.m_metis)) out{};
        QMetaObject::invokeMethod(b.m_metis, [&] { out = f(*b.m_metis); },
                                  Qt::BlockingQueuedConnection);
        return out;
    }
    static std::uint32_t txRegisterHz(Hl2Backend& b)
    {
        return onMetis(b, [](const MetisClient& c) { return MetisClientTestAccess::txRegisterHz(c); });
    }
    static std::uint32_t rx0RegisterHz(Hl2Backend& b)
    {
        return onMetis(b, [](const MetisClient& c) { return MetisClientTestAccess::rxRegisterHz(c, 0); });
    }
    static bool mox(Hl2Backend& b)
    {
        return onMetis(b, [](const MetisClient& c) { return MetisClientTestAccess::mox(c); });
    }
};

} // namespace AetherSDR::hl2

using namespace AetherSDR;
using namespace AetherSDR::hl2;
using A = Hl2RitXitTestAccess;

static int g_failures = 0;
static void check(bool cond, const char* what)
{
    std::printf("  %s  %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond)
        ++g_failures;
}
static bool near(double a, double b) { return std::abs(a - b) < 0.5; }

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("hl2-rit-xit-test"));
    QCoreApplication app(argc, argv);
    check(profile.isValid(), "settings are isolated");
    std::printf("\n  HL2 RIT / XIT (#5386, #6105)\n\n");

    Hl2Backend backend;
    A::twoReceivers(backend);

    std::optional<double> publishedMhz;
    std::optional<int> publishedRit0;
    std::optional<int> publishedXit0;
    QObject::connect(&backend, &IRadioBackend::sliceChanged, &backend,
                     [&](int sliceId, const SliceDelta& d) {
                         if (sliceId != 0)
                             return;
                         if (d.frequency)
                             publishedMhz = *d.frequency;
                         if (d.ritFreq)
                             publishedRit0 = *d.ritFreq;
                         if (d.xitFreq)
                             publishedXit0 = *d.xitFreq;
                     });

    constexpr double kDial0 = 14'074'000.0;   // TX receiver (DDC 0 owns transmit)
    constexpr double kDial1 = 7'074'000.0;    // a second receiver on another band
    backend.setSliceFrequency(0, kDial0);
    backend.setSliceFrequency(1, kDial1);
    const double base0 = A::shiftHz(backend, 0);
    const double base1 = A::shiftHz(backend, 1);
    const double nco1 = A::ncoHz(backend, 1);
    check(A::txRegisterHz(backend) == 14'074'000u, "TX register starts on the dial");

    // ---- RIT +500 Hz, in the order RadioModel sends it: enable, then offset ----
    backend.setSliceRitEnabled(0, true);
    backend.setSliceRitOffset(0, 500);
    check(near(A::shiftHz(backend, 0), base0 + 500.0),
          "RIT +500: receiver 0's receive shift moves +500 Hz");
    check(A::txRegisterHz(backend) == 14'074'000u,
          "RIT +500: TX register does not move");
    check(near(A::shiftHz(backend, 1), base1) && near(A::ncoHz(backend, 1), nco1),
          "RIT +500: second receiver is unaffected");
    check(near(A::sliceHz(backend, 0), kDial0), "RIT +500: slice state stays the dial");
    check(publishedMhz && near(*publishedMhz * 1e6, kDial0),
          "RIT +500: published slice frequency stays the dial");
    check(publishedRit0 == 500, "RIT +500: the held offset is published back");

    backend.setSliceRitEnabled(0, false);
    check(near(A::shiftHz(backend, 0), base0), "RIT off: receive shift restored");

    // ---- XIT -300 Hz ----
    backend.setSliceXitEnabled(0, true);
    backend.setSliceXitOffset(0, -300);
    check(A::txRegisterHz(backend) == 14'073'700u, "XIT -300: TX register moves -300 Hz");
    check(near(A::shiftHz(backend, 0), base0), "XIT -300: receive shift does not move");
    check(publishedXit0 == -300, "XIT -300: the held offset is published back");

    // Both on at once: two registers, no aliasing either way.
    backend.setSliceRitEnabled(0, true);
    backend.setSliceRitOffset(0, 500);
    check(near(A::shiftHz(backend, 0), base0 + 500.0) && A::txRegisterHz(backend) == 14'073'700u,
          "RIT +500 with XIT -300: each offset reaches only its own path");
    backend.setSliceRitEnabled(0, false);

    backend.setSliceXitEnabled(0, false);
    check(A::txRegisterHz(backend) == 14'074'000u, "XIT off: TX register restored");

    // ---- clamp to the app's ±9999 Hz (SmartCatProtocol kRitMaxHz) ----
    backend.setSliceRitEnabled(0, true);
    backend.setSliceRitOffset(0, 20'000);
    check(near(A::shiftHz(backend, 0), base0 + 9999.0), "RIT offset clamps to +9999 Hz");
    check(publishedRit0 == 9999, "RIT clamp: the clamped offset is published back");
    backend.setSliceXitEnabled(0, true);
    backend.setSliceXitOffset(0, -20'000);
    check(A::txRegisterHz(backend) == 14'064'001u, "XIT offset clamps to -9999 Hz");
    check(publishedXit0 == -9999, "XIT clamp: the clamped offset is published back");
    backend.setSliceXitEnabled(0, false);
    backend.setSliceXitOffset(0, 0);
    backend.setSliceRitEnabled(0, false);
    backend.setSliceRitOffset(0, 0);   // the next case starts from no stored offset

    // ---- a non-transmit receiver's RIT is its own (#6105) ----
    // Receiver 0 owns transmit. RIT on receiver 1 shifts receiver 1 only, and
    // turning it off leaves receiver 0's RIT alone.
    backend.setSliceRitEnabled(0, true);
    backend.setSliceRitOffset(0, 300);
    backend.setSliceRitEnabled(1, true);
    backend.setSliceRitOffset(1, 500);
    check(near(A::shiftHz(backend, 1), base1 + 500.0),
          "RIT +500 on receiver 1 (not transmitting): receiver 1 shifts +500 Hz");
    check(near(A::shiftHz(backend, 0), base0 + 300.0),
          "RIT +500 on receiver 1: receiver 0 keeps its own +300 Hz");
    backend.setSliceRitEnabled(1, false);
    check(near(A::shiftHz(backend, 1), base1) && near(A::shiftHz(backend, 0), base0 + 300.0),
          "RIT off on receiver 1: receiver 0's RIT is untouched");
    backend.setSliceRitEnabled(0, false);
    backend.setSliceRitOffset(0, 0);
    backend.setSliceRitOffset(1, 0);

    // ---- a non-transmit receiver's XIT waits for transmit (#6105) ----
    backend.setSliceXitEnabled(0, true);
    backend.setSliceXitOffset(0, -300);
    backend.setSliceXitEnabled(1, true);
    backend.setSliceXitOffset(1, 200);
    check(A::txRegisterHz(backend) == 14'073'700u,
          "XIT +200 on receiver 1 (not transmitting): TX register keeps receiver 0's -300");
    backend.setTxSlice(1);
    check(A::txRegisterHz(backend) == 7'074'200u,
          "TX moved to receiver 1: TX register takes receiver 1's own XIT +200");
    backend.setSliceXitEnabled(1, false);
    check(A::txRegisterHz(backend) == 7'074'000u,
          "XIT off on the transmitting receiver 1: the setter rewrites the TX register to its dial");
    backend.setTxSlice(0);
    check(A::txRegisterHz(backend) == 14'073'700u,
          "TX back on receiver 0: its XIT -300 applies again");
    backend.setTxSlice(1);
    check(A::txRegisterHz(backend) == 7'074'000u,
          "handoff to a receiver with XIT off: TX register is its dial, not offset by the old slice's XIT");
    backend.setSliceXitEnabled(0, false);
    backend.setSliceXitOffset(0, 0);
    backend.setSliceXitOffset(1, 0);
    backend.setTxSlice(0);

    // ---- an offset that leaves the usable window moves the NCO register ----
    // 48 kHz -> usable half-window 19.2 kHz. Park the dial 19 kHz above the NCO;
    // +500 Hz of RIT puts the receive frequency outside it.
    const double nco0 = A::ncoHz(backend, 0);
    const double edgeDial = nco0 + 19'000.0;
    backend.setSliceFrequency(0, edgeDial);
    check(near(A::ncoHz(backend, 0), nco0), "edge dial is still inside the window");
    backend.setSliceRitEnabled(0, true);
    backend.setSliceRitOffset(0, 500);
    check(A::rx0RegisterHz(backend) == static_cast<std::uint32_t>(edgeDial + 500.0),
          "RIT past the window edge re-centres the NCO register on dial + RIT");
    check(near(A::shiftHz(backend, 0), 0.0), "…and the receive shift is then zero");
    check(A::txRegisterHz(backend) == static_cast<std::uint32_t>(edgeDial),
          "…while the TX register stays on the dial");
    check(publishedMhz && near(*publishedMhz * 1e6, edgeDial),
          "…and the published slice frequency stays the dial");

    // ---- ...and comes back when RIT is cleared ----
    // The NCO moved only because of RIT, so clearing RIT re-centres it on the
    // dial; otherwise the pan centre stays offset by the old RIT amount for the
    // rest of the session (|dial - NCO| = 500 Hz is well inside the window).
    backend.setSliceRitEnabled(0, false);
    check(A::rx0RegisterHz(backend) == static_cast<std::uint32_t>(edgeDial)
              && near(A::ncoHz(backend, 0), edgeDial),
          "RIT cleared: the NCO register returns to the dial");
    check(near(A::shiftHz(backend, 0), 0.0), "…and the receive shift is zero on the dial");
    backend.setSliceRitEnabled(0, true);
    check(near(A::ncoHz(backend, 0), edgeDial) && near(A::shiftHz(backend, 0), 500.0),
          "RIT back on inside the window: shift only, the NCO stays");

    // ---- RIT stays with its receiver when transmit moves (#6105) ----
    const double r0WithRit = A::shiftHz(backend, 0);
    const double r1Before = A::shiftHz(backend, 1);
    backend.setTxSlice(1);
    check(near(A::shiftHz(backend, 1), r1Before),
          "TX moved to receiver 1: receiver 1 does not pick up receiver 0's RIT");
    check(near(A::shiftHz(backend, 0), r0WithRit),
          "TX moved to receiver 1: receiver 0 keeps its RIT");
    backend.setSliceRitEnabled(0, false);
    check(near(A::shiftHz(backend, 0), r0WithRit - 500.0), "RIT off: receiver 0 restored");
    backend.setSliceRitOffset(0, 0);

    // ---- XIT must not walk the TX register through zero ----
    // The dial guard in setTxFrequency() is on the dial; XIT is added after it.
    // Receiver 1 owns transmit here. Park XIT at -9999 on a real dial first, so
    // a skipped write would leave a STALE register behind, then tune to 5 kHz.
    backend.setSliceXitEnabled(1, true);
    backend.setSliceXitOffset(1, -9999);
    check(A::txRegisterHz(backend) == 7'064'001u, "XIT -9999 on 7.074 MHz: TX register 7.064001 MHz");
    backend.setSliceFrequency(1, 5'000.0);
    const std::uint32_t lowTx = A::txRegisterHz(backend);
    check(lowTx != 0u, "dial 5 kHz + XIT -9999: TX register is not commanded to DC");
    check(lowTx != 7'064'001u, "dial 5 kHz + XIT -9999: TX register is not left on the old band");
    check(lowTx == 5'000u, "dial 5 kHz + XIT -9999: TX register holds the dial, XIT dropped");
    backend.setSliceFrequency(1, 20'000.0);
    check(A::txRegisterHz(backend) == 10'001u, "dial 20 kHz: XIT -9999 applies again");
    backend.setSliceXitEnabled(1, false);
    backend.setSliceXitOffset(1, 0);

    // ---- closing the transmit receiver hands TX to DDC 0, with DDC 0's own XIT ----
    // Receiver 1 owns transmit. Park DDC 0's dial near its window edge: its own
    // RIT moves its NCO at once, not on the hand-off. Then close receiver 1.
    const double ncoA = A::ncoHz(backend, 0);
    const double dialA = ncoA + 19'000.0;
    backend.setSliceFrequency(0, dialA);
    backend.setSliceRitEnabled(0, true);
    backend.setSliceRitOffset(0, 800);
    check(A::rx0RegisterHz(backend) == static_cast<std::uint32_t>(dialA + 800.0),
          "RIT on receiver 0 (not transmitting) moves its own NCO register");
    backend.setSliceXitEnabled(0, true);
    backend.setSliceXitOffset(0, 100);
    check(A::txRegisterHz(backend) == 20'000u,
          "XIT on receiver 0 (not transmitting) leaves the TX register on receiver 1");
    check(backend.removePanadapter(A::panIdOf(backend, 1)), "transmit receiver closes");
    check(A::txRegisterHz(backend) == static_cast<std::uint32_t>(dialA + 100.0),
          "TX receiver closed: the TX register follows DDC 0's dial plus DDC 0's XIT");
    check(A::rx0RegisterHz(backend) == static_cast<std::uint32_t>(dialA + 800.0),
          "TX receiver closed: DDC 0's RIT still on its NCO register");
    backend.setSliceXitEnabled(0, false);
    backend.setSliceRitEnabled(0, false);
    backend.setSliceRitOffset(0, 0);

    check(!A::mox(backend), "nothing keyed: MOX never set");

    // ---- closing receiver 0 renumbers receiver 1 to DDC 0; its RIT goes with it ----
    {
        Hl2Backend closing;
        A::twoReceivers(closing);
        closing.setSliceFrequency(0, kDial0);
        closing.setSliceFrequency(1, kDial1);
        closing.setSliceRitEnabled(0, true);
        closing.setSliceRitOffset(0, 300);
        closing.setSliceRitEnabled(1, true);
        closing.setSliceRitOffset(1, -700);
        const double shift1 = A::shiftHz(closing, 1);
        check(closing.removePanadapter(A::panIdOf(closing, 0)), "receiver 0 closes");
        check(A::receiverCount(closing) == 1 && A::ritOn(closing, 0) && A::ritHz(closing, 0) == -700,
              "receiver 0 closed: slice 1, now DDC 0, keeps its own RIT -700, not slice 0's +300");
        check(near(A::shiftHz(closing, 0), shift1),
              "receiver 0 closed: slice 1's receive shift is unchanged");
        check(!A::mox(closing), "closing case: MOX never set");
    }

    // ---- a receiver a (re)connect adds starts with RIT/XIT off ----
    // buildReceivers() carries existing receivers' state and seeds a new one
    // from receiver 0; the seed must not carry receiver 0's RIT/XIT.
    {
        Hl2Backend growing;
        growing.setSliceFrequency(0, kDial0);
        growing.setSliceRitEnabled(0, true);
        growing.setSliceFrequency(0, A::ncoHz(growing, 0) + 19'000.0);
        growing.setSliceRitOffset(0, 400);
        growing.setSliceXitEnabled(0, true);
        growing.setSliceXitOffset(0, -200);
        check(A::ncoMovedForRit(growing, 0), "build: receiver 0's RIT moved its NCO");
        A::buildReceivers(growing, 2);
        check(A::receiverCount(growing) == 2, "build: two receivers");
        check(A::ritOn(growing, 0) && A::ritHz(growing, 0) == 400 && A::xitOn(growing, 0)
                  && A::xitHz(growing, 0) == -200,
              "build: receiver 0 keeps its RIT +400 / XIT -200 across the rebuild");
        check(!A::ritOn(growing, 1) && A::ritHz(growing, 1) == 0 && !A::xitOn(growing, 1)
                  && A::xitHz(growing, 1) == 0,
              "build: the added receiver starts with RIT and XIT off, not receiver 0's");
        check(!A::ncoMovedForRit(growing, 1),
              "build: the added receiver does not inherit receiver 0's RIT-moved-NCO flag");
        check(!A::mox(growing), "build case: MOX never set");
    }

    std::printf("\n  %s (%d failure%s)\n", g_failures ? "FAILED" : "PASSED",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
