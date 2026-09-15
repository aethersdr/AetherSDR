// Unit test for CwSidetoneBackendPolicy (#5713).
//
// Which sidetone backend AudioEngine constructs used to be one inline
// expression — `AppSettings.value("CwSidetoneBackend", "PortAudio") !=
// "QAudioSink"` — and it had no platform term at all. That was correct only
// because HAVE_PORTAUDIO was never defined on Windows, so the "PortAudio
// unless the operator opted out" default was unreachable there. #5200/#5201
// started shipping PortAudio in the Windows installer, which flipped the
// effective Windows default with no line of code saying so, and v26.9.3
// crashed at connect on three field boxes with STATUS_HEAP_CORRUPTION.
//
// The rows that are load-bearing, and the reason this file exists:
//
//   * Windows + PortAudio built + nothing saved -> QAudioSink   (the fix)
//   * Linux/macOS + nothing saved               -> PortAudio    (#4978 intact)
//   * Windows + saved "PortAudio"               -> PortAudio    (escape hatch:
//     the reporter needs it to reproduce #5713 and to run a diagnostic build)
//   * any platform + saved "qaudiosink"         -> QAudioSink   (the setting
//     has no GUI and is typed at a command line from a GitHub comment; a
//     case-sensitive compare left a crashing Windows box on the crashing
//     backend while its owner believed they had opted out)
//
// Pure policy, so no Qt, no audio backend, no device enumeration — and
// constexpr, so the whole table is also checked at compile time.

#include "core/CwSidetoneBackendPolicy.h"

#include <cstdio>

using namespace AetherSDR;

static int g_failures = 0;
static int g_checks = 0;

static const char* choiceName(SidetoneBackendChoice c)
{
    return c == SidetoneBackendChoice::PortAudio ? "PortAudio" : "QAudioSink";
}

#define EXPECT_CHOICE(built, windows, pref, expected, why) do { \
    ++g_checks; \
    const SidetoneBackendChoice got_ = sidetoneBackendChoice((built), (windows), (pref)); \
    if (got_ != (expected)) { \
        std::fprintf(stderr, "FAIL %s:%d  sidetoneBackendChoice(built=%d, windows=%d) " \
                             "= %s, expected %s — %s\n", \
                     __FILE__, __LINE__, int(built), int(windows), \
                     choiceName(got_), choiceName(expected), (why)); \
        ++g_failures; \
    } else { \
        std::printf("[ OK ] %s\n", (why)); \
    } \
} while (0)

#define EXPECT_PREF(saved, expected, why) do { \
    ++g_checks; \
    const SidetoneBackendPreference got_ = parseSidetoneBackendPreference(saved); \
    if (got_ != (expected)) { \
        std::fprintf(stderr, "FAIL %s:%d  parseSidetoneBackendPreference(\"%s\") " \
                             "= %d, expected %d — %s\n", \
                     __FILE__, __LINE__, (saved), int(got_), int(expected), (why)); \
        ++g_failures; \
    } else { \
        std::printf("[ OK ] %s\n", (why)); \
    } \
} while (0)

// ── Compile-time proof of both truth tables ─────────────────────────────────
// A regression that makes any of these false is a build error, not a field
// crash report.

constexpr auto kUnset = SidetoneBackendPreference::Unset;
constexpr auto kPa    = SidetoneBackendPreference::PortAudio;
constexpr auto kQt    = SidetoneBackendPreference::QAudioSink;

// The fix and its counterpart: the platform only decides an unset install.
static_assert(sidetoneBackendChoice(true, true,  kUnset) == SidetoneBackendChoice::QAudioSink,
              "#5713: a Windows install that has never set CwSidetoneBackend must get QAudioSink");
static_assert(sidetoneBackendChoice(true, false, kUnset) == SidetoneBackendChoice::PortAudio,
              "#4978: Linux/macOS keep the PortAudio callback path by default");

// An explicit preference wins on every platform, in both directions.
static_assert(sidetoneBackendChoice(true, true,  kPa) == SidetoneBackendChoice::PortAudio,
              "the Windows escape hatch must still reach PortAudio — #5713 is unfixed, not gone");
static_assert(sidetoneBackendChoice(true, false, kPa) == SidetoneBackendChoice::PortAudio,
              "explicit PortAudio on Linux/macOS is the default restated");
static_assert(sidetoneBackendChoice(true, true,  kQt) == SidetoneBackendChoice::QAudioSink,
              "explicit QAudioSink on Windows is the default restated");
static_assert(sidetoneBackendChoice(true, false, kQt) == SidetoneBackendChoice::QAudioSink,
              "explicit QAudioSink on Linux/macOS must opt out of PortAudio");

