#pragma once

#include <QString>

namespace AetherSDR {

// How a PortAudio device name relates to the Qt output description the
// operator selected in Radio Setup.  Pure string predicate — no PortAudio,
// no Qt Multimedia — so the rule is unit-testable against captured device
// lists without audio hardware (#5123).
enum class DeviceNameMatch { None, Exact, Partial };

inline QString normalizedDeviceName(QString name)
{
    return name.simplified().toCaseFolded();
}

// paName: PaDeviceInfo::name.  qtDescription: QAudioDevice::description().
//
// Partial is a PREFIX test in both directions — neither side may match an
// interior substring:
//   - paName is a prefix of qtDescription: Windows MME truncates device
//     names to 31 characters, so the PortAudio name is the head of the
//     fuller Qt description (#3193 relies on those MME rows being candidates).
//   - qtDescription is a prefix of paName: retained for a PortAudio host that
//     appends decoration to the description it shares with Qt.  No captured
//     device list in #4978 / #5123 / #5135 holds such a pair — macOS reports
//     identical strings, and ALSA's form is "<card>: <pcm> (hw:X,Y)" with the
//     card name first — so the unit test does not pin this arm.
// Interior substrings are what let the 4-character ALSA plugin "hdmi" claim
// "Built-in Audio Digital Stereo (HDMI)" and route the sidetone to whichever
// card defaults.pcm.iec958.card names — a different physical port than the
// one selected (#5123).  The mirror image — a long PortAudio name claiming a
// short Qt description it happens to contain — is closed by the first arm
// being a prefix test as well.  Should a device exist that a prefix test
// refuses, the cost is a fallback to QAudioSink — the wrong-timing path with
// the right speakers — rather than a confident match on the wrong port, which
// is the failure #5123 is about.
inline DeviceNameMatch classifyDeviceNameMatch(const QString& paName,
                                               const QString& qtDescription)
{
    const QString cand = normalizedDeviceName(paName);
    const QString target = normalizedDeviceName(qtDescription);
    if (cand.isEmpty() || target.isEmpty())
        return DeviceNameMatch::None;
    if (cand == target)
        return DeviceNameMatch::Exact;
    if (cand.startsWith(target) || target.startsWith(cand))
        return DeviceNameMatch::Partial;
    return DeviceNameMatch::None;
}

} // namespace AetherSDR
