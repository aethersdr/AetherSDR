#include "SharedCapturePolicy.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>

namespace AetherSDR::SharedCapturePolicy {
namespace {
constexpr double kMaxIndex = 4503599627370496.0; // 2^52, exact integer double.
constexpr double kInfinity = std::numeric_limits<double>::infinity();

bool validNumber(double value)
{
    return std::isfinite(value) && std::abs(value) <= kMaxMagnitudeHz;
}

// TwoSum's residual tells which way IEEE round-to-nearest moved the sum.
// Bound every input before arithmetic; no overflow, epsilon or wider long-double
// assumption (MSVC long double is double). Never compile this with fast-math.
double directedSum(double left, double right, bool upward)
{
    const double sum = left + right;
    const double rightPart = sum - left;
    const double residual = (left - (sum - rightPart)) + (right - rightPart);
    if ((upward && residual > 0) || (!upward && residual < 0)) {
        return std::nextafter(sum, upward ? kInfinity : -kInfinity);
    }
    return sum;
}

// Keep the residual when comparing distances, too: at a very distant current
// center, two distinct nearby candidates can have the same rounded distance.
struct Distance {
    double value;
    double residual;
};

Distance distanceFrom(double center, double current)
{
    const double value = center - current;
    const double rightPart = value - center;
    const double residual = (center - (value - rightPart)) + (-current - rightPart);
    return value < 0 ? Distance{-value, -residual} : Distance{value, residual};
}

bool closer(const Distance& left, const Distance& right)
{
    return left.value < right.value
        || (left.value == right.value && left.residual < right.residual);
}

Error captureInterval(const CaptureDescriptor& capture, Interval& interval)
{
    if (capture.captureId == 0 || capture.generation == 0) {
        return Error::InvalidIdentity;
    }
    if (!validNumber(capture.centerHz) || !validNumber(capture.achievedSampleRateHz)
        || !validNumber(capture.usableLeftHz) || !validNumber(capture.usableRightHz)) {
        return Error::InvalidNumber;
    }
    if (capture.centerHz < 0 || capture.achievedSampleRateHz <= 0
        || capture.usableLeftHz < 0 || capture.usableRightHz < 0
        || 2 * capture.usableLeftHz > capture.achievedSampleRateHz
        || 2 * capture.usableRightHz > capture.achievedSampleRateHz) {
        return Error::InvalidInterval;
    }
    // Inward capture edges cannot promise an unrepresentable sliver of RF.
    interval = {directedSum(capture.centerHz, -capture.usableLeftHz, true),
                directedSum(capture.centerHz, capture.usableRightHz, false)};
    if (interval.lowHz >= interval.highHz) {
        return Error::InvalidInterval;
    }
    return Error::None;
}

Error sliceInterval(const SliceDescriptor& slice, Interval& interval)
{
    if (slice.stableId < 0) {
        return Error::InvalidIdentity;
    }
    for (const double value : {slice.carrierHz, slice.filterLowHz, slice.filterHighHz,
                              slice.translationHz, slice.guardLowHz, slice.guardHighHz}) {
        if (!validNumber(value)) {
            return Error::InvalidNumber;
        }
    }
    if (slice.carrierHz < 0 || slice.filterLowHz >= slice.filterHighHz
        || slice.guardLowHz < 0 || slice.guardHighHz < 0) {
        return Error::InvalidInterval;
    }
    const double lowCarrier = directedSum(slice.carrierHz, slice.translationHz, false);
    const double highCarrier = directedSum(slice.carrierHz, slice.translationHz, true);
    interval = {
        directedSum(directedSum(lowCarrier, slice.filterLowHz, false), -slice.guardLowHz, false),
        directedSum(directedSum(highCarrier, slice.filterHighHz, true), slice.guardHighHz, true)};
    if (interval.lowHz < 0 || interval.highHz > kMaxMagnitudeHz
        || interval.lowHz >= interval.highHz) {
        return Error::InvalidInterval;
    }
    return Error::None;
}

// Enumerate bounded neighboring integer grid indices around a target. Division
// may round at an endpoint; neighbors are checked using the returned fma value.
// Valid domains keep indices <= 2^52 and step >= their largest ULP, so two
// neighbors cover quotient rounding and a center rounded onto an endpoint.
template <typename Visitor>
void gridNeighbors(const CenterDomain& domain, double target, Visitor visit)
{
    const double index = std::floor((target - domain.gridOriginHz) / domain.stepHz);
    for (int offset = -2; offset <= 2; ++offset) {
        const double candidateIndex = index + offset;
        if (std::abs(candidateIndex) <= kMaxIndex) {
            const double center = std::fma(candidateIndex, domain.stepHz, domain.gridOriginHz);
            if (center >= domain.minimumHz && center <= domain.maximumHz) {
                visit(center);
            }
        }
    }
}

Error validateDomains(std::span<const CenterDomain> domains)
{
    if (domains.empty() || domains.size() > kMaxDomains) {
        return Error::InvalidDomains;
    }
    for (const CenterDomain& domain : domains) {
        for (const double value : {domain.minimumHz, domain.maximumHz, domain.gridOriginHz, domain.stepHz}) {
            if (!validNumber(value)) {
                return Error::InvalidNumber;
            }
        }
        if (domain.minimumHz < 0 || domain.maximumHz < domain.minimumHz
            || domain.gridOriginHz < 0 || domain.stepHz <= 0) {
            return Error::InvalidDomains;
        }
        const double magnitude = std::max(domain.maximumHz, domain.gridOriginHz);
        const double resolution = std::nextafter(magnitude, kInfinity) - magnitude;
        const double first = (domain.minimumHz - domain.gridOriginHz) / domain.stepHz;
        const double last = (domain.maximumHz - domain.gridOriginHz) / domain.stepHz;
        if (domain.stepHz < resolution || !std::isfinite(first) || !std::isfinite(last)
            || std::abs(first) > kMaxIndex || std::abs(last) > kMaxIndex) {
            return Error::InvalidDomains;
        }
        bool any = false;
        gridNeighbors(domain, domain.minimumHz, [&any](double) { any = true; });
        if (!any) {
            return Error::InvalidDomains;
        }
    }
    return Error::None;
}

bool legalCenter(double center, std::span<const CenterDomain> domains)
{
    for (const CenterDomain& domain : domains) {
        if (center < domain.minimumHz || center > domain.maximumHz) {
            continue;
        }
        bool matches = false;
        gridNeighbors(domain, center, [center, &matches](double candidate) {
            matches = matches || center == candidate;
        });
        if (matches) {
            return true;
        }
    }
    return false;
}

bool contains(const Interval& capture, const Interval& slice)
{
    return slice.lowHz >= capture.lowHz && slice.highHz <= capture.highHz;
}

Error validateLimits(ReceiverLimits limits)
{
    if (limits.slotCount == 0 || limits.slotCount > kMaxEntries || limits.capacity > limits.slotCount) {
        return Error::InvalidCapacity;
    }
    return Error::None;
}

Error desiredIntervals(std::span<const SliceDescriptor> desired, ReceiverLimits limits,
                       std::vector<Interval>& result)
{
    if (desired.size() > kMaxEntries) {
        return Error::TooManyEntries;
    }
    if (desired.size() > limits.capacity) {
        return Error::CapacityExceeded;
    }
    std::set<std::int32_t> ids;
    for (const SliceDescriptor& slice : desired) {
        Interval interval;
        const Error error = sliceInterval(slice, interval);
        if (error != Error::None) {
            return error;
        }
        if (static_cast<std::size_t>(slice.stableId) >= limits.slotCount) {
            return Error::InvalidIdentity;
        }
        if (!ids.insert(slice.stableId).second) {
            return Error::DuplicateId;
        }
        result.push_back(interval);
    }
    return Error::None;
}

Error fixedCapture(const CaptureDescriptor& descriptor, std::span<const CenterDomain> domains,
                   Interval& interval)
{
    Error error = captureInterval(descriptor, interval);
    if (error != Error::None) {
        return error;
    }
    error = validateDomains(domains);
    if (error != Error::None) {
        return error;
    }
    if (!legalCenter(descriptor.centerHz, domains)) {
        return Error::NoLegalCenter;
    }
    return Error::None;
}
} // namespace

IntervalResult occupiedInterval(const SliceDescriptor& slice)
{
    Interval interval;
    const Error error = sliceInterval(slice, interval);
    if (error != Error::None) {
        return {error, std::nullopt};
    }
    return {Error::None, interval};
}

SelectionResult selectCenter(const CaptureDescriptor& capture,
    std::span<const SliceDescriptor> desired, std::span<const CenterDomain> domains,
    ReceiverLimits limits)
{
    if (validateLimits(limits) != Error::None) {
        return {Error::InvalidCapacity, std::nullopt};
    }
    Interval currentInterval;
    Error error = captureInterval(capture, currentInterval);
    if (error != Error::None) {
        return {error, std::nullopt};
    }
    error = validateDomains(domains);
    if (error != Error::None) {
        return {error, std::nullopt};
    }
    std::vector<Interval> intervals;
    error = desiredIntervals(desired, limits, intervals);
    if (error != Error::None) {
        return {error, std::nullopt};
    }
    double low = 0;
    double high = kMaxMagnitudeHz;
    for (const Interval& interval : intervals) {
        low = std::max(low, directedSum(interval.highHz, -capture.usableRightHz, true));
        high = std::min(high, directedSum(interval.lowHz, capture.usableLeftHz, false));
    }
    std::optional<double> best;
    Distance bestDistance{kInfinity, 0};
    for (const CenterDomain& domain : domains) {
        const double first = std::max(low, domain.minimumHz);
        const double last = std::min(high, domain.maximumHz);
        if (first > last) {
            continue;
        }
        const auto consider = [&](double center) {
            if (center < first || center > last) {
                return;
            }
            CaptureDescriptor candidate = capture;
            candidate.centerHz = center;
            Interval candidateInterval;
            if (captureInterval(candidate, candidateInterval) != Error::None) {
                return;
            }
            for (const Interval& interval : intervals) {
                if (!contains(candidateInterval, interval)) {
                    return;
                }
            }
            const Distance distance = distanceFrom(center, capture.centerHz);
            const bool sameDistance = distance.value == bestDistance.value
                && distance.residual == bestDistance.residual;
            if (!best || closer(distance, bestDistance) || (sameDistance && center < *best)) {
                best = center;
                bestDistance = distance;
            }
        };
        gridNeighbors(domain, std::clamp(capture.centerHz, first, last), consider);
        gridNeighbors(domain, first, consider);
        gridNeighbors(domain, last, consider);
    }
    if (!best) {
        return {Error::NoLegalCenter, std::nullopt};
    }
    CaptureDescriptor proposed = capture;
    proposed.centerHz = *best;
    // Acceptance is only a proposal. Re-validate as defense in depth after
    // candidate range and containment checks.
    error = validateReadback(proposed, proposed, desired, domains, limits);
    if (error != Error::None) {
        return {error, std::nullopt};
    }
    return {Error::None, proposed};
}

Error validateReadback(const CaptureDescriptor& proposed, const CaptureDescriptor& actual,
    std::span<const SliceDescriptor> desired, std::span<const CenterDomain> domains,
    ReceiverLimits limits)
{
    if (validateLimits(limits) != Error::None) {
        return Error::InvalidCapacity;
    }
    Interval proposalInterval, actualInterval;
    Error error = fixedCapture(proposed, domains, proposalInterval);
    if (error != Error::None) {
        return error;
    }
    error = fixedCapture(actual, domains, actualInterval);
    if (error != Error::None) {
        return error;
    }
    if (actual.captureId != proposed.captureId) {
        return Error::CaptureMismatch;
    }
    if (actual.generation != proposed.generation) {
        return Error::GenerationMismatch;
    }
    std::vector<Interval> intervals;
    error = desiredIntervals(desired, limits, intervals);
    if (error != Error::None) {
        return error;
    }
    for (const Interval& interval : intervals) {
        if (!contains(proposalInterval, interval) || !contains(actualInterval, interval)) {
            return Error::OutsideCapture;
        }
    }
    return Error::None;
}

RestoreResult restoreFixedCapture(const CaptureDescriptor& actual,
    std::span<const SliceDescriptor> saved, std::span<const CenterDomain> domains,
    ReceiverLimits limits)
{
    if (validateLimits(limits) != Error::None) {
        return {Error::InvalidCapacity, {}, {}};
    }
    if (saved.size() > kMaxEntries) {
        return {Error::TooManyEntries, {}, {}};
    }
    Interval capture;
    const Error error = fixedCapture(actual, domains, capture);
    if (error != Error::None) {
        return {error, {}, {}};
    }
    std::map<std::int32_t, std::size_t> counts;
    for (const SliceDescriptor& slice : saved) {
        ++counts[slice.stableId];
    }
    std::vector<Error> reasons(saved.size(), Error::None);
    std::vector<std::size_t> fitting;
    for (std::size_t i = 0; i < saved.size(); ++i) {
        const SliceDescriptor& slice = saved[i];
        Interval interval;
        reasons[i] = sliceInterval(slice, interval);
        if (slice.stableId >= 0 && static_cast<std::size_t>(slice.stableId) >= limits.slotCount) {
            reasons[i] = Error::InvalidIdentity;
        }
        if (slice.stableId >= 0 && counts[slice.stableId] > 1) {
            reasons[i] = Error::DuplicateId;
        }
        if (reasons[i] == Error::None && !contains(capture, interval)) {
            reasons[i] = Error::OutsideCapture;
        }
        if (reasons[i] == Error::None) {
            fitting.push_back(i);
        }
    }
    std::sort(fitting.begin(), fitting.end(), [saved](std::size_t left, std::size_t right) {
        return saved[left].stableId < saved[right].stableId;
    });
    RestoreResult result;
    for (const std::size_t index : fitting) {
        if (result.accepted.size() < limits.capacity) {
            result.accepted.push_back(saved[index]);
        } else {
            reasons[index] = Error::CapacityExceeded;
        }
    }
    for (std::size_t i = 0; i < saved.size(); ++i) {
        if (reasons[i] != Error::None) {
            result.rejected.push_back({i, saved[i].stableId, reasons[i]});
        }
    }
    return result;
}
} // namespace AetherSDR::SharedCapturePolicy
