#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace AetherSDR::txlin {

// Safety interlocks for the TX linearity analyzer.
//
// A two-tone test is a high-duty-cycle, PEP-heavy transmission deliberately
// driven towards the region where the amplifier misbehaves — that is the whole
// point of the measurement. It is therefore the most hardware-hostile thing
// this application can ask a radio to do, and it happens under the operator's
// callsign and legal responsibility.
//
// Everything here is Qt-free and clock-injected: every decision is a pure
// function of (now, state, telemetry), so the timeout, the duty-cycle limiter,
// the telemetry aborts and the band-plan refusal are all unit-testable without
// a radio, without a GUI and without sleeping. Interlocks that can only be
// tested by transmitting are interlocks that do not get tested.
//
// Constitution Principle VI — AetherSDR never transmits without operator
// intent — is the governing constraint: every path here fails CLOSED. When
// intent, telemetry or the capture stream is in any doubt, the answer is "do
// not key" or "unkey now", never "carry on and see".

struct TxTelemetry {
    // All optional because a meter that has stopped updating is a different
    // condition from a meter reading zero, and the two must not be confused.
    // MeterModel already distinguishes them (swrIfLive(), the per-meter
    // freshness timestamps); this type preserves that distinction across the
    // seam instead of flattening it to 0.0.
    std::optional<double> swr;
    std::optional<double> paTempC;
    std::optional<double> supplyVolts;
    std::optional<double> forwardWatts;
};

struct SafetyLimits {
    // Hard transmit timeout. Cut TX unconditionally on expiry — not "ask the
    // user", not "finish the current average".
    int maxTransmitMs = 10000;

    // Minimum idle time between test transmissions. A two-tone at PEP is close
    // to worst case for PA dissipation and the finals need the recovery time
    // whether or not the temperature meter has noticed yet.
    int minCooldownMs = 30000;

    // Telemetry abort thresholds. Chosen to trip before the radio's own
    // protection does, so the analyzer stops the test rather than the radio
    // stopping the transmitter — a cleaner failure, and one the operator can
    // see the reason for.
    double maxSwr = 2.0;
    double maxPaTempC = 75.0;
    double minSupplyVolts = 11.0;

    // How stale telemetry may be before the run aborts. A radio that has
    // stopped reporting SWR during a high-power test is not a radio to keep
    // transmitting on.
    int telemetryMaxAgeMs = 2000;
};

// The band the test is allowed to occupy, in absolute Hz. Fed from
// BandPlanManager by the Qt layer so this file stays free of it.
struct AllocationRange {
    double lowHz = 0.0;
    double highHz = 0.0;
};

struct RunRequest {
    // Absolute frequency of the lower and upper test tones.
    double tone1Hz = 0.0;
    double tone2Hz = 0.0;
    // Highest odd IMD order the test will generate meaningfully. Order 7 puts
    // products 3 tone-spacings beyond each tone, so the occupied bandwidth is
    // markedly wider than the tone pair — which is exactly what a band-edge
    // check has to account for and what an eyeball estimate always forgets.
    int maxOrder = 7;
    std::vector<AllocationRange> allocations;
    bool captureReady = false;
};

enum class PreflightVerdict {
    Ok,
    AlreadyRunning,
    CoolingDown,
    NoDummyLoadConfirmation,
    NoCaptureSource,
    InvalidToneSpacing,
    OutsideAllocation,
};

struct PreflightResult {
    PreflightVerdict verdict = PreflightVerdict::Ok;
    std::string message;
    // Non-blocking advisories the UI must show but which do not by themselves
    // refuse the run — band-edge proximity being the main one.
    std::vector<std::string> warnings;
    // The span the test will actually occupy, including IMD products, so the
    // UI can state it rather than leaving the operator to work it out.
    double occupiedLowHz = 0.0;
    double occupiedHighHz = 0.0;

    [[nodiscard]] bool ok() const { return verdict == PreflightVerdict::Ok; }
};

enum class AbortReason {
    None,
    TransmitTimeout,
    HighSwr,
    PaOverTemp,
    LowSupplyVoltage,
    TelemetryStale,
    CaptureLost,
    OperatorStop,
};

std::string abortReasonText(AbortReason reason);

class TxLinearitySafety {
public:
    TxLinearitySafety() = default;
    explicit TxLinearitySafety(const SafetyLimits& limits) : m_limits(limits) {}

    [[nodiscard]] const SafetyLimits& limits() const { return m_limits; }

    // Apply operator-adjusted limits.
    //
    // The transmit timeout is adjustable DOWNWARD only unless `allowOverride`
    // is set: a limit that any dialog can quietly raise is not a limit. The
    // same rule applies to the cooldown, for the same reason. Returns false
    // (and applies nothing) when the request would loosen a limit without an
    // override, so the caller can tell the operator why rather than silently
    // clamping.
    bool setLimits(const SafetyLimits& limits, bool allowOverride = false);

    // ── Dummy-load confirmation ─────────────────────────────────────────────
    //
    // Deliberately session-scoped and single-use: cleared by noteRunStopped()
    // and never written to AppSettings. §8 requires explicit confirmation
    // before ANY test transmission and requires it not to be remembered across
    // sessions — the antenna on the other end of the coax is not a preference,
    // it is a fact that can change between one run and the next.
    void confirmDummyLoad() { m_dummyLoadConfirmed = true; }
    void clearDummyLoadConfirmation() { m_dummyLoadConfirmed = false; }
    [[nodiscard]] bool dummyLoadConfirmed() const { return m_dummyLoadConfirmed; }

    [[nodiscard]] PreflightResult preflight(std::int64_t nowMs,
                                            const RunRequest& request) const;

    void noteRunStarted(std::int64_t nowMs);
    void noteRunStopped(std::int64_t nowMs);
    [[nodiscard]] bool running() const { return m_running; }

    // Evaluate the interlocks against the current telemetry. Call on every
    // telemetry update AND on a timer, so a radio that stops reporting entirely
    // still trips TelemetryStale rather than transmitting until the timeout.
    //
    // `lastTelemetryMs` is when the telemetry snapshot was last refreshed.
    [[nodiscard]] AbortReason evaluate(std::int64_t nowMs,
                                       const TxTelemetry& telemetry,
                                       std::int64_t lastTelemetryMs) const;

    [[nodiscard]] std::int64_t transmitElapsedMs(std::int64_t nowMs) const;
    [[nodiscard]] std::int64_t cooldownRemainingMs(std::int64_t nowMs) const;

private:
    SafetyLimits m_limits;
    bool m_dummyLoadConfirmed = false;
    bool m_running = false;
    std::int64_t m_runStartedMs = 0;
    std::int64_t m_runStoppedMs = 0;
    bool m_everRan = false;
};

}  // namespace AetherSDR::txlin
