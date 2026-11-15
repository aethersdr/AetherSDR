#include "core/txlinearity/TxLinearitySafety.h"

#include <cstdio>
#include <cmath>
#include <limits>

using namespace AetherSDR::txlin;

static int g_failures = 0;

static void check(bool cond, const char* what)
{
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

namespace {

// 20 m, IARU/FCC general segment boundaries in Hz. Only used as plausible
// numbers — the real values come from BandPlanManager at runtime.
constexpr double kBandLow = 14000000.0;
constexpr double kBandHigh = 14350000.0;

TxTelemetry samples(std::int64_t stamp, double swr = 1.2, double temp = 40.0,
                    double volts = 13.8, double watts = 100.0)
{
    return {{swr, stamp}, {temp, stamp}, {volts, stamp}, {watts, stamp}};
}

RunRequest makeRequest(double tone1Hz, double tone2Hz)
{
    RunRequest req;
    req.tone1Hz = tone1Hz;
    req.tone2Hz = tone2Hz;
    req.maxOrder = 7;
    req.captureReady = true;
    req.allocations.push_back(AllocationRange{kBandLow, kBandHigh});
    return req;
}

}  // namespace

int main()
{
    // ── Dummy-load confirmation is required and is never remembered ─────────
    {
        TxLinearitySafety safety;
        const RunRequest req = makeRequest(14200000.0, 14202000.0);

        check(safety.preflight(0, req).verdict == PreflightVerdict::NoDummyLoadConfirmation,
              "preflight refuses without a dummy-load confirmation");

        safety.confirmDummyLoad();
        check(safety.preflight(0, req).ok(), "preflight passes once confirmed");

        safety.noteRunStarted(0);
        safety.noteRunStopped(1000);
        check(!safety.dummyLoadConfirmed(),
              "the confirmation is cleared when the run stops, so the next run asks again");
    }

    // ── Hard transmit timeout ───────────────────────────────────────────────
    {
        SafetyLimits limits;
        limits.maxTransmitMs = 10000;
        TxLinearitySafety safety(limits);
        safety.confirmDummyLoad();
        safety.noteRunStarted(100000);

        check(safety.evaluate(105000, samples(105000)) == AbortReason::None,
              "a healthy run partway through the window does not abort");
        check(safety.evaluate(109999, samples(109999)) == AbortReason::None,
              "the timeout has not tripped one millisecond early");
        check(safety.evaluate(110000, samples(110000)) == AbortReason::TransmitTimeout,
              "the timeout trips exactly at the limit");
        // The timeout must not depend on telemetry arriving. A radio that has
        // gone silent must still be unkeyed at the deadline.
        check(safety.evaluate(120000, TxTelemetry{}) == AbortReason::TransmitTimeout,
              "the timeout trips even with no telemetry at all");
    }

    // ── Duty-cycle limiter ──────────────────────────────────────────────────
    {
        SafetyLimits limits;
        limits.minCooldownMs = 30000;
        TxLinearitySafety safety(limits);
        const RunRequest req = makeRequest(14200000.0, 14202000.0);

        safety.confirmDummyLoad();
        safety.noteRunStarted(0);
        safety.noteRunStopped(5000);

        check(safety.cooldownRemainingMs(5000) == 30000, "cooldown starts at the full period");
        check(safety.cooldownRemainingMs(20000) == 15000, "cooldown counts down");

        safety.confirmDummyLoad();
        check(safety.preflight(20000, req).verdict == PreflightVerdict::CoolingDown,
              "preflight refuses during the cooldown");
        check(safety.cooldownRemainingMs(35000) == 0, "cooldown expires");
        check(safety.preflight(35000, req).ok(), "preflight passes after the cooldown");
    }

    // ── Limits are adjustable downward only ─────────────────────────────────
    {
        TxLinearitySafety safety;
        SafetyLimits tighter = safety.limits();
        tighter.maxTransmitMs = 5000;
        check(safety.setLimits(tighter), "tightening the timeout is allowed");
        check(safety.limits().maxTransmitMs == 5000, "the tightened timeout applied");

        SafetyLimits looser = safety.limits();
        looser.maxTransmitMs = 60000;
        check(!safety.setLimits(looser), "loosening the timeout needs an explicit override");
        check(safety.limits().maxTransmitMs == 5000,
              "a rejected limit change applies nothing at all");
        check(safety.setLimits(looser, true), "an explicit override loosens the timeout");
        check(safety.limits().maxTransmitMs == 60000, "the override applied");

        SafetyLimits shortCooldown = safety.limits();
        shortCooldown.minCooldownMs = 0;
        check(!safety.setLimits(shortCooldown), "shortening the cooldown needs an override too");

        SafetyLimits zeroTimeout = safety.limits();
        zeroTimeout.maxTransmitMs = 0;
        check(safety.setLimits(zeroTimeout), "a zero timeout is accepted as a tightening");
        check(safety.limits().maxTransmitMs >= 1,
              "but 'no timeout' is not a configuration that can exist");
    }

    // ── Telemetry aborts ────────────────────────────────────────────────────
    {
        TxLinearitySafety safety;
        safety.confirmDummyLoad();
        safety.noteRunStarted(0);

        check(safety.evaluate(1000, samples(1000, 3.5, 40.0, 13.8, 100.0))
                  == AbortReason::HighSwr,
              "high SWR with real forward power aborts");
        // The same SWR reading with no power behind it is a ratio of two noise
        // samples, and tripping on it would abort every run during key-up.
        check(safety.evaluate(1000, samples(1000, 3.5, 40.0, 13.8, 0.01))
                  == AbortReason::None,
              "high SWR with no forward power does not abort");
        check(safety.evaluate(1000, samples(1000, 1.2, 95.0, 13.8, 100.0))
                  == AbortReason::PaOverTemp,
              "PA over-temperature aborts");
        check(safety.evaluate(1000, samples(1000, 1.2, 40.0, 9.5, 100.0))
                  == AbortReason::LowSupplyVoltage,
              "low supply voltage aborts");
        check(safety.evaluate(5000, samples(1000, 1.2, 40.0, 13.8, 100.0))
                  == AbortReason::TelemetryStale,
              "telemetry that has stopped updating aborts even when the last values were fine");

        safety.noteRunStopped(6000);
        check(safety.evaluate(7000, samples(7000, 3.5, 95.0, 9.5, 100.0))
                  == AbortReason::None,
              "a stopped run has nothing to abort");
    }

    // ── Band plan ───────────────────────────────────────────────────────────
    {
        TxLinearitySafety safety;
        safety.confirmDummyLoad();

        // No band plan loaded is not permission to transmit.
        RunRequest noPlan = makeRequest(14200000.0, 14202000.0);
        noPlan.allocations.clear();
        check(safety.preflight(0, noPlan).verdict == PreflightVerdict::OutsideAllocation,
              "an unloaded band plan fails closed");

        check(safety.preflight(0, makeRequest(13900000.0, 13902000.0)).verdict
                  == PreflightVerdict::OutsideAllocation,
              "tones outside the allocation are refused");

        // Mid-band: inside, and far enough from both edges that nothing warns.
        const PreflightResult mid = safety.preflight(0, makeRequest(14200000.0, 14202000.0));
        check(mid.ok(), "a mid-band run is permitted");
        check(mid.warnings.empty(), "a mid-band run raises no warnings");
        // Order 7 reaches three tone-spacings beyond each tone: 2 kHz spacing
        // means 6 kHz of products either side, so 14.194–14.208 MHz.
        check(std::abs(mid.occupiedLowHz - 14194000.0) < 1.0,
              "occupied span accounts for order-7 products below");
        check(std::abs(mid.occupiedHighHz - 14208000.0) < 1.0,
              "occupied span accounts for order-7 products above");

        // Tones inside the band but products spilling over the edge: permitted,
        // with an explicit warning. Refusing outright would block exactly the
        // near-edge measurements worth making, but the operator must be told.
        const PreflightResult edge = safety.preflight(0, makeRequest(14001000.0, 14003000.0));
        check(edge.ok(), "a near-edge run with spilling products is still permitted");
        check(!edge.warnings.empty(), "but it warns about the band edge");

        // Just inside with clearance under the advisory threshold.
        const PreflightResult tight = safety.preflight(0, makeRequest(14006200.0, 14008200.0));
        check(tight.ok(), "a run with small but positive clearance is permitted");
        check(!tight.warnings.empty(), "and warns about the proximity");
    }

    // ── Ordering and prerequisites ──────────────────────────────────────────
    {
        TxLinearitySafety safety;
        RunRequest noCapture = makeRequest(14200000.0, 14202000.0);
        noCapture.captureReady = false;
        check(safety.preflight(0, noCapture).verdict == PreflightVerdict::NoCaptureSource,
              "a missing capture source is reported before the dummy-load prompt");

        safety.confirmDummyLoad();
        RunRequest sameFreq = makeRequest(14200000.0, 14200000.0);
        check(safety.preflight(0, sameFreq).verdict == PreflightVerdict::InvalidToneSpacing,
              "two tones at the same frequency are refused");

        safety.noteRunStarted(0);
        check(safety.preflight(0, makeRequest(14200000.0, 14202000.0)).verdict
                  == PreflightVerdict::AlreadyRunning,
              "a second run cannot start while one is in progress");
    }

    // Polling a cached snapshot does not extend any sensor's lifetime.
    {
        TxLinearitySafety safety;
        safety.noteRunStarted(1000);
        const TxTelemetry original = samples(1000);
        check(safety.evaluate(3000, original) == AbortReason::None,
              "samples remain usable at the exact age limit");
        for (std::int64_t now = 3001; now < 4000; now += 100) {
            check(safety.evaluate(now, original) == AbortReason::TelemetryStale,
                  "repeated polls cannot refresh a cached snapshot");
        }
        for (const auto member : {&TxTelemetry::swr, &TxTelemetry::paTempC,
                                  &TxTelemetry::supplyVolts, &TxTelemetry::forwardWatts}) {
            TxTelemetry oneStale = samples(4000);
            (oneStale.*member).observedAtMs = 1000;
            check(safety.evaluate(4000, oneStale) == AbortReason::TelemetryStale,
                  "fresh other meters cannot hide one stale safety meter");
            (oneStale.*member).observedAtMs.reset();
            check(safety.evaluate(4000, oneStale) == AbortReason::TelemetryStale,
                  "a value without a sample time is refused");
            (oneStale.*member).observedAtMs = 4001;
            check(safety.evaluate(4000, oneStale) == AbortReason::TelemetryStale,
                  "a future sample time is refused");
            (oneStale.*member).observedAtMs = 4000;
            (oneStale.*member).value = std::numeric_limits<double>::quiet_NaN();
            check(safety.evaluate(4000, oneStale) == AbortReason::TelemetryStale,
                  "a nonfinite reading is refused");
        }
        check(safety.evaluate(4000, samples(4000)) == AbortReason::None,
              "unchanged readings with actual new samples remain fresh");
        TxTelemetry lowPower = samples(4000, 1.2, 40.0, 13.8, 0.0);
        lowPower.swr = {};
        check(safety.evaluate(4000, lowPower) == AbortReason::None,
              "SWR may be absent while forward power is below its usable floor");
        check(safety.evaluate(4000, TxTelemetry{}) == AbortReason::TelemetryStale,
              "missing telemetry cannot be made fresh by polling");
        check(safety.evaluate(999, original) == AbortReason::TransmitTimeout,
              "a backwards clock fails closed");
    }
    {
        TxLinearitySafety safety;
        SafetyLimits bad = safety.limits();
        bad.maxSwr = std::numeric_limits<double>::quiet_NaN();
        check(!safety.setLimits(bad, true), "an override cannot install NaN thresholds");
        check(safety.limits().maxSwr == 2.0, "invalid updates preserve the old limits");
        bad = safety.limits();
        bad.telemetryMaxAgeMs += 1;
        check(!safety.setLimits(bad), "loosening telemetry age requires an override");
        safety.confirmDummyLoad();
        RunRequest badRequest = makeRequest(14200000.0, 14202000.0);
        badRequest.tone1Hz = std::numeric_limits<double>::quiet_NaN();
        check(!safety.preflight(0, badRequest).ok(), "NaN tones fail preflight");
        badRequest = makeRequest(14200000.0, 14202000.0);
        badRequest.maxOrder = std::numeric_limits<int>::min();
        check(!safety.preflight(0, badRequest).ok(), "unbounded IMD orders fail preflight");
    }

    {
        TxLinearitySafety safety;
        SafetyLimits limits = safety.limits();
        limits.vitalsMaxAgeMs = 3000;
        check(!safety.setLimits(limits), "slower vital expiry requires an explicit override");
        check(safety.setLimits(limits, true), "independent slow-vital limit accepted");
        safety.noteRunStarted(10000);
        TxTelemetry telemetry = samples(12060);
        telemetry.paTempC.observedAtMs = 10000;
        telemetry.supplyVolts.observedAtMs = 10000;
        check(safety.evaluate(12060, telemetry) == AbortReason::None,
              "healthy 2.06-second vital cadence does not expire between packets");
        for (const auto member : {&TxTelemetry::forwardWatts, &TxTelemetry::swr}) {
            TxTelemetry stale = telemetry;
            (stale.*member).observedAtMs = 10000;
            check(safety.evaluate(12060, stale) == AbortReason::TelemetryStale,
                  "slow vital cadence cannot extend power or SWR lifetime");
        }
        check(safety.evaluate(13000, telemetry) == AbortReason::None,
              "vitals remain valid at the exact configured deadline");
        for (std::int64_t now = 13001; now < 14000; now += 100) {
            telemetry.forwardWatts.observedAtMs = now;
            telemetry.swr.observedAtMs = now;
            check(safety.evaluate(now, telemetry) == AbortReason::TelemetryStale,
                  "fresh power and repeated reads cannot prolong missing vitals");
        }
        for (const auto member : {&TxTelemetry::paTempC, &TxTelemetry::supplyVolts}) {
            TxTelemetry stale = samples(14000);
            (stale.*member).observedAtMs = 10000;
            check(safety.evaluate(14000, stale) == AbortReason::TelemetryStale,
                  "either missing vital independently expires");
        }
        check(safety.evaluate(12060, samples(10000, 1.2, 95.0, 13.8, 0.1))
                  == AbortReason::TelemetryStale, "slow vitals never conceal expired forward power");
        telemetry = samples(12060);
        telemetry.paTempC = {95.0, 10000};
        check(safety.evaluate(12060, telemetry) == AbortReason::PaOverTemp,
              "a valid slower over-temperature sample still trips immediately");
        telemetry.paTempC = {40.0, 10000};
        telemetry.supplyVolts = {9.0, 10000};
        check(safety.evaluate(12060, telemetry) == AbortReason::LowSupplyVoltage,
              "a valid slower undervoltage sample still trips immediately");
        limits.vitalsMaxAgeMs = 0;
        check(!safety.setLimits(limits, true), "vital freshness cannot be disabled");
    }

    if (g_failures == 0) {
        std::printf("tx_linearity_safety_test: all checks passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}
