#pragma once

namespace AetherSDR {

// The local helper uses the Flex waveform API. Keep disconnected browsing
// permissive, but never advertise it in a build that omits the helper.
inline constexpr bool dstarTabAvailable(bool connected, bool hasWaveforms,
                                        bool helperAvailable)
{
    return helperAvailable && (!connected || hasWaveforms);
}

// Re-evaluate at timer delivery: a session can disconnect, switch backend,
// or become WAN while an autostart request is pending. The helper needs a
// directly reachable waveform-capable radio.
inline constexpr bool dstarServiceCanStart(bool connected, bool wan,
                                           bool hasWaveforms)
{
    return connected && !wan && hasWaveforms;
}

} // namespace AetherSDR
