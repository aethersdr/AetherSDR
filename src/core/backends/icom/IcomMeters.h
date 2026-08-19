#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "core/backends/icom/CivCodec.h"

// Phase 4 — metering.
//
// This is the first backend where METERING POLICY is a real engineering
// constraint rather than a subscription. A Flex streams meters over VITA-49 and
// an HL2 embeds them in the IQ header; an Icom returns a meter value only when
// asked, one at a time, over the SAME CI-V stream that carries tuning. On WiFi
// each poll costs a 5-30 ms round trip.
//
// So there are two halves here and they are deliberately separate:
//
//   * CALIBRATION — raw 0..255 to real units. Non-linear, per-model, and the
//     published breakpoints are the only truth. Fitting a line through the
//     endpoints reads several S-units wrong mid-scale.
//   * SCHEDULING  — what to ask for, how often, and when to shut up so the
//     operator's tuning does not queue behind a meter poll.
//
// Qt-free; icom_meters_test drives both halves with a synthetic clock.
//
// See ~/oracles/icom/icom-oracle.md §6.

namespace AetherSDR::icom {

// ---------------------------------------------------------------------------
// Calibration
// ---------------------------------------------------------------------------

// One published breakpoint. These come from Icom's own CI-V Reference Guide
// where it gives them, and from Hamlib/flrig's measured tables where Icom only
// gives percentages.
struct CurvePoint {
    int raw = 0;
    double value = 0.0;
};

// Piecewise-linear interpolation, clamped at both ends.
//
// LINEAR BETWEEN THE PUBLISHED POINTS, never a single fit through the
// endpoints.
//
// The POWER curve is where this matters most: it is badly non-linear, and a
// straight line from 0 to full scale reads 6.7 W where the radio means 5 W, and
// 1.0 W where it means 0.5 W. The S-meter is milder — its two segments diverge
// from a straight line by at most 2.76 dB, about half an S-unit — but that
// error peaks exactly AT S9, which is the reading operators quote.
//
// Both errors are zero at the endpoints, so a check of only the first and last
// breakpoints passes against a wrong implementation.
[[nodiscard]] double interpolateCurve(std::span<const CurvePoint> curve, int raw);

// The IC-705's published curves. Each is documented at its definition with the
// source it came from.
[[nodiscard]] std::span<const CurvePoint> powerCurveIc705();   // raw -> watts
[[nodiscard]] std::span<const CurvePoint> powerCurveIc7300Mk2(); // raw -> watts
[[nodiscard]] std::span<const CurvePoint> swrCurve();          // raw -> SWR
[[nodiscard]] std::span<const CurvePoint> compCurve();         // raw -> dB
[[nodiscard]] std::span<const CurvePoint> vdCurve();           // raw -> volts
[[nodiscard]] std::span<const CurvePoint> idCurve();           // raw -> amps
[[nodiscard]] std::span<const CurvePoint> alcCurve();          // raw -> percent

// S-meter, in dBm.
//
// The reference is BAND-DEPENDENT, not model-dependent: IARU Region 1 defines
// S9 as -73 dBm below 30 MHz and -93 dBm above it. A single -73 everywhere
// reports VHF signals 20 dB hot, which on a 2 m weak-signal band is the whole
// usable range.
//
// Icom publishes the raw breakpoints (0 = S0, 120 = S9, 241 = S9+60 dB); the
// dBm mapping is the IARU convention applied to them.
inline constexpr double kS9DbmHf  = -73.0;
inline constexpr double kS9DbmVhf = -93.0;
[[nodiscard]] double sMeterDbm(int raw, double s9Dbm);
// True when this frequency uses the VHF-and-above S9 reference.
[[nodiscard]] constexpr bool usesVhfSReference(std::uint64_t hz) noexcept
{
    return hz >= 30'000'000ULL;
}

// ---------------------------------------------------------------------------
// The meter set
// ---------------------------------------------------------------------------

enum class MeterId : std::uint8_t {
    SMeter,
    Power,
    Swr,
    Alc,
    Comp,
    Vd,
    Id,
    Overflow,
};

// When a meter is worth asking for at all.
enum class MeterWhen : std::uint8_t {
    Always,   // meaningful in both states
    RxOnly,   // asking while transmitting wastes a round trip
    TxOnly,   // and reads zero while receiving, which looks like a fault
};

struct MeterSpec {
    MeterId id{};
    std::uint8_t sub = 0;          // the 0x15 subcommand
    // MeterDef::source — "SLC" for receive, "TX" for transmit, "RAD" for the
    // radio itself. NOT cosmetic: consumers look a meter up by source AND name,
    // so a receive level published under "RAD" is invisible to everything that
    // wants "SLC:LEVEL", and radiocert reports it as never defined.
    std::string_view source;
    std::string_view name;         // MeterDef::name
    std::string_view unit;         // MeterDef::unit
    double low = 0.0;
    double high = 0.0;
    MeterWhen when = MeterWhen::Always;
    int intervalMs = 200;
};

[[nodiscard]] std::span<const MeterSpec> meterSpecs();
[[nodiscard]] const MeterSpec* meterSpecFor(MeterId id);
[[nodiscard]] const MeterSpec* meterSpecForSub(std::uint8_t sub);

// Convert a raw reading to the spec's unit. `s9Dbm` selects the S-meter
// reference and is ignored by every other meter.
[[nodiscard]] double meterValue(MeterId id, int raw, double s9Dbm,
                                std::uint8_t civAddress = 0xA4);

// ---------------------------------------------------------------------------
// The poll scheduler
// ---------------------------------------------------------------------------

// Decides which meters to ask for, and when to stay quiet.
//
// Four rules, each of which exists because breaking it has a specific cost:
//
//   1. Poll only what is VISIBLE. An invisible meter's round trip is pure
//      contention on the stream that carries tuning.
//   2. Respect the TX/RX split. Polling SWR while receiving returns zero, which
//      renders as a meter pinned at the bottom rather than as "not applicable".
//   3. One request in flight per meter. Without this, a slow link accumulates
//      duplicate requests and the backlog never drains.
//   4. Yield to user commands. A frequency change that queues behind three
//      meter polls is a VFO knob that feels broken.
//
// Deliberately clock-injected rather than owning a QTimer: the whole policy is
// then testable in microseconds against a synthetic clock, which is the only
// practical way to prove rule 3 and rule 4.
class MeterPoller {
public:
    // A meter nobody is looking at is not polled at all.
    void setVisible(MeterId id, bool visible);
    [[nodiscard]] bool isVisible(MeterId id) const;

