#include "core/SharedCapturePolicy.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

using namespace AetherSDR::SharedCapturePolicy;

namespace {
int g_failures = 0;
int g_checks = 0;
void check(bool condition, const char* message)
{
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
}

CaptureDescriptor capture(double center = 1000, double left = 100, double right = 100)
{
    return {7, 3, center, 1000, left, right};
}

SliceDescriptor slice(std::int32_t id, double carrier, double low = -10, double high = 10)
{
    return {id, carrier, low, high, 0, 0, 0};
}

constexpr ReceiverLimits kLimits{kMaxEntries, kMaxEntries};

const std::array<CenterDomain, 1> kDomains{{{100, 2000, 0, 1}}};

SelectionResult selectCenter(const CaptureDescriptor& current, std::span<const SliceDescriptor> desired,
                             std::span<const CenterDomain> domains)
{
    return AetherSDR::SharedCapturePolicy::selectCenter(current, desired, domains, kLimits);
}

Error validateReadback(const CaptureDescriptor& proposed, const CaptureDescriptor& actual,
                      std::span<const SliceDescriptor> desired, std::span<const CenterDomain> domains)
{
    return AetherSDR::SharedCapturePolicy::validateReadback(proposed, actual, desired, domains, kLimits);
}

RestoreResult restoreFixedCapture(const CaptureDescriptor& actual, std::span<const SliceDescriptor> saved,
                                 std::span<const CenterDomain> domains, std::size_t capacity)
{
    return AetherSDR::SharedCapturePolicy::restoreFixedCapture(actual, saved, domains, {kMaxEntries, capacity});
}

void expectCenter(const SelectionResult& result, double expected, const char* message)
{
    check(result.error == Error::None && result.capture && result.capture->centerHz == expected, message);
}

// Independent containment against explicit RF edges: no policy formula duplicated.
void expectContains(const SelectionResult& result, double low, double high)
{
    check(result.capture && result.capture->centerHz - result.capture->usableLeftHz <= low
          && result.capture->centerHz + result.capture->usableRightHz >= high,
          "selected capture contains independently specified RF interval");
}

void geometryAndWholeSet()
{
    SliceDescriptor translated = slice(0, 1000, -30, 10);
    translated.translationHz = 20;
    translated.guardLowHz = 5;
    translated.guardHighHz = 7;
    const IntervalResult occupied = occupiedInterval(translated);
    check(occupied.error == Error::None && occupied.interval
          && occupied.interval->lowHz == 985 && occupied.interval->highHz == 1037,
          "asymmetric filter plus signed BFO translation and two guards produce [985,1037]");
    translated.translationHz = -20;
    const IntervalResult reversed = occupiedInterval(translated);
    check(reversed.interval && reversed.interval->lowHz == 945 && reversed.interval->highHz == 997,
          "negative carrier/BFO translation has explicit RF sign");

    const CaptureDescriptor current = capture(1000, 80, 120); // [920,1120]
    const std::array<SliceDescriptor, 2> edgeSlices{{slice(9, 930), slice(2, 1110)}};
    const SelectionResult fixed = selectCenter(current, edgeSlices, kDomains);
    expectCenter(fixed, 1000, "two opposite-edge receivers fit full asymmetric capture exactly");
    expectContains(fixed, 920, 1120);
    const auto savedInputs = edgeSlices;
    const CaptureDescriptor savedCapture = current;

    std::array<SliceDescriptor, 2> moved = edgeSlices;
    moved[0].carrierHz = 970;
    moved[1].carrierHz = 1130;
    const SelectionResult recentered = selectCenter(current, moved, kDomains);
    expectCenter(recentered, 1020, "recenter chooses nearest fit for complete desired set");
    expectContains(recentered, 960, 1140);
    check(moved[0].carrierHz == 970 && moved[1].carrierHz == 1130,
          "successful selection preserves absolute sibling frequencies");

    moved = edgeSlices;
    moved[1].carrierHz += 1;
    const SelectionResult impossible = selectCenter(current, moved, kDomains);
    check(impossible.error == Error::NoLegalCenter && !impossible.capture,
          "one-Hz whole-set excess refuses atomically");
    check(current == savedCapture && edgeSlices == savedInputs && moved[1].carrierHz == 1111,
          "refusal leaves inputs and prior valid state unchanged");

    moved = edgeSlices;
    moved[1].guardHighHz = 1;
    check(selectCenter(current, moved, kDomains).error == Error::NoLegalCenter,
          "transition guard widening participates in whole-set fit");
    moved = edgeSlices;
    moved[1].filterHighHz = 11;
    check(selectCenter(current, moved, kDomains).error == Error::NoLegalCenter,
          "mode/filter widening participates in whole-set fit");
    CaptureDescriptor shrink = current;
    shrink.achievedSampleRateHz = 200;
    shrink.usableLeftHz = 80;
    shrink.usableRightHz = 100;
    check(selectCenter(shrink, edgeSlices, kDomains).error == Error::NoLegalCenter,
          "proposed achieved-rate shrink validates all existing receivers");
    expectCenter(selectCenter(current, std::span(edgeSlices).first(1), kDomains), 1000,
                 "removing a receiver preserves a legal center");
    expectCenter(selectCenter(current, {}, kDomains), 1000,
                 "removing all receivers preserves a legal center");
}

void gridsAndDomains()
{
    const std::array<SliceDescriptor, 1> desired{{slice(0, 1065, -40, 40)}}; // [1025,1105]
    const std::array<CenterDomain, 1> grid{{{100, 2000, 0, 10}}};
    expectCenter(selectCenter(capture(1000, 50, 50), desired, grid), 1060,
                 "rounding 1055 to outside point 1050 still finds legal 1060");
    expectContains(selectCenter(capture(1000, 50, 50), desired, grid), 1025, 1105);
    const std::array<SliceDescriptor, 1> noGrid{{slice(0, 1060, -49, 49)}}; // centers [1059,1061]
    const std::array<CenterDomain, 1> offsetGrid{{{100, 2000, 5, 10}}};
    check(selectCenter(capture(1000, 50, 50), noGrid, offsetGrid).error == Error::NoLegalCenter,
          "continuous feasible interval with no realizable center refuses");

    const std::array<CenterDomain, 2> bands{{{900, 980, 0, 10}, {1020, 1100, 0, 10}}};
    const std::array<SliceDescriptor, 1> narrow{{slice(0, 1000)}};
    expectCenter(selectCenter(capture(), narrow, bands), 980,
                 "disjoint hardware bands do not bridge gap; lower-Hz tie wins");
    auto reversedBands = bands;
    std::reverse(reversedBands.begin(), reversedBands.end());
    expectCenter(selectCenter(capture(), narrow, reversedBands), 980,
                 "domain input order does not change tie outcome");
    expectCenter(selectCenter(capture(1020), narrow, bands), 1020,
                 "existing legal grid center beats recentering");
    const std::array<CenterDomain, 1> negativeIndex{{{900, 1100, 2000, 10}}};
    expectCenter(selectCenter(capture(1005), narrow, negativeIndex), 1000,
                 "negative grid indices and halfway tie are mathematical, not truncating");
    const std::array<CenterDomain, 1> point{{{1000, 1000, 0, 1}}};
    expectCenter(selectCenter(capture(), narrow, point), 1000,
                 "single realizable hardware center is a valid domain");

    CaptureDescriptor far = capture(kMaxMagnitudeHz / 2, 0.125, 0.125);
    const double next = std::nextafter(1.0, 2.0);
    const std::array<CenterDomain, 1> tiny{{{1, next, 1, next - 1}}};
    expectCenter(selectCenter(far, {}, tiny), next,
                 "nearest comparison retains residual when large distances round equal");

    // Exhaustive small integer domain oracle enumerates hardware points directly,
    // never computes the policy's intersection formula or quantization quotient.
    const std::array<CenterDomain, 2> oracleBands{{{80, 98, 0, 3}, {103, 130, 1, 4}}};
    for (int current = 90; current <= 112; current += 2) {
        for (int station = 85; station <= 125; station += 5) {
            const CaptureDescriptor proposed = capture(current, 11, 15);
            const std::array<SliceDescriptor, 2> receivers{{slice(7, 100, -2, 3), slice(3, station, -3, 4)}};
            std::optional<int> expected;
            for (int center = 80; center <= 130; ++center) {
                const bool legal = (center <= 98 && center % 3 == 0)
                    || (center >= 103 && (center - 1) % 4 == 0);
                if (!legal || center - 11 > 98 || center + 15 < 103
                    || center - 11 > station - 3 || center + 15 < station + 4) {
                    continue;
                }
                if (!expected || std::abs(center - current) < std::abs(*expected - current)) {
                    expected = center;
                }
            }
            const SelectionResult actual = selectCenter(proposed, receivers, oracleBands);
            check(expected ? actual.capture && actual.capture->centerHz == *expected
                           : actual.error == Error::NoLegalCenter && !actual.capture,
                  "enumerated hardware-center oracle agrees across desired sets");
        }
    }
}

void slotBoundsAndUnusedCapture()
{
    const std::array<CenterDomain, 1> lowBand{{{100, 100, 0, 1}}};
    const std::array<SliceDescriptor, 1> desired{{slice(0, 100)}};
    const CaptureDescriptor low = capture(100, 400, 400);
    expectCenter(selectCenter(low, desired, lowBand), 100,
                 "unused negative capture interval does not veto legal positive RF slice");
    check(validateReadback(low, low, desired, lowBand) == Error::None,
          "actual capture may extend below zero when requested RF is valid");
    const std::array<CenterDomain, 1> fineGrid{{{1, 2, 0, 1e-5}}};
    expectCenter(selectCenter(capture(1e12, 1, 1), {}, fineGrid), 2,
                 "rounded distance ties cannot select 1.99998 over nearer 2");

    const std::array<SliceDescriptor, 1> positive{{slice(0, 1000, 0, 10)}};
    expectCenter(selectCenter(capture(1000, 0, 100), positive, kDomains), 1000,
                 "positive-only measured usable interval is valid");
    const std::array<SliceDescriptor, 1> negative{{slice(0, 1000, -10, 0)}};
    expectCenter(selectCenter(capture(1000, 100, 0), negative, kDomains), 1000,
                 "negative-only measured usable interval is valid");

    const ReceiverLimits limited{4, 2};
    const std::array<SliceDescriptor, 3> receivers{{slice(1, 1000), slice(3, 1000), slice(2, 1000)}};
    check(AetherSDR::SharedCapturePolicy::selectCenter(capture(), receivers, kDomains, limited).error
          == Error::CapacityExceeded, "whole desired set respects caller concurrent capacity");
    const std::array<SliceDescriptor, 1> invalidSlot{{slice(std::numeric_limits<std::int32_t>::max(), 1000)}};
    check(AetherSDR::SharedCapturePolicy::selectCenter(capture(), invalidSlot, kDomains, limited).error
          == Error::InvalidIdentity, "positive but out-of-range desired stable ID rejected");
    const RestoreResult invalid = AetherSDR::SharedCapturePolicy::restoreFixedCapture(
        capture(), invalidSlot, kDomains, limited);
    check(invalid.error == Error::None && invalid.accepted.empty() && invalid.rejected.size() == 1
          && invalid.rejected[0].reason == Error::InvalidIdentity,
          "positive but out-of-range saved stable ID rejected without renumbering");
    const RestoreResult sparse = AetherSDR::SharedCapturePolicy::restoreFixedCapture(
        capture(), receivers, kDomains, limited);
    check(sparse.accepted.size() == 2 && sparse.accepted[0].stableId == 1 && sparse.accepted[1].stableId == 2,
          "capacity reduction preserves stable slots within explicit slot bounds");
    for (const ReceiverLimits invalidLimits : {ReceiverLimits{0, 0}, {4, 5}, {kMaxEntries + 1, 1}}) {
        check(AetherSDR::SharedCapturePolicy::selectCenter(capture(), {}, kDomains, invalidLimits).error
              == Error::InvalidCapacity, "invalid public receiver limits rejected");
        check(AetherSDR::SharedCapturePolicy::restoreFixedCapture(capture(), {}, kDomains, invalidLimits).error
              == Error::InvalidCapacity, "invalid restore slot limits rejected");
    }
}

void readbackAndPrecision()
{
    const std::array<SliceDescriptor, 2> edges{{slice(0, 920), slice(1, 1080)}}; // [910,1090]
    const CaptureDescriptor proposed = capture();
    check(validateReadback(proposed, proposed, edges, kDomains) == Error::None,
          "coherent achieved readback validates");
    CaptureDescriptor actual = proposed;
    actual.centerHz = 1005;
    actual.achievedSampleRateHz = 220;
    actual.usableLeftHz = 95;
    actual.usableRightHz = 85;
    check(validateReadback(proposed, actual, edges, kDomains) == Error::None,
          "different actual center/rate/asymmetric margins accepted if complete set fits");
    actual.centerHz = 1006;
    check(validateReadback(proposed, actual, edges, kDomains) == Error::OutsideCapture,
          "actual center mismatch that loses left edge fails");
    actual = proposed;
    actual.achievedSampleRateHz = 160;
    actual.usableLeftHz = 80;
    actual.usableRightHz = 80;
    check(validateReadback(proposed, actual, edges, kDomains) == Error::OutsideCapture,
          "actual achieved-rate shrink cannot publish false acceptance");
    actual = proposed;
    ++actual.generation;
    check(validateReadback(proposed, actual, edges, kDomains) == Error::GenerationMismatch,
          "mixed readback generation rejected");
    actual = proposed;
    ++actual.captureId;
    check(validateReadback(proposed, actual, edges, kDomains) == Error::CaptureMismatch,
          "different capture endpoint readback rejected");
    actual = proposed;
    actual.centerHz = 1000.5;
    check(validateReadback(proposed, actual, edges, kDomains) == Error::NoLegalCenter,
          "off-grid actual readback rejected");

    const std::array<CenterDomain, 1> point{{{1000, 1000, 0, 1}}};
    std::array<SliceDescriptor, 1> exact{{slice(0, 1000, -100, 100)}};
    expectCenter(selectCenter(proposed, exact, point), 1000, "exact edge is accepted without epsilon");
    exact[0].filterHighHz = std::nextafter(100.0, 101.0);
    check(selectCenter(proposed, exact, point).error == Error::NoLegalCenter,
          "nextafter over edge cannot disappear in carrier addition");
    exact[0] = slice(0, 1000, std::nextafter(-100.0, -101.0), 100);
    check(selectCenter(proposed, exact, point).error == Error::NoLegalCenter,
          "nextafter below left edge cannot disappear in carrier addition");
    exact[0] = slice(0, 1000, -100, std::nextafter(100.0, 99.0));
    expectCenter(selectCenter(proposed, exact, point), 1000, "nextafter inward is conservatively accepted");

    CaptureDescriptor decimal = capture(1000, 0.45 * 333.3, 0.43 * 333.3);
    decimal.achievedSampleRateHz = 333.3;
    const std::array<SliceDescriptor, 1> fractional{{slice(0, 1000.1, -10.2, 10.3)}};
    expectCenter(selectCenter(decimal, fractional, kDomains), 1000,
                 "ordinary decimal achieved rate and measured margins need no invented lattice");
    const std::array<CenterDomain, 1> decimalGrid{{{999, 1001, 1000, 0.1}}};
    CaptureDescriptor decimalCurrent = capture(std::fma(3.0, 0.1, 1000.0));
    expectCenter(selectCenter(decimalCurrent, fractional, decimalGrid), decimalCurrent.centerHz,
                 "correctly rounded decimal tuning grid preserves current center");
    CaptureDescriptor unresolvable = capture(kMaxMagnitudeHz, 1e-10, 1e-10);
    check(selectCenter(unresolvable, {}, kDomains).error == Error::InvalidInterval,
          "usable span erased by floating precision fails closed");
}

void malformedInputs()
{
    const CaptureDescriptor good = capture();
    const std::array<SliceDescriptor, 1> one{{slice(0, 1000)}};
    const std::array<double, 4> badNumbers{{std::numeric_limits<double>::quiet_NaN(),
        std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::max()}};
    for (const double value : badNumbers) {
        for (double SliceDescriptor::*field : {&SliceDescriptor::carrierHz, &SliceDescriptor::filterLowHz,
             &SliceDescriptor::filterHighHz, &SliceDescriptor::translationHz,
             &SliceDescriptor::guardLowHz, &SliceDescriptor::guardHighHz}) {
            auto malformed = one;
            malformed[0].*field = value;
            const SelectionResult result = selectCenter(good, malformed, kDomains);
            check(result.error == Error::InvalidNumber && !result.capture,
                  "every public slice numeric field rejects nonfinite/overflow magnitude");
        }
        for (double CaptureDescriptor::*field : {&CaptureDescriptor::centerHz,
             &CaptureDescriptor::achievedSampleRateHz, &CaptureDescriptor::usableLeftHz,
             &CaptureDescriptor::usableRightHz}) {
            CaptureDescriptor malformed = good;
            malformed.*field = value;
            check(selectCenter(malformed, one, kDomains).error == Error::InvalidNumber,
                  "every public capture numeric field rejects nonfinite/overflow magnitude");
        }
        for (double CenterDomain::*field : {&CenterDomain::minimumHz, &CenterDomain::maximumHz,
             &CenterDomain::gridOriginHz, &CenterDomain::stepHz}) {
            auto malformed = kDomains;
            malformed[0].*field = value;
            check(selectCenter(good, one, malformed).error == Error::InvalidNumber,
                  "every public hardware-domain numeric field rejects nonfinite/overflow magnitude");
        }
    }
    for (const SliceDescriptor& invalid : {slice(-1, 1000), slice(0, -1), slice(0, 1000, 10, 10),
                                         slice(0, 1000, 11, 10)}) {
        check(occupiedInterval(invalid).error != Error::None, "invalid slot/carrier/empty/inverted passband rejected");
    }
    SliceDescriptor invalid = one[0];
    invalid.guardLowHz = -1;
    check(occupiedInterval(invalid).error == Error::InvalidInterval, "negative left guard rejected");
    invalid = one[0];
    invalid.guardHighHz = -1;
    check(occupiedInterval(invalid).error == Error::InvalidInterval, "negative right guard rejected");
    invalid = slice(0, kMaxMagnitudeHz, -1, 1);
    invalid.translationHz = kMaxMagnitudeHz;
    check(occupiedInterval(invalid).error == Error::InvalidInterval, "bounded operands cannot overflow RF result");
    invalid = slice(0, 1, -2, 1);
    check(occupiedInterval(invalid).error == Error::InvalidInterval, "negative occupied RF rejected");
    CaptureDescriptor bad = good;
    bad.captureId = 0;
    check(selectCenter(bad, one, kDomains).error == Error::InvalidIdentity, "zero capture identity rejected");
    bad = good;
    bad.generation = 0;
    check(selectCenter(bad, one, kDomains).error == Error::InvalidIdentity, "zero readback generation rejected");
    for (const double rate : {0.0, -1.0, 199.0}) {
        bad = good;
        bad.achievedSampleRateHz = rate;
        check(selectCenter(bad, one, kDomains).error == Error::InvalidInterval,
              "achieved rate must be positive and contain both usable extents within Nyquist");
    }
    bad = good;
    bad.usableLeftHz = 0;
    bad.usableRightHz = 0;
    check(selectCenter(bad, one, kDomains).error == Error::InvalidInterval, "empty whole usable interval rejected");
    const double denormal = std::numeric_limits<double>::denorm_min();
    bad = capture(0, 2 * denormal, 0);
    bad.achievedSampleRateHz = 3 * denormal;
    check(selectCenter(bad, {}, kDomains).error == Error::InvalidInterval,
          "subnormal rate cannot round half-rate upward and admit excess usable extent");
    bad = good;
    bad.usableRightHz = -1;
    check(selectCenter(bad, one, kDomains).error == Error::InvalidInterval, "negative usable right extent rejected");
    check(selectCenter(good, one, {}).error == Error::InvalidDomains, "empty center domains rejected");
    for (const CenterDomain& invalidDomain : {CenterDomain{1000, 999, 0, 1}, {1, 2, 0, 0},
             {1, 2, 0, -1}, {1000, 1000, 0, 3}, {1, 2, 0, 1e-300}, {-1, 2, 0, 1}, {1, 2, -1, 1}}) {
        const std::array<CenterDomain, 1> domain{{invalidDomain}};
        check(selectCenter(good, one, domain).error == Error::InvalidDomains,
              "invalid, empty-grid, unresolved or overflowing-index domain rejected");
    }
    // The capture and slice otherwise fit; small indices leave the resolution
    // guard as the reason to reject this grid's colliding center values.
    constexpr double kLargeGridOriginHz = 549755813888.0; // 2^39; ULP is 2^-13.
    const std::array<CenterDomain, 1> unresolvedGrid{{
        {kLargeGridOriginHz, kLargeGridOriginHz + 1, kLargeGridOriginHz, 1e-5}}};
    const std::array<SliceDescriptor, 1> highSlice{{slice(0, kLargeGridOriginHz)}};
    check(selectCenter(capture(kLargeGridOriginHz), highSlice, unresolvedGrid).error == Error::InvalidDomains,
          "sub-ULP center step rejected even when grid indices are bounded");
    const std::array<SliceDescriptor, 2> duplicate{{one[0], one[0]}};
    check(selectCenter(good, duplicate, kDomains).error == Error::DuplicateId, "duplicate desired slots refuse whole request");
    const std::vector<SliceDescriptor> oversized(kMaxEntries + 1, one[0]);
    check(selectCenter(good, oversized, kDomains).error == Error::TooManyEntries,
          "desired collection size bounded before iteration");
    const std::vector<CenterDomain> tooManyDomains(kMaxDomains + 1, kDomains[0]);
    check(selectCenter(good, one, tooManyDomains).error == Error::InvalidDomains,
          "hardware-domain collection bounded before iteration");
}

void restorePlan()
{
    const CaptureDescriptor current = capture();
    std::vector<SliceDescriptor> saved{slice(8, 1020), slice(2, 1000), slice(7, 1400),
        slice(4, 1010), slice(4, 990), slice(-1, 1000), slice(1, 980), slice(6, 1030)};
    saved[4].guardLowHz = -1; // Invalid duplicate still invalidates both ID-4 rows.
    saved[7].filterHighHz = std::numeric_limits<double>::infinity();
    const RestoreResult result = restoreFixedCapture(current, saved, kDomains, 2);
    check(result.error == Error::None && result.accepted.size() == 2
          && result.accepted[0] == saved[6] && result.accepted[1] == saved[1],
          "restore keeps fitting ascending stable IDs 1,2 without renumbering");
    const std::array<std::size_t, 6> indices{{0, 2, 3, 4, 5, 7}};
    const std::array<Error, 6> reasons{{Error::CapacityExceeded, Error::OutsideCapture,
        Error::DuplicateId, Error::DuplicateId, Error::InvalidIdentity, Error::InvalidNumber}};
    check(result.rejected.size() == reasons.size(), "every refused restore row has explicit reason");
    for (std::size_t i = 0; i < std::min(reasons.size(), result.rejected.size()); ++i) {
        check(result.rejected[i].inputIndex == indices[i] && result.rejected[i].reason == reasons[i],
              "restore rejection is indexed to original input and deterministic");
    }
    std::reverse(saved.begin(), saved.end());
    const RestoreResult reordered = restoreFixedCapture(current, saved, kDomains, 2);
    check(reordered.accepted == result.accepted, "restore survivor set independent of input order");
    const RestoreResult zero = restoreFixedCapture(current, saved, kDomains, 0);
    check(zero.error == Error::None && zero.accepted.empty() && zero.rejected.size() == saved.size(),
          "validated zero capacity rejects every row without partial application");
    const RestoreResult invalidCapacity = restoreFixedCapture(current, saved, kDomains, kMaxEntries + 1);
    check(invalidCapacity.error == Error::InvalidCapacity && invalidCapacity.accepted.empty()
          && invalidCapacity.rejected.empty(), "invalid capacity gives no partial restore plan");
    const std::vector<SliceDescriptor> oversized(kMaxEntries + 1, slice(0, 1000));
    check(restoreFixedCapture(current, oversized, kDomains, 2).error == Error::TooManyEntries,
          "restore parser collection ceiling enforced");
    const std::array<SliceDescriptor, 1> far{{slice(0, 1500)}};
    const RestoreResult refused = restoreFixedCapture(current, far, kDomains, 1);
    check(refused.accepted.empty() && refused.rejected.size() == 1
          && refused.rejected[0].reason == Error::OutsideCapture && current.centerHz == 1000,
          "restore never silently recenters even when one remembered receiver could be retuned into view");
    CaptureDescriptor reconnected = current;
    reconnected.captureId = 9;
    reconnected.generation = 80;
    check(restoreFixedCapture(reconnected, saved, kDomains, 2).accepted == result.accepted,
          "stored RF configuration is independent of old session identity/generation");
    CaptureDescriptor invalidCapture = current;
    invalidCapture.centerHz = 1000.5;
    const RestoreResult noPlan = restoreFixedCapture(invalidCapture, saved, kDomains, 2);
    check(noPlan.error == Error::NoLegalCenter && noPlan.accepted.empty() && noPlan.rejected.empty(),
          "invalid actual capture rejects global restore before any partial plan");
    std::vector<SliceDescriptor> bounded;
    for (std::size_t i = 0; i < kMaxEntries; ++i) {
        bounded.push_back(slice(static_cast<std::int32_t>(kMaxEntries - i - 1), 1000));
    }
    const RestoreResult maximum = restoreFixedCapture(current, bounded, kDomains, kMaxEntries);
    check(maximum.error == Error::None && maximum.accepted.size() == kMaxEntries
          && maximum.accepted.front().stableId == 0 && maximum.accepted.back().stableId == 4095,
          "parser ceiling is inclusive and does not hardcode a small architecture RX count");
}
} // namespace

int main()
{
    geometryAndWholeSet();
    gridsAndDomains();
    slotBoundsAndUnusedCapture();
    readbackAndPrecision();
    malformedInputs();
    restorePlan();
    std::fprintf(stderr, "shared_capture_policy_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
