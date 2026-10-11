#pragma once

#include "core/backends/NoiseBlankerKind.h"
#include "core/dsp/WdspChannel.h"

// The one place the seam's blanker vocabulary meets the engine's (core/dsp is a
// leaf and keeps its own enums). The static_asserts fail the build if an
// existing value is renumbered; they do not catch a value added to only one
// enum. Only backends that run WDSP on the host include this.
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
