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

// paName: PaDeviceInfo::name. qtDescription: QAudioDevice::description().
// Partial is a PREFIX test in both directions, never an interior substring:
//   - paName prefixes qtDescription: Windows MME truncates names to 31 chars
//     (#3193 relies on those rows).
//   - qtDescription prefixes paName: for a host that decorates the shared name;
//     no captured list shows one, so the test doesn't pin this arm.
// Interior matching let ALSA's "hdmi" claim "Built-in Audio Digital Stereo
// (HDMI)" and route to the wrong port (#5123). A device a prefix test refuses
// falls back to QAudioSink (right speakers, worse timing).
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
