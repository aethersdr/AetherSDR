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
// Partial is deliberately asymmetric:
//   - paName.contains(qtDescription): PortAudio decorates the name
//     ("Scarlett 2i2 USB Analog Stereo (hw:1,0)").
//   - paName is a PREFIX of qtDescription: Windows MME truncates device
//     names to 31 characters, so the PortAudio name is the head of the
//     fuller Qt description (#3193 relies on those MME rows being candidates).
// An interior substring in the second direction is NOT a match.  That is what
// let the 4-character ALSA plugin "hdmi" claim "Built-in Audio Digital Stereo
// (HDMI)" and route the sidetone to whichever card defaults.pcm.iec958.card
// names — a different physical port than the one selected (#5123).
inline DeviceNameMatch classifyDeviceNameMatch(const QString& paName,
                                               const QString& qtDescription)
{
    const QString cand = normalizedDeviceName(paName);
    const QString target = normalizedDeviceName(qtDescription);
    if (cand.isEmpty() || target.isEmpty())
        return DeviceNameMatch::None;
    if (cand == target)
        return DeviceNameMatch::Exact;
    if (cand.contains(target) || target.startsWith(cand))
        return DeviceNameMatch::Partial;
    return DeviceNameMatch::None;
}

} // namespace AetherSDR