// Without HAVE_PORTAUDIO there is only one sink, and no preference conjures one.
static_assert(sidetoneBackendChoice(false, true,  kPa)    == SidetoneBackendChoice::QAudioSink, "");
static_assert(sidetoneBackendChoice(false, false, kPa)    == SidetoneBackendChoice::QAudioSink, "");
static_assert(sidetoneBackendChoice(false, true,  kUnset) == SidetoneBackendChoice::QAudioSink, "");
static_assert(sidetoneBackendChoice(false, false, kUnset) == SidetoneBackendChoice::QAudioSink, "");
static_assert(sidetoneBackendChoice(false, true,  kQt)    == SidetoneBackendChoice::QAudioSink, "");
static_assert(sidetoneBackendChoice(false, false, kQt)    == SidetoneBackendChoice::QAudioSink, "");

static_assert(parseSidetoneBackendPreference("") == kUnset,
              "an install that never wrote the key is unset");
static_assert(parseSidetoneBackendPreference("PortAudio") == kPa, "");
static_assert(parseSidetoneBackendPreference("QAudioSink") == kQt, "");
static_assert(parseSidetoneBackendPreference("qaudiosink") == kQt,
              "typed at a command line: case must not decide whether a box crashes");
static_assert(parseSidetoneBackendPreference("PORTAUDIO") == kPa, "");
static_assert(parseSidetoneBackendPreference("Qt") == kUnset,
              "an unrecognised value is unset, so it lands on the platform default");
static_assert(parseSidetoneBackendPreference("QAudioSink2") == kUnset,
              "a longer string that starts with a backend name is not that backend");

int main()
{
    // ── The regression, stated as its own row ──────────────────────────────
    // v26.9.2 Windows: no PortAudio in the build at all.
    // v26.9.3 Windows: PortAudio built, and this row is what changed.
    EXPECT_CHOICE(false, true, kUnset, SidetoneBackendChoice::QAudioSink,
        "Windows v26.9.2 (no PortAudio in the build), nothing saved -> QAudioSink");
    EXPECT_CHOICE(true, true, kUnset, SidetoneBackendChoice::QAudioSink,
        "Windows v26.9.3+ (PortAudio built), nothing saved -> QAudioSink, same as v26.9.2 (the #5713 fix)");

    // ── Linux/macOS are untouched by the fix ───────────────────────────────
    EXPECT_CHOICE(true, false, kUnset, SidetoneBackendChoice::PortAudio,
        "Linux/macOS, PortAudio built, nothing saved -> PortAudio (callback path, #4978)");
    EXPECT_CHOICE(false, false, kUnset, SidetoneBackendChoice::QAudioSink,
        "Linux/macOS without portaudio19-dev -> QAudioSink, nothing else to pick");

    // ── Both escape hatches ────────────────────────────────────────────────
    EXPECT_CHOICE(true, true, kPa, SidetoneBackendChoice::PortAudio,
        "Windows + CwSidetoneBackend=PortAudio -> PortAudio (reproduce #5713 / run the diagnostic build)");
    EXPECT_CHOICE(true, false, kQt, SidetoneBackendChoice::QAudioSink,
        "Linux/macOS + CwSidetoneBackend=QAudioSink -> QAudioSink (the pre-existing opt-out)");
    EXPECT_CHOICE(false, true, kPa, SidetoneBackendChoice::QAudioSink,
        "asking for PortAudio in a build without it -> QAudioSink, not a crash");

    // ── Parsing: what an operator actually types ───────────────────────────
    EXPECT_PREF("", kUnset, "unset key -> Unset");
    EXPECT_PREF("QAudioSink", kQt, "the documented spelling -> QAudioSink");
    EXPECT_PREF("qaudiosink", kQt, "lower case -> QAudioSink (would have kept a Windows box crashing)");
    EXPECT_PREF("PortAudio", kPa, "the documented spelling -> PortAudio");
    EXPECT_PREF("portaudio", kPa, "lower case -> PortAudio");
    EXPECT_PREF("nonsense", kUnset, "garbage -> Unset -> platform default (safe on Windows)");

    // ── The escape hatch, end to end, on the reporting operator's box ──────
    // scott-mss ran `AetherSDR.exe --config set CwSidetoneBackend QAudioSink`
    // on Windows with a v26.9.3 build and stopped crashing; the same box with
    // `PortAudio` crashed again. Both directions are pinned so neither the
    // mitigation nor the reproduction path can be refactored away while the
    // root cause is open.
    {
        ++g_checks;
        const auto mitigated = sidetoneBackendChoice(
            true, true, parseSidetoneBackendPreference("QAudioSink"));
        const auto reproduced = sidetoneBackendChoice(
            true, true, parseSidetoneBackendPreference("PortAudio"));
        if (mitigated != SidetoneBackendChoice::QAudioSink
            || reproduced != SidetoneBackendChoice::PortAudio) {
            std::fprintf(stderr, "FAIL %s:%d  the #5713 A/B must stay reachable from the "
                                 "command line in both directions\n", __FILE__, __LINE__);
            ++g_failures;
        } else {
            std::printf("[ OK ] #5713 A/B: saved QAudioSink -> QAudioSink, saved PortAudio -> PortAudio, on Windows\n");
        }
    }

    std::printf("%d/%d checks passed\n", g_checks - g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
}
