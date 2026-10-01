#pragma once

// CwSidetoneBackendPolicy — which sidetone backend AudioEngine constructs (#5713).
//
// Inputs: whether PortAudio was compiled in (HAVE_PORTAUDIO), the platform, and
// AppSettings["CwSidetoneBackend"]. A saved value that names a backend always
// wins; the platform supplies only the default for an unset install.
//
// Windows defaults to QAudioSink and Linux/macOS to PortAudio: the Windows
// PortAudio path corrupts the heap shortly after connect (#5713, root cause
// open). PortAudio stays reachable on Windows via an explicit setting so the
// fault can still be reproduced. This is a mitigation, not a fix.
//
// The saved value is matched case-insensitively and an unrecognised value means
// "unset", so a mistyped `--config set` lands on the safe platform default.
//
// Pure, header-only, no Qt types; the truth table is pinned by compile-time
// asserts and tests/cw_sidetone_backend_policy_test.cpp.

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
