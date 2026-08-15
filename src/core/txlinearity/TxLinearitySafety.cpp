#include "TxLinearitySafety.h"

#include <algorithm>
#include <cmath>

namespace AetherSDR::txlin {
namespace {

// Advisory clearance between the occupied span and the allocation edge. Below
// this the run is still permitted — the products are inside the band — but the
// operator is told, because the tone estimates and the radio's own filtering
// both carry uncertainty and "just inside" is not a comfortable place to put a
// -30 dBc product.
constexpr double kBandEdgeWarnHz = 500.0;

std::string hzText(double hz)
{
    // Plain kHz with three decimals: this text goes into operator-facing
    // refusal messages, where a raw float in Hz is unreadable.
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.3f kHz", hz / 1000.0);
    return std::string(buf);
}

}  // namespace

std::string abortReasonText(AbortReason reason)
{
    switch (reason) {
    case AbortReason::None:             return "";
    case AbortReason::TransmitTimeout:  return "Transmit timeout reached";
    case AbortReason::HighSwr:          return "SWR above the configured limit";
    case AbortReason::PaOverTemp:       return "PA temperature above the configured limit";
    case AbortReason::LowSupplyVoltage: return "Supply voltage below the configured limit";
    case AbortReason::TelemetryStale:   return "Radio telemetry stopped updating";
    case AbortReason::CaptureLost:      return "Capture stream lost";
    case AbortReason::OperatorStop:     return "Stopped by the operator";
    }
    return "";
}

bool TxLinearitySafety::setLimits(const SafetyLimits& limits, bool allowOverride)
{
    if (!allowOverride) {
        if (limits.maxTransmitMs > m_limits.maxTransmitMs) {
            return false;
        }
        if (limits.minCooldownMs < m_limits.minCooldownMs) {
            return false;
        }
        if (limits.maxSwr > m_limits.maxSwr || limits.maxPaTempC > m_limits.maxPaTempC) {
            return false;
        }
        if (limits.minSupplyVolts < m_limits.minSupplyVolts) {
            return false;
        }
    }
    m_limits = limits;
    // A zero or negative timeout would mean "no timeout", which is the one
    // configuration §8 does not allow to exist.
    m_limits.maxTransmitMs = std::max(1, m_limits.maxTransmitMs);
    m_limits.minCooldownMs = std::max(0, m_limits.minCooldownMs);
    return true;
}

PreflightResult TxLinearitySafety::preflight(std::int64_t nowMs,
                                             const RunRequest& request) const
{
    PreflightResult out;

    const double lo = std::min(request.tone1Hz, request.tone2Hz);
    const double hi = std::max(request.tone1Hz, request.tone2Hz);
    const double spacing = hi - lo;
    // Occupied span including the highest odd product the test generates:
    // order N reaches ((N-1)/2) tone spacings beyond each tone.
    const double reach = spacing * double(std::max(1, (request.maxOrder - 1) / 2));
    out.occupiedLowHz = lo - reach;
    out.occupiedHighHz = hi + reach;

    // Order matters: report the condition the operator can act on first. A
    // cooldown message while the real problem is an unconfirmed dummy load
    // wastes 30 seconds before showing the actual blocker.
    if (m_running) {
        out.verdict = PreflightVerdict::AlreadyRunning;
        out.message = "A measurement is already running";
        return out;
    }
    if (spacing <= 0.0) {
        out.verdict = PreflightVerdict::InvalidToneSpacing;
        out.message = "The two test tones must be at different frequencies";
        return out;
    }
    if (!request.captureReady) {
        out.verdict = PreflightVerdict::NoCaptureSource;
        out.message = "No TX capture source is running";
        return out;
    }
    if (!m_dummyLoadConfirmed) {
        out.verdict = PreflightVerdict::NoDummyLoadConfirmation;
        out.message = "Confirm a dummy load or a verified-clear frequency before transmitting";
        return out;
    }
    const std::int64_t cooldown = cooldownRemainingMs(nowMs);
    if (cooldown > 0) {
        out.verdict = PreflightVerdict::CoolingDown;
        out.message = "Cooling down — " + std::to_string((cooldown + 999) / 1000)
                      + " s remaining before the next test transmission";
        return out;
    }

    // ── Band plan ───────────────────────────────────────────────────────────
    //
    // The TONES must sit inside one allocation: a test that puts a carrier
    // outside the licensed service's allocation is not a measurement, it is an
    // out-of-band emission. The PRODUCTS crossing an edge is a warning rather
    // than a refusal — they are real emissions and the operator must know, but
    // they are also 30+ dB down and the radio's own TX filtering acts on them,
    // so refusing outright would block legitimate near-edge measurements that
    // are the very ones worth making.
    //
    // An empty allocation list means the band plan has not loaded. Fail closed:
    // no data is not permission.
    const auto containsTones = [&](const AllocationRange& r) {
        return lo >= r.lowHz && hi <= r.highHz;
    };
    const auto it = std::find_if(request.allocations.begin(), request.allocations.end(),
                                 containsTones);
    if (it == request.allocations.end()) {
        out.verdict = PreflightVerdict::OutsideAllocation;
        out.message = request.allocations.empty()
                          ? "No band plan is loaded, so the test frequency cannot be verified"
                          : ("Test tones at " + hzText(lo) + " to " + hzText(hi)
                             + " are outside the licensed amateur allocation");
        return out;
    }

    if (out.occupiedLowHz < it->lowHz) {
        out.warnings.push_back(
            "Order-" + std::to_string(request.maxOrder) + " products reach "
            + hzText(out.occupiedLowHz) + ", below the band edge at " + hzText(it->lowHz));
    } else if (out.occupiedLowHz - it->lowHz < kBandEdgeWarnHz) {
        out.warnings.push_back("Occupied bandwidth comes within "
                               + hzText(out.occupiedLowHz - it->lowHz)
                               + " of the lower band edge");
    }
    if (out.occupiedHighHz > it->highHz) {
        out.warnings.push_back(
            "Order-" + std::to_string(request.maxOrder) + " products reach "
            + hzText(out.occupiedHighHz) + ", above the band edge at " + hzText(it->highHz));
    } else if (it->highHz - out.occupiedHighHz < kBandEdgeWarnHz) {
        out.warnings.push_back("Occupied bandwidth comes within "
                               + hzText(it->highHz - out.occupiedHighHz)
                               + " of the upper band edge");
    }

    out.verdict = PreflightVerdict::Ok;
    return out;
}

void TxLinearitySafety::noteRunStarted(std::int64_t nowMs)
{
    m_running = true;
    m_runStartedMs = nowMs;
    m_everRan = true;
}

void TxLinearitySafety::noteRunStopped(std::int64_t nowMs)
{
    m_running = false;
    m_runStoppedMs = nowMs;
    // Single-use confirmation. The next run asks again — see the header.
    m_dummyLoadConfirmed = false;
}

AbortReason TxLinearitySafety::evaluate(std::int64_t nowMs,
                                        const TxTelemetry& telemetry,
                                        std::int64_t lastTelemetryMs) const
{
    if (!m_running) {
        return AbortReason::None;
    }

    // The timeout is checked first and unconditionally. Every other interlock
    // depends on data arriving; this one does not, which is precisely why it is
    // the backstop.
    if (transmitElapsedMs(nowMs) >= std::int64_t(m_limits.maxTransmitMs)) {
        return AbortReason::TransmitTimeout;
    }

    if (nowMs - lastTelemetryMs > std::int64_t(m_limits.telemetryMaxAgeMs)) {
        return AbortReason::TelemetryStale;
    }

    // SWR is only meaningful with real forward power behind it — a reflected
    // reading taken against a near-zero forward reading is a ratio of two noise
    // samples, and tripping on it would abort every run during key-up. This
    // mirrors MeterModel's own kMinForwardWattsForSwr gate.
    if (telemetry.swr.has_value()
        && (!telemetry.forwardWatts.has_value() || *telemetry.forwardWatts > 0.5)
        && *telemetry.swr > m_limits.maxSwr) {
        return AbortReason::HighSwr;
    }
    if (telemetry.paTempC.has_value() && *telemetry.paTempC > m_limits.maxPaTempC) {
        return AbortReason::PaOverTemp;
    }
    if (telemetry.supplyVolts.has_value() && *telemetry.supplyVolts < m_limits.minSupplyVolts) {
        return AbortReason::LowSupplyVoltage;
    }
    return AbortReason::None;
}

std::int64_t TxLinearitySafety::transmitElapsedMs(std::int64_t nowMs) const
{
    return m_running ? std::max<std::int64_t>(0, nowMs - m_runStartedMs) : 0;
}

std::int64_t TxLinearitySafety::cooldownRemainingMs(std::int64_t nowMs) const
{
    if (!m_everRan || m_running) {
        return 0;
    }
    const std::int64_t elapsed = nowMs - m_runStoppedMs;
    return std::max<std::int64_t>(0, std::int64_t(m_limits.minCooldownMs) - elapsed);
}

}  // namespace AetherSDR::txlin
