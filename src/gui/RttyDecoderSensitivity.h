#pragma once

// RTTY "Sens:" slider → confidence threshold (#5028), pure for testing.
// RttyDecoder's confidence is the character's weakest data bit, scaled so 0.5 is
// a coin flip and 1.0 a clean signal (#7422), so 0..100 maps onto 0.50..0.95 and
// 0 means "show everything" (the default, a no-op filter). Against the pane's
// color bands, 23 is the first value that hides every red letter, 45 every
// orange one, and 78 leaves only green.
namespace AetherSDR {

constexpr int kRttySensitivityDefault = 0;

constexpr float rttyConfThresholdFor(int sens)
{
    const int clamped = sens < 0 ? 0 : (sens > 100 ? 100 : sens);
    return 0.5f + (static_cast<float>(clamped) / 100.0f) * 0.45f;
}

} // namespace AetherSDR
