// CwSidetoneDeviceMatch: the name rule that maps the operator's explicit Qt
// output selection onto a PortAudio device (#5123).
//
// Every device string below is a claim about the world and carries its
// provenance:
//   MEASURED    — taken from a captured device list (the 13-device ALSA-only
//                 PortAudio list and the Qt/PipeWire descriptions from the
//                 Linux box in #4978; the #5123 replay table).
//   SOURCED     — the string is quoted from the code or config that produces
//                 it, not from a captured list.
//   CONSTRUCTED — not a real device name; exercises only the normalisation
//                 (whitespace, case) or the empty-input path, never a claim
//                 that the shape exists in the field.
// No PortAudio init, no device — the rule is a pure predicate over two
// strings.

#include "core/CwSidetoneDeviceMatch.h"

#include <cstdio>

using namespace AetherSDR;

namespace {

int g_failures = 0;
int g_checks = 0;

const char* name(DeviceNameMatch m)
{
    switch (m) {
    case DeviceNameMatch::None:    return "None";
    case DeviceNameMatch::Exact:   return "Exact";
    case DeviceNameMatch::Partial: return "Partial";
    }
    return "?";
}

void expectMatch(const char* paName, const char* qtDescription,
                 DeviceNameMatch expected, const char* why)
{
    ++g_checks;
    const DeviceNameMatch got =
        classifyDeviceNameMatch(QString::fromUtf8(paName), QString::fromUtf8(qtDescription));
    if (got == expected) {
        std::printf("[ OK ] %-8s pa=\"%s\" qt=\"%s\"\n", name(expected), paName, qtDescription);
    } else {
        std::printf("[FAIL] pa=\"%s\" qt=\"%s\": got %s, expected %s — %s\n",
                    paName, qtDescription, name(got), name(expected), why);
        ++g_failures;
    }
}

} // namespace

int main()
{
    // #5123: degenerate ALSA plugin names must not claim a long Qt description.
    // Both replay-table rows from the issue resolve to None -> paNoDevice ->
    // the documented QAudioSink fallback, not to the `hdmi` plugin.
    // MEASURED: PortAudio names are #4978's list (index 9 `hdmi`, 10
    // `pipewire`, 11 `pulse`, 12 `default`, 0 `HDA NVidia: HDMI 0 (hw:0,3)`);
    // Qt descriptions are the #5123 replay table.
    expectMatch("hdmi", "Built-in Audio Digital Stereo (HDMI)", DeviceNameMatch::None,
                "interior substring of the description is not a match");
    expectMatch("hdmi", "GA104 High Definition Audio Controller Digital Stereo (HDMI 2)",
                DeviceNameMatch::None, "interior substring of the description is not a match");
    expectMatch("pulse", "Built-in Audio Analog Stereo", DeviceNameMatch::None, "unrelated plugin");
    expectMatch("default", "Scarlett 2i2 USB Analog Stereo", DeviceNameMatch::None, "unrelated plugin");
    expectMatch("pipewire", "Built-in Audio Digital Stereo (HDMI)", DeviceNameMatch::None, "unrelated plugin");
    expectMatch("HDA NVidia: HDMI 0 (hw:0,3)", "Built-in Audio Digital Stereo (HDMI)",
                DeviceNameMatch::None, "different vocabulary, no prefix either way");

    // A short token that happens to START the description is still a prefix
    // match — the rule is about direction, not length.
    // SOURCED, not captured: Qt's plain-ALSA backend reports the ALSA hint
    // DESC verbatim as description() (qtmultimedia 6.8,
    // src/multimedia/alsa/qalsaaudiodevices.cpp:58-62), and alsa-plugins
    // names its `pulse` PCM "PulseAudio Sound Server"
    // (pulse/50-pulseaudio.conf:16).  That IS the pulse plugin, so matching
    // it is correct.  No captured list from a plain-ALSA Qt backend exists.
    expectMatch("pulse", "PulseAudio Sound Server", DeviceNameMatch::Partial,
                "short PA name that is a prefix of the description");

    // Shared product string, prefix in neither direction.
    // Published in our #4978 comment of 2026-08-18 (Linux box): ALSA's form
    // is "<card>: <pcm> (hw:X,Y)" while Qt reports the PulseAudio
    // description.  That enumeration is NOT the 13-device list captured in
    // the same comment (no card 2 there); the capture that would put this
    // row in the MEASURED class is the Linux box with the Scarlett attached.
    expectMatch("Scarlett 2i2 USB: Audio (hw:2,0)", "Scarlett 2i2 USB Analog Stereo",
                DeviceNameMatch::None, "shared product string, prefix in neither direction");

    // Windows MME truncates to 31 chars: PA name is a PREFIX of the description
    // (#3193 relies on those MME rows being candidates).
    // Qt side MEASURED by NF0T on his Windows box (PR #5135 review,
    // 2026-08-26).  PA side CONSTRUCTED: it is the Qt description our own
    // Windows box reports (#4978, 2026-08-20) standing in for an MME name;
    // the real MME truncation of the Qt string would be the 31-character
    // "Speakers (Realtek(R) Audio) - 2", which no published list holds.
    expectMatch("Speakers (Realtek(R) Audio)", "Speakers (Realtek(R) Audio) - 2- High Definition Audio",
                DeviceNameMatch::Partial, "MME truncation is a prefix (#3193)");

    // The other Partial arm — the Qt description as a PREFIX of the PortAudio
    // name — has no row: no captured device list holds such a pair (see the
    // header comment in CwSidetoneDeviceMatch.h).  Nothing here pins it.

    // Exact after normalisation.  CONSTRUCTED: the base strings are the
    // measured Linux "Built-in Audio Analog Stereo" (#4978) and the measured
    // macOS "MacBook Pro Speakers" (#4978, 2026-08-20); the doubled space and
    // the all-caps form are not real device names and exercise only
    // simplified() and toCaseFolded().
    expectMatch("Built-in Audio Analog Stereo", "Built-in  Audio Analog Stereo",
                DeviceNameMatch::Exact, "simplified() folds double spaces");
    expectMatch("MACBOOK PRO SPEAKERS", "MacBook Pro Speakers",
                DeviceNameMatch::Exact, "toCaseFolded() folds case");

    // Empty inputs never match.  CONSTRUCTED.
    expectMatch("", "Built-in Audio Analog Stereo", DeviceNameMatch::None, "empty PA name");
    expectMatch("hdmi", "", DeviceNameMatch::None, "empty description");

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
