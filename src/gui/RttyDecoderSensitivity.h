#pragma once

// RTTY "Sens:" slider → confidence threshold (#5028), pure for testing.
// RttyDecoder's confidence is max(mark,space)/(mark+space): 0.5 at worst, 1.0
// clean, so 0..100 maps onto 0.50..0.95 and 0 means "show everything" (the
// default, a no-op filter). 38 gives ~0.67, the decoder's 3 dB lock point
// (snrDb == 10*log10(c/(1-c))).
namespace AetherSDR {

constexpr int kRttySensitivityDefault = 0;

constexpr float rttyConfThresholdFor(int sens)
{
    const int clamped = sens < 0 ? 0 : (sens > 100 ? 100 : sens);
    return 0.5f + (static_cast<float>(clamped) / 100.0f) * 0.45f;
}

} // namespace AetherSDR
