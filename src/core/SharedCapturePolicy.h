#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace AetherSDR::SharedCapturePolicy {

// Representation/resource ceilings, not hardware bandwidth or receiver limits.
// Finite double-Hz input; RF intervals are rounded conservatively outward.
inline constexpr double kMaxMagnitudeHz = 1099511627776.0; // 2^40
inline constexpr std::size_t kMaxEntries = 4096;
inline constexpr std::size_t kMaxDomains = 256;

enum class Error {
    None, InvalidNumber, InvalidInterval, InvalidIdentity, DuplicateId,
    TooManyEntries, InvalidCapacity, InvalidDomains, CaptureMismatch,
    GenerationMismatch, OutsideCapture, NoLegalCenter, CapacityExceeded
};

struct CaptureDescriptor {
    std::uint64_t captureId = 0; // Session endpoint identity, not persistent USB identity.
    std::uint64_t generation = 0; // Coherent center/rate/usable readback generation.
    double centerHz = 0;
    double achievedSampleRateHz = 0;
    double usableLeftHz = 0; // Nonnegative extents; each <= achieved rate / 2.
    double usableRightHz = 0;
    bool operator==(const CaptureDescriptor&) const = default;
};

struct SliceDescriptor {
    std::int32_t stableId = -1; // Stable UI slot, never a vector index or DSP channel.
    double carrierHz = 0;
    double filterLowHz = 0;
    double filterHighHz = 0;
    // RF = carrier + translation + filter offset. Adapter supplies the signed
    // carrier/BFO translation; this helper invents no mode presets or CW pitch.
    double translationHz = 0;
    double guardLowHz = 0;
    double guardHighHz = 0;
    bool operator==(const SliceDescriptor&) const = default;
};

struct CenterDomain {
    double minimumHz = 0;
    double maximumHz = 0; // Inclusive; a single realizable point is allowed.
    double gridOriginHz = 0;
    double stepHz = 0; // Realizable centers are fma(integer, step, origin).
    // Exact integer indices are bounded to +/-2^52; step must resolve across
    // this domain. These are numeric guards, not hardware tuning limits.
};

struct ReceiverLimits {
    // Must match the caller's published maxSlices/addressable range.
    std::size_t slotCount = 0; // Valid stable IDs are [0, slotCount).
    std::size_t capacity = 0; // Available receivers, may be less than slotCount.
};

struct Interval {
    double lowHz = 0;
    double highHz = 0;
};

struct IntervalResult {
    Error error = Error::None;
    std::optional<Interval> interval;
};

struct SelectionResult {
    Error error = Error::None;
    std::optional<CaptureDescriptor> capture; // Proposal only; needs actual readback.
};

struct RestoreRejection {
    std::size_t inputIndex = 0;
    std::int32_t stableId = -1;
    Error reason = Error::None;
};

struct RestoreResult {
    Error error = Error::None; // Global error => no accepted/rejected partial plan.
    std::vector<SliceDescriptor> accepted; // Ascending stable ID, unchanged values.
    std::vector<RestoreRejection> rejected; // Original input order.
};

[[nodiscard]] IntervalResult occupiedInterval(const SliceDescriptor& slice);
// The complete desired set is atomic: malformed or impossible => no proposal.
// Keep a legal current center; otherwise nearest grid point, lower Hz on ties.
[[nodiscard]] SelectionResult selectCenter(const CaptureDescriptor& capture,
    std::span<const SliceDescriptor> desired, std::span<const CenterDomain> domains,
    ReceiverLimits limits);
// Actual center/rate/margins may differ from the proposal if the complete set
// still fits. Identity and generation must match. Does not perform a transaction.
[[nodiscard]] Error validateReadback(const CaptureDescriptor& proposed,
    const CaptureDescriptor& actual, std::span<const SliceDescriptor> desired,
    std::span<const CenterDomain> domains, ReceiverLimits limits);
// Fixed actual capture: no retune, bandwidth change, truncation, or renumbering.
// All occurrences of a duplicated ID are rejected, regardless of validity/order.
// Capacity zero is valid. If accepted is empty the caller retains initial state.
[[nodiscard]] RestoreResult restoreFixedCapture(const CaptureDescriptor& actual,
    std::span<const SliceDescriptor> saved, std::span<const CenterDomain> domains,
    ReceiverLimits limits);

} // namespace AetherSDR::SharedCapturePolicy
