#pragma once

#include "core/backends/NoiseBlankerKind.h"
#include "core/dsp/WdspChannel.h"

// The one place the seam's blanker vocabulary meets the engine's.
//
// Two enums exist rather than one because core/dsp is a leaf — WdspChannel
// depends on Qt and the standard library and nothing else in this tree, and it
// should not have to include the seam's headers to blank an impulse. The cost of
// that is a conversion, and the cost of a conversion is that it can rot. So it
// lives here, once, with static_asserts that fail the BUILD if either enum is
// renumbered — rather than as a switch in each backend, where a third algorithm
// added to one enum and not the other would compile and mis-dispatch.
//
// Only a backend that runs WDSP on the host includes this. A backend whose
// blanker is the radio's own firmware never sees the engine's enum at all.
namespace AetherSDR {

static_assert(static_cast<int>(NoiseBlankerKind::Off)
                  == static_cast<int>(WdspChannel::NoiseBlanker::Off),
              "seam and engine blanker kinds must share their values");
static_assert(static_cast<int>(NoiseBlankerKind::Impulse)
                  == static_cast<int>(WdspChannel::NoiseBlanker::Impulse),
              "seam and engine blanker kinds must share their values");
static_assert(static_cast<int>(NoiseBlankerKind::Advanced)
                  == static_cast<int>(WdspChannel::NoiseBlanker::Advanced),
              "seam and engine blanker kinds must share their values");

static_assert(static_cast<int>(NoiseBlankerFill::Zero)
                  == static_cast<int>(WdspChannel::NoiseBlankerFill::Zero),
              "seam and engine blanker fills must share their values");
static_assert(static_cast<int>(NoiseBlankerFill::SampleHold)
                  == static_cast<int>(WdspChannel::NoiseBlankerFill::SampleHold),
              "seam and engine blanker fills must share their values");
static_assert(static_cast<int>(NoiseBlankerFill::MeanHold)
                  == static_cast<int>(WdspChannel::NoiseBlankerFill::MeanHold),
              "seam and engine blanker fills must share their values");
static_assert(static_cast<int>(NoiseBlankerFill::HoldSample)
                  == static_cast<int>(WdspChannel::NoiseBlankerFill::HoldSample),
              "seam and engine blanker fills must share their values");
static_assert(static_cast<int>(NoiseBlankerFill::Interpolate)
                  == static_cast<int>(WdspChannel::NoiseBlankerFill::Interpolate),
              "seam and engine blanker fills must share their values");

[[nodiscard]] constexpr WdspChannel::NoiseBlanker toWdsp(NoiseBlankerKind kind) noexcept
{
    return static_cast<WdspChannel::NoiseBlanker>(kind);
}

[[nodiscard]] constexpr WdspChannel::NoiseBlankerFill toWdsp(NoiseBlankerFill fill) noexcept
{
    return static_cast<WdspChannel::NoiseBlankerFill>(fill);
}

[[nodiscard]] constexpr NoiseBlankerKind fromWdsp(WdspChannel::NoiseBlanker kind) noexcept
{
    return static_cast<NoiseBlankerKind>(kind);
}

[[nodiscard]] constexpr NoiseBlankerFill fromWdsp(WdspChannel::NoiseBlankerFill fill) noexcept
{
    return static_cast<NoiseBlankerFill>(fill);
}

} // namespace AetherSDR