    void setTransmitting(bool tx) noexcept { m_transmitting = tx; }
    [[nodiscard]] bool isTransmitting() const noexcept { return m_transmitting; }

    // Which meters should be requested now. Marks each returned meter in
    // flight, so it will not be returned again until markAnswered() or the
    // in-flight timeout.
    [[nodiscard]] std::vector<MeterId> due(std::int64_t nowMs);

    // A reply arrived. Clears the in-flight mark and starts the next interval.
    void markAnswered(MeterId id, std::int64_t nowMs);

    // WHEN a reply last arrived, or 0 if one never has. The poller already knows
    // this — markAnswered is called on every reading — and it is the one fact
    // that separates a meter that works from one that is merely defined. A
    // defined-but-never-fed meter renders as a real instrument reading a quiet
    // band, which is worse than a missing one.
    [[nodiscard]] std::int64_t lastReadingAtMs(MeterId id) const;

    // A user-initiated command just went out. Metering stays quiet for a short
    // guard so the command is not stuck behind a queue of polls.
    void noteUserCommand(std::int64_t nowMs) noexcept { m_quietUntilMs = nowMs + kUserGuardMs; }

    void reset() noexcept;

    // Long enough that a burst of tuning stays responsive, short enough that
    // meters do not visibly freeze while the operator turns the VFO.
    static constexpr int kUserGuardMs = 80;
    // A lost reply must eventually be re-asked or the meter dies silently.
    static constexpr int kInFlightTimeoutMs = 1500;

private:
    struct State {
        bool visible = false;
        bool inFlight = false;
        std::int64_t nextDueMs = 0;
        std::int64_t sentAtMs = 0;
        std::int64_t answeredAtMs = 0;
    };
    [[nodiscard]] State& stateFor(MeterId id);
    [[nodiscard]] const State& stateFor(MeterId id) const;

    std::array<State, 8> m_state{};
    bool m_transmitting = false;
    std::int64_t m_quietUntilMs = 0;
};

}  // namespace AetherSDR::icom
