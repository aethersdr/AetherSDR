#pragma once

// CwSidetoneBackendPolicy — which sidetone backend does AudioEngine construct?
// (#5713)
//
// Three facts decide it: whether a PortAudio sink was compiled in at all
// (HAVE_PORTAUDIO), which platform this is, and what the operator saved in
// AppSettings["CwSidetoneBackend"]. The operator's saved value always wins when
// it names a backend; the platform only supplies the DEFAULT for an install
// that has never set it.
//
// ── Why the default is platform-dependent ───────────────────────────────────
//
// v26.9.3 was the first Windows build to ship PortAudio at all (#5200 / #5201
// added scripts/setup/setup-portaudio.ps1 to the installer workflow and made
// the build fail without it). Before that, HAVE_PORTAUDIO was never defined on
// Windows, so `makeSidetoneBackend`'s "PortAudio unless the operator opted out"
// default was unreachable there and every Windows install ran the QAudioSink
// push path. Defining HAVE_PORTAUDIO flipped the effective Windows default
// without anyone choosing to, and three field operators on v26.9.3 hit
// STATUS_HEAP_CORRUPTION (0xc0000374, raised by ntdll's heap manager) a few
// hundred ms after connect — startSidetoneStream() is unconditional at the tail
// of startRxStream(), so Pa_Initialize() and a WASAPI open run on every connect
// whether or not the operator uses CW.
//
// The reporting operator's A/B is the evidence: CwSidetoneBackend=QAudioSink
// survives, CwSidetoneBackend=PortAudio crashes, on the same install, twice,
// with two different output devices selected. So the corrupting write is in the
// Windows PortAudio path and is NOT specific to one endpoint.
//
// This restores the v26.9.2 Windows behaviour exactly: Windows defaults to
// QAudioSink, Linux and macOS keep PortAudio. Those two have shipped the
// callback path as their default since #4978 with no crash of this class, and
// the sub-5 ms latency it buys is real, so the fault is quarantined to the
// platform that has it rather than paid for everywhere.
//
// PortAudio stays reachable on Windows via an explicit
// `CwSidetoneBackend=PortAudio`. That is deliberate: it is the escape hatch a
// reporter needs to reproduce #5713 and to run the diagnostic build, and the
// root cause is still open. This is a mitigation, not a fix.
//
// ── Why the saved value is matched case-insensitively ───────────────────────
//
// `CwSidetoneBackend` has no GUI; the only way to set it is
// `AetherSDR.exe --config set CwSidetoneBackend QAudioSink`, typed by an
// operator reading it off a GitHub comment. The previous inline rule was
// `pref != "QAudioSink" -> PortAudio`, which means a typed `qaudiosink` left a
// crashing Windows box on the crashing backend while its owner believed they
// had opted out. An unrecognised value now means "unset" and lands on the
// platform default, which on Windows is the safe one. On Linux and macOS the
// outcome is unchanged for every value: named or not, a non-QAudioSink string
// still resolves to PortAudio.
//
// Pure and header-only, with no Qt types, so the whole truth table is a
// compile-time assertion and a unit-test row (tests/cw_sidetone_backend_policy_test.cpp)
// — the same shape as CwSidetoneStartPolicy.h next door, which decides what the
// chosen backend is then handed at start().

#include <string_view>

namespace AetherSDR {

enum class SidetoneBackendChoice {
    PortAudio,
    QAudioSink,
};

// What the operator saved, once parsed. `Unset` covers both "never written"
// and "written but unrecognised" — see the case-insensitivity note above.
enum class SidetoneBackendPreference {
    Unset,
    PortAudio,
    QAudioSink,
};

namespace detail {
constexpr char asciiLower(char c)
{
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

constexpr bool asciiIEquals(std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (asciiLower(a[i]) != asciiLower(b[i]))
            return false;
    }
    return true;
}
} // namespace detail

// `saved`  the raw AppSettings["CwSidetoneBackend"] string, empty when unset.
constexpr SidetoneBackendPreference parseSidetoneBackendPreference(std::string_view saved)
{
    if (detail::asciiIEquals(saved, "PortAudio"))
        return SidetoneBackendPreference::PortAudio;
    if (detail::asciiIEquals(saved, "QAudioSink"))
        return SidetoneBackendPreference::QAudioSink;
    return SidetoneBackendPreference::Unset;
}

// `portAudioBuilt`     HAVE_PORTAUDIO — a CwSidetonePortAudioSink can be
//                      constructed at all. False on any build without it, and
//                      on every Windows build before v26.9.3.
// `platformIsWindows`  Q_OS_WIN. Enters only as the source of the default, so
//                      an operator who names a backend gets it on every
//                      platform.
// `preference`         parseSidetoneBackendPreference(...) above.
constexpr SidetoneBackendChoice sidetoneBackendChoice(bool portAudioBuilt,
                                                      bool platformIsWindows,
                                                      SidetoneBackendPreference preference)
{
    // Nothing to choose: without HAVE_PORTAUDIO there is only one sink, and an
    // explicit PortAudio preference cannot conjure one.
    if (!portAudioBuilt)
        return SidetoneBackendChoice::QAudioSink;

    switch (preference) {
    case SidetoneBackendPreference::PortAudio:
        return SidetoneBackendChoice::PortAudio;
    case SidetoneBackendPreference::QAudioSink:
        return SidetoneBackendChoice::QAudioSink;
    case SidetoneBackendPreference::Unset:
        break;
    }

    // #5713: Windows defaults to the push path it had through v26.9.2.
    return platformIsWindows ? SidetoneBackendChoice::QAudioSink
                             : SidetoneBackendChoice::PortAudio;
}

} // namespace AetherSDR
