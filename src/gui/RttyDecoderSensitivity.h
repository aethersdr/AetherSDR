#pragma once

// RTTY decoder sensitivity: the slider→threshold mapping behind the RTTY
// pane's "Sens:" control (#5028).  Pure and header-only so the mapping is
// pinned by a test without a widget.
//
// RttyDecoder reports per-character confidence as max(mark,space)/(mark+space),
// which can never fall below 0.5 (it is the larger of two envelopes over their
// sum) and is 1.0 for a clean tone.  The CW pane's cost runs the other way
// ([0,1], lower = better), so its mapping cannot be copied verbatim: a slider
// at 0 must still mean "show everything".  0..100 maps onto 0.50..0.95.
//
// The default is 0: threshold 0.50 is the confidence floor, so `confidence <
// threshold` can never fire and out-of-the-box behavior is provably identical
// to the pane before this control existed — filtering is strictly opt-in.
// A useful starting value when noise floods the pane is 38, which lands the
// threshold at ~0.67 — the decoder's own 3 dB "locked" point
// (snrDb == 10*log10(c/(1-c))), i.e. filter what the stats bar calls UNLOCK.
namespace AetherSDR {

constexpr int kRttySensitivityDefault = 0;

constexpr float rttyConfThresholdFor(int sens)
{
    const int clamped = sens < 0 ? 0 : (sens > 100 ? 100 : sens);
    return 0.5f + (static_cast<float>(clamped) / 100.0f) * 0.45f;
}

} // namespace AetherSDR
