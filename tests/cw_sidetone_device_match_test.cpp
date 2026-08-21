// CwSidetoneDeviceMatch: the name rule that maps the operator's explicit Qt
// output selection onto a PortAudio device (#5123).
//
// The inputs are measured strings: the 13-device ALSA-only PortAudio list and
// Qt/PipeWire descriptions captured on the Linux box in #4978, plus the
// Windows MME truncation shape #3193 depends on.  No PortAudio init, no
// device — the rule is a pure predicate over two strings.

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
    expectMatch("hdmi", "Built-in Audio Digital Stereo (HDMI)", DeviceNameMatch::None,
                "interior substring of the description is not a match");
    expectMatch("hdmi", "GA104 High Definition Audio Controller Digital Stereo (HDMI 2)",
                DeviceNameMatch::None, "interior substring of the description is not a match");
    expectMatch("pulse", "Built-in Audio Analog Stereo", DeviceNameMatch::None, "unrelated plugin");
    expectMatch("default", "Scarlett 2i2 USB Analog Stereo", DeviceNameMatch::None, "unrelated plugin");
    expectMatch("sysdefault", "Built-in Audio Analog Stereo", DeviceNameMatch::None, "unrelated plugin");
    expectMatch("pipewire", "Built-in Audio Digital Stereo (HDMI)", DeviceNameMatch::None, "unrelated plugin");
    expectMatch("iec958", "Built-in Audio Digital Stereo (HDMI)", DeviceNameMatch::None, "unrelated plugin");
    expectMatch("dmix", "Built-in Audio Analog Stereo", DeviceNameMatch::None, "unrelated plugin");
    // A short token that happens to START the description is still a prefix
    // match by the rule below — the rule is about direction, not length.
    expectMatch("HDA NVidia: HDMI 0 (hw:0,3)", "Built-in Audio Digital Stereo (HDMI)",
                DeviceNameMatch::None, "different vocabulary, no containment either way");

    // Preserved shapes.
    // PortAudio decorates the name: description is contained in the PA name.
    expectMatch("Scarlett 2i2 USB Analog Stereo (hw:1,0)", "Scarlett 2i2 USB Analog Stereo",
                DeviceNameMatch::Partial, "PortAudio-decorated name contains the description");
    // Windows MME truncates to 31 chars: PA name is a PREFIX of the description.
    expectMatch("Speakers (Realtek(R) Audio)", "Speakers (Realtek(R) Audio) - 2- High Definition Audio",
                DeviceNameMatch::Partial, "MME truncation is a prefix (#3193)");
    // Exact after normalisation (whitespace, case).
    expectMatch("Built-in Audio Analog Stereo", "Built-in  Audio Analog Stereo",
                DeviceNameMatch::Exact, "simplified() folds double spaces");
    expectMatch("MACBOOK PRO SPEAKERS", "MacBook Pro Speakers",
                DeviceNameMatch::Exact, "toCaseFolded() folds case");
    // Empty inputs never match.
    expectMatch("", "Built-in Audio Analog Stereo", DeviceNameMatch::None, "empty PA name");
    expectMatch("hdmi", "", DeviceNameMatch::None, "empty description");

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
