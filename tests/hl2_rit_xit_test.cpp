// HL2 RIT / XIT (#5386) — receive and transmit offsets reach the registers.
//
// The RIT/XIT controls were wired all the way to IRadioBackend's
// setRitEnabled/setRitOffset/setXitEnabled/setXitOffset, whose defaults are
// no-ops, and Hl2Backend overrode none of them: the button toggled, the readout
// moved, and the radio heard and transmitted exactly where it did before.
//
// What this pins, on the HL2's two independent paths:
//
//   receive   the transmit-owning receiver's WDSP shift (and, when the offset
//             leaves the usable window, its NCO register) moves by the RIT
//             offset; no other receiver moves.
//   transmit  the TX NCO register (C&C addr 0x01) moves by the XIT offset and
//             by nothing else — RIT must not reach it, and XIT must not alias
//             onto RIT (the seam's default setXitOffset forwards to
//             setRitOffset, which is right only for a one-register radio).
//   display   the published slice frequency stays the dial frequency.
//
// SOCKET-FREE and UNKEYED: a constructed backend with no connectRadio(), no
// peer and no DSP. The TX register is read back from MetisClient's own C&C
// bank; oscillator setup keys nothing, and the test asserts MOX stays clear.

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
    std::printf("\n  HL2 RIT / XIT (#5386)\n\n");

    Hl2Backend backend;
    A::twoReceivers(backend);

    std::optional<double> publishedMhz;
    QObject::connect(&backend, &IRadioBackend::sliceChanged, &backend,
                     [&](int sliceId, const SliceDelta& d) {
                         if (sliceId == 0 && d.frequency)
                             publishedMhz = *d.frequency;
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
    backend.setRitEnabled(true);
    backend.setRitOffset(500);
    check(near(A::shiftHz(backend, 0), base0 + 500.0),
          "RIT +500: TX receiver's receive shift moves +500 Hz");
    check(A::txRegisterHz(backend) == 14'074'000u,
          "RIT +500: TX register does not move");
    check(near(A::shiftHz(backend, 1), base1) && near(A::ncoHz(backend, 1), nco1),
          "RIT +500: second receiver is unaffected");
    check(near(A::sliceHz(backend, 0), kDial0), "RIT +500: slice state stays the dial");
    check(publishedMhz && near(*publishedMhz * 1e6, kDial0),
          "RIT +500: published slice frequency stays the dial");

    backend.setRitEnabled(false);
    check(near(A::shiftHz(backend, 0), base0), "RIT off: receive shift restored");

    // ---- XIT -300 Hz ----
    backend.setXitEnabled(true);
    backend.setXitOffset(-300);
    check(A::txRegisterHz(backend) == 14'073'700u, "XIT -300: TX register moves -300 Hz");
    check(near(A::shiftHz(backend, 0), base0), "XIT -300: receive shift does not move");

    // Both on at once: two registers, no aliasing either way.
    backend.setRitEnabled(true);
    backend.setRitOffset(500);
    check(near(A::shiftHz(backend, 0), base0 + 500.0) && A::txRegisterHz(backend) == 14'073'700u,
          "RIT +500 with XIT -300: each offset reaches only its own path");
    backend.setRitEnabled(false);

    backend.setXitEnabled(false);
    check(A::txRegisterHz(backend) == 14'074'000u, "XIT off: TX register restored");

    // ---- clamp to the app's ±9999 Hz (SmartCatProtocol kRitMaxHz) ----
    backend.setRitEnabled(true);
    backend.setRitOffset(20'000);
    check(near(A::shiftHz(backend, 0), base0 + 9999.0), "RIT offset clamps to +9999 Hz");
    backend.setXitEnabled(true);
    backend.setXitOffset(-20'000);
    check(A::txRegisterHz(backend) == 14'064'001u, "XIT offset clamps to -9999 Hz");
    backend.setXitEnabled(false);
    backend.setRitEnabled(false);
    backend.setRitOffset(0);   // the next case starts from no stored offset

    // ---- an offset that leaves the usable window moves the NCO register ----
    // 48 kHz -> usable half-window 19.2 kHz. Park the dial 19 kHz above the NCO;
    // +500 Hz of RIT puts the receive frequency outside it.
    const double nco0 = A::ncoHz(backend, 0);
    const double edgeDial = nco0 + 19'000.0;
    backend.setSliceFrequency(0, edgeDial);
    check(near(A::ncoHz(backend, 0), nco0), "edge dial is still inside the window");
    backend.setRitEnabled(true);
    backend.setRitOffset(500);
    check(A::rx0RegisterHz(backend) == static_cast<std::uint32_t>(edgeDial + 500.0),
          "RIT past the window edge re-centres the NCO register on dial + RIT");
    check(near(A::shiftHz(backend, 0), 0.0), "…and the receive shift is then zero");
    check(A::txRegisterHz(backend) == static_cast<std::uint32_t>(edgeDial),
          "…while the TX register stays on the dial");
    check(publishedMhz && near(*publishedMhz * 1e6, edgeDial),
          "…and the published slice frequency stays the dial");

    // ---- RIT follows the transmit-owning receiver ----
    const double r0WithRit = A::shiftHz(backend, 0);
    const double r1Before = A::shiftHz(backend, 1);
    backend.setTxSlice(1);
    check(near(A::shiftHz(backend, 1), r1Before + 500.0),
          "TX moved to receiver 1: RIT now shifts receiver 1");
    check(near(A::shiftHz(backend, 0), r0WithRit - 500.0),
          "TX moved to receiver 1: receiver 0 no longer carries RIT");
    backend.setRitEnabled(false);
    check(near(A::shiftHz(backend, 1), r1Before), "RIT off: receiver 1 restored");

    check(!A::mox(backend), "nothing keyed: MOX never set");

    std::printf("\n  %s (%d failure%s)\n", g_failures ? "FAILED" : "PASSED",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
