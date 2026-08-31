// The HL2 RX mode table (Plan 2). Pins the operator-mode-string -> WDSP-mode +
// passband map, the restore-boundary guard, and the authoritative supported
// list. Pure: no connected backend, no WDSP channel.
//
// The property that matters: NOTHING falls silently through to a plain USB
// passband. Every mode the backend advertises resolves to a real demod + a
// passband that is not the {150, 3000} USB fallback, and D-STAR / DRM / FreeDV
// are not advertised at all.

#include "core/backends/hl2/Hl2ModeTable.h"

#include <cstdio>

using namespace AetherSDR::hl2;
using Mode = WdspChannel::Mode;

namespace {

int failures = 0;

void check(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) {
        ++failures;
    }
}

bool isUsbFallbackPassband(const QString& mode)
{
    const auto pb = defaultPassbandForMode(mode);
    return pb.first == 150 && pb.second == 3000;
}

} // namespace

int main()
{
    // --- the supported list is exactly the raw-IQ-honourable set ------------
    const QStringList supported = supportedModes();
    check(supported.contains("RTTY") && supported.contains("DFM"),
          "RTTY and DFM are advertised");
    check(!supported.contains("DSTR") && !supported.contains("DRM")
              && !supported.contains("FDVU") && !supported.contains("FDVL")
              && !supported.contains("FREEDV") && !supported.contains("RADE"),
          "D-STAR / DRM / FreeDV / RADE are NOT advertised");
    for (const QString& m : supported) {
        check(isKnownModeString(m),
              qUtf8Printable(QStringLiteral("advertised mode %1 is a known mode string").arg(m)));
        // USB and DIGU legitimately share the {150,3000} shape; every other
        // advertised mode must have carved out its own entry rather than hit
        // the fallback line.
        check(!isUsbFallbackPassband(m) || m == "USB" || m == "DIGU",
              qUtf8Printable(QStringLiteral("advertised mode %1 has its own passband, not the USB fallback").arg(m)));
    }

    // --- RTTY: filtered USB slice, narrow window over the decoder tones -----
    check(modeFromString("RTTY") == Mode::Usb,
          "RTTY demodulates as USB (the Baudot decode is client-side)");
    {
        // The contract (Plan 2.2): a narrow window around the decoder's default
        // mark (2125 Hz) / 170 Hz shift, wide enough for either polarity
        // (space at 1955 or 2295) and no wider -- distinctly tighter than an
        // SSB voice passband, so an adjacent RTTY signal does not bleed in.
        constexpr int kMark = 2125;
        constexpr int kShift = 170;
        const auto pb = defaultPassbandForMode("RTTY");
        const int width = pb.second - pb.first;
        check(!isUsbFallbackPassband("RTTY"),
              "RTTY has its own passband, not the USB fallback");
        check(pb.first <= (kMark - kShift) && pb.second >= (kMark + kShift),
              "RTTY passband covers both mark and space in either polarity");
        check(width > kShift && width <= 450,
              "RTTY passband is wider than the shift but far tighter than SSB");
    }
    check(isKnownModeString("RTTY"), "RTTY survives the restore boundary");

    // --- DFM: FM demod, FM-width passband (was SSB fallback) ---------------
    check(modeFromString("DFM") == Mode::Fm,
          "DFM demodulates as FM, not SSB");
    {
        const auto pb = defaultPassbandForMode("DFM");
        check(pb == defaultPassbandForMode("FM"),
              "DFM shares FM's passband");
        check(pb.first < 0 && pb.second > 0,
              "DFM passband straddles the carrier like FM");
    }
    check(isKnownModeString("DFM"), "DFM survives the restore boundary");

    // --- DRM is firmly not a thing on this radio (no decoder anywhere) -----
    check(modeFromString("DRM") == Mode::Usb,
          "DRM is no longer mapped — it falls back to USB if forced");
    check(isUsbFallbackPassband("DRM"), "DRM gets no dedicated passband");
    check(!isKnownModeString("DRM"),
          "a restored DRM slice is dropped, not honoured");

    // --- regression: the modes that already worked still map the same ------
    check(modeFromString("USB") == Mode::Usb && modeFromString("LSB") == Mode::Lsb
              && modeFromString("CW") == Mode::Cwu && modeFromString("CWL") == Mode::Cwl
              && modeFromString("AM") == Mode::Am && modeFromString("SAM") == Mode::Sam
              && modeFromString("FM") == Mode::Fm && modeFromString("NFM") == Mode::Fm
              && modeFromString("DIGU") == Mode::Digu && modeFromString("DIGL") == Mode::Digl
              && modeFromString("DSB") == Mode::Dsb
              && modeFromString("WFM") == Mode::Wbfm,
          "the pre-Plan-2 mode map is unchanged");
    check(modeFromString("NONSENSE") == Mode::Usb,
          "an unknown mode still falls back to USB (the last resort)");
    check(!isKnownModeString("NONSENSE"),
          "an unknown mode is not a known mode string");

    // --- wdspModeName round-trips for the 1:1 values -----------------------
    check(wdspModeName(Mode::Usb) == "USB" && wdspModeName(Mode::Cwu) == "CWU"
              && wdspModeName(Mode::Fm) == "FM" && wdspModeName(Mode::Digl) == "DIGL",
          "wdspModeName maps the enum back to canonical names");

    std::printf("%s\n", failures == 0 ? "ALL PASS" : "SOME FAILED");
    return failures == 0 ? 0 : 1;
}
