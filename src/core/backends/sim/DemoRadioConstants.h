#pragma once

#include <QString>

namespace AetherSDR::DemoRadio {

// Backend-only compatibility data for the in-process Demo radio and its
// synthetic Flex connection. Neither consumer needs the other's implementation:
// RadioConnection reads these values, not SimBackend.
inline QString serial()
{
    return QStringLiteral("DEMO-0001");
}

// Flex's line_duration field is a 1..100 rate, NOT a millisecond interval.
// Demo produces its own cadence; 100 leaves those rows ungated (#4606).
inline constexpr int kWaterfallRate = 100;

// The demo pan span, and the ONE definition behind all three of its
// publications: the synthetic connect's "display pan ... bandwidth=" wire
// field, SimBackend::kDemoPanBandwidthMhz, and the spectrum span
// SimSignalSource renders the audio scene across. All three MUST agree — the
// demo's spectrum row IS the +/-4 kHz audio scene, and AE stretches that row
// across the pan width, so a pan wider than the data puts the birdie at the
// wrong frequency and outside the RX passband. Matching them makes the
// on-screen birdie position and the demodulated audio pitch agree.
// (RFC #4288 — birdie fix.)
inline constexpr double kPanBandwidthMhz = 0.008;

// The same span in Hz, for the spectrum renderer. Exact in IEEE-754 double:
// 0.008 * 1e6 == 8000.0, so this is the identical quantity, not a rounding of
// it.
inline constexpr double kAudioSpanHz = kPanBandwidthMhz * 1e6;

} // namespace AetherSDR::DemoRadio
