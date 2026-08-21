#include "core/backends/icom/IcomMeters.h"

#include <algorithm>
#include <array>

namespace AetherSDR::icom {
namespace {

// ---- The published curves -------------------------------------------------
//
// PROVENANCE MATTERS HERE, so each table says where it came from. Icom's CI-V
// Reference Guide gives breakpoints for most meters directly; where it gives
// only percentages, the measured tables in Hamlib/flrig fill the gap. Both are
// facts about the hardware, not anyone's creative work.

// IC-705 Po meter, raw -> WATTS. Icom's guide gives only 0/50/100 %; these are
// the 13 measured points from Hamlib's IC705_RFPOWER_METER_CAL (sourced in turn
// from flrig).
//
// Note the curve tops out at 12 W, ABOVE the radio's rated 10 W — the meter has
// headroom past 100 %, and clamping at 10 would misreport a hot final.
constexpr std::array<CurvePoint, 13> kPowerIc705{{
    {0, 0.0},   {21, 0.5},  {43, 1.0},  {65, 1.5},  {83, 2.0},
    {95, 2.5},  {105, 3.0}, {114, 3.5}, {124, 4.0}, {143, 5.0},
    {183, 7.5}, {213, 10.0}, {255, 12.0},
}};

// IC-7300MK2 Po meter, raw -> watts. Its official guide gives 0/50/100
// percent at 0/143/213 and the model is rated at 100 W. Keep the documented
// headroom above the 100 percent point rather than pinning a hot final.
constexpr std::array<CurvePoint, 4> kPowerIc7300Mk2{{
    {0, 0.0}, {143, 50.0}, {213, 100.0}, {255, 130.0},
}};

// SWR. Icom's guide: 0 = 1.0, 48 = 1.5, 80 = 2.0, 120 = 3.0.
//
// The guide stops at 3.0 and the field is a full byte. The final point is an
// EXTRAPOLATION at the same slope, present so a genuinely bad match reads as
// "very high" rather than clamping at 3.0 — a mismatch pinned at exactly 3.0
// looks like a working antenna to anyone glancing at the meter.
constexpr std::array<CurvePoint, 5> kSwr{{
    {0, 1.0}, {48, 1.5}, {80, 2.0}, {120, 3.0}, {255, 6.4},
}};

// COMP meter, raw -> dB. Icom's guide: 0 = 0 dB, 130 = 15 dB, 210 = 25.5 dB.
constexpr std::array<CurvePoint, 3> kComp{{
    {0, 0.0}, {130, 15.0}, {210, 25.5},
}};

// Vd (PA drain), raw -> volts. Icom's guide: 0 = 0 V, 75 = 5 V, 241 = 16 V.
constexpr std::array<CurvePoint, 3> kVd{{
    {0, 0.0}, {75, 5.0}, {241, 16.0},
}};

// Id (PA current), raw -> amps. Icom's guide: 0 = 0 A, 121 = 2 A, 241 = 4 A.
constexpr std::array<CurvePoint, 3> kId{{
    {0, 0.0}, {121, 2.0}, {241, 4.0},
}};

// IC-7300MK2 desktop-radio calibration from its own CI-V guide.
constexpr std::array<CurvePoint, 3> kVdIc7300Mk2{{
    {0, 0.0}, {13, 10.0}, {241, 16.0},
}};
constexpr std::array<CurvePoint, 4> kIdIc7300Mk2{{
    {0, 0.0}, {97, 10.0}, {146, 15.0}, {241, 25.0},
}};

// ALC, raw -> percent. Icom's guide gives only "0 = Minimum, 120 = Maximum",
// so full scale is 120 and NOT 255. Scaling by 255 would make a fully-driven
// ALC read 47 %.
constexpr std::array<CurvePoint, 2> kAlc{{
    {0, 0.0}, {120, 100.0},
}};

// The meter set, in the order a UI would naturally show them.
//
// The intervals are the poll BUDGET, and they are deliberately modest: every
// one of these costs a round trip on the same stream that carries tuning.
constexpr std::array<MeterSpec, 8> kSpecs{{
    {MeterId::SMeter,   meter::kSMeter,   "SLC", "LEVEL",   "dBm",  -140.0,  -10.0,
     MeterWhen::RxOnly, 100},
    {MeterId::Power,    meter::kPower,    "TX",  "FWDPWR",  "Watts",   0.0,   12.0,
     MeterWhen::TxOnly, 200},
    {MeterId::Swr,      meter::kSwr,      "TX",  "SWR",     "SWR",     1.0,    6.4,
     MeterWhen::TxOnly, 200},
    {MeterId::Alc,      meter::kAlc,      "TX",  "ALC",     "Percent", 0.0,  100.0,
     MeterWhen::TxOnly, 200},
    {MeterId::Comp,     meter::kComp,     "TX",  "COMPPEAK","dB",      0.0,   25.5,
     MeterWhen::TxOnly, 200},
    {MeterId::Vd,       meter::kVd,       "RAD", "+13.8A",  "Volts",   0.0,   16.0,
     MeterWhen::Always, 1000},
    {MeterId::Id,       meter::kId,       "RAD", "PACURRENT","Amps",   0.0,    4.0,
     MeterWhen::TxOnly, 500},
    // OVF is a boolean the radio reports through the meter command. It belongs
    // in the health snapshot rather than on a meter face, but it is polled the
    // same way, so it lives here.
    {MeterId::Overflow, meter::kOverflow, "RAD", "OVF",     "Percent", 0.0,    1.0,
     MeterWhen::RxOnly, 500},
}};

std::size_t indexOf(MeterId id) noexcept { return static_cast<std::size_t>(id); }

}  // namespace

// ---------------------------------------------------------------------------
// Calibration
// ---------------------------------------------------------------------------

double interpolateCurve(std::span<const CurvePoint> curve, int raw)
{
    if (curve.empty())
        return 0.0;
    if (raw <= curve.front().raw)
        return curve.front().value;
    if (raw >= curve.back().raw)
        return curve.back().value;

    for (std::size_t i = 1; i < curve.size(); ++i) {
        if (raw <= curve[i].raw) {
            const auto& a = curve[i - 1];
            const auto& b = curve[i];
            const int span = b.raw - a.raw;
            if (span <= 0)
                return b.value;
            const double t = static_cast<double>(raw - a.raw) / span;
            return a.value + t * (b.value - a.value);
        }
    }
    return curve.back().value;
}

std::span<const CurvePoint> powerCurveIc705() { return kPowerIc705; }
std::span<const CurvePoint> powerCurveIc7300Mk2() { return kPowerIc7300Mk2; }
std::span<const CurvePoint> swrCurve()        { return kSwr; }
std::span<const CurvePoint> compCurve()       { return kComp; }
std::span<const CurvePoint> vdCurve()         { return kVd; }
std::span<const CurvePoint> idCurve()         { return kId; }
std::span<const CurvePoint> alcCurve()        { return kAlc; }

double sMeterDbm(int raw, double s9Dbm)
{
    // Icom's published breakpoints: 0 = S0, 120 = S9, 241 = S9 + 60 dB.
    // An S-unit is 6 dB, so S0 sits 54 dB below S9.
    //
    // Built as a curve rather than one line because the two segments have
    // genuinely different slopes: 54 dB over 120 counts below S9, and 60 dB
    // over 121 counts above it.
    //
    // The error from collapsing them to one line peaks at 2.76 dB — about half
    // an S-unit — and it peaks AT S9, which is the reading operators actually
    // quote. It is zero at both endpoints, so a test that only checks S0 and
    // S9+60 would pass against the wrong implementation.
    const std::array<CurvePoint, 3> curve{{
        {0, s9Dbm - 54.0},
        {120, s9Dbm},
        {241, s9Dbm + 60.0},
    }};
    return interpolateCurve(curve, raw);
}

std::span<const MeterSpec> meterSpecs() { return kSpecs; }

const MeterSpec* meterSpecFor(MeterId id)
{
    for (const auto& s : kSpecs)
        if (s.id == id)
            return &s;
    return nullptr;
}

const MeterSpec* meterSpecForSub(std::uint8_t sub)
{
    for (const auto& s : kSpecs)
        if (s.sub == sub)
            return &s;
    return nullptr;
}

double meterValue(MeterId id, int raw, double s9Dbm, std::uint8_t civAddress)
{
    switch (id) {
    case MeterId::SMeter:   return sMeterDbm(raw, s9Dbm);
    case MeterId::Power:    return interpolateCurve(
                                civAddress == 0xB6 ? powerCurveIc7300Mk2()
                                                   : powerCurveIc705(), raw);
    case MeterId::Swr:      return interpolateCurve(kSwr, raw);
    case MeterId::Alc:      return interpolateCurve(kAlc, raw);
    case MeterId::Comp:     return interpolateCurve(kComp, raw);
    case MeterId::Vd:       return interpolateCurve(
                                civAddress == 0xB6
                                    ? std::span<const CurvePoint>(kVdIc7300Mk2)
                                    : std::span<const CurvePoint>(kVd), raw);
    case MeterId::Id:       return interpolateCurve(
                                civAddress == 0xB6
                                    ? std::span<const CurvePoint>(kIdIc7300Mk2)
                                    : std::span<const CurvePoint>(kId), raw);
    // OVF is 00/01 from the radio, not a scaled reading.
    case MeterId::Overflow: return raw != 0 ? 1.0 : 0.0;
    }
    return 0.0;
}

// ---------------------------------------------------------------------------
// The poll scheduler
// ---------------------------------------------------------------------------

MeterPoller::State& MeterPoller::stateFor(MeterId id) { return m_state[indexOf(id)]; }
const MeterPoller::State& MeterPoller::stateFor(MeterId id) const { return m_state[indexOf(id)]; }

void MeterPoller::setVisible(MeterId id, bool visible)
{
    State& s = stateFor(id);
    if (s.visible == visible)
        return;
    s.visible = visible;
    // Becoming visible must not wait out a stale interval — the operator opened
    // the panel and expects a reading, not a second of blank.
    s.nextDueMs = 0;
    s.inFlight = false;
}

bool MeterPoller::isVisible(MeterId id) const { return stateFor(id).visible; }

std::vector<MeterId> MeterPoller::due(std::int64_t nowMs)
{
    std::vector<MeterId> out;

    // Rule 4: yield to user commands. A frequency change that queues behind
    // three meter polls is a VFO knob that feels broken.
    if (nowMs < m_quietUntilMs)
        return out;

    for (const auto& spec : kSpecs) {
        State& s = stateFor(spec.id);

        if (!s.visible)                                   // rule 1
            continue;
        if (spec.when == MeterWhen::TxOnly && !m_transmitting)   // rule 2
            continue;
        if (spec.when == MeterWhen::RxOnly && m_transmitting)
            continue;

        if (s.inFlight) {                                 // rule 3
            if (nowMs - s.sentAtMs < kInFlightTimeoutMs)
                continue;
            // The reply never came. Re-asking is right — the alternative is a
            // meter that dies silently for the rest of the session.
        }
        if (nowMs < s.nextDueMs)
            continue;

        s.inFlight = true;
        s.sentAtMs = nowMs;
        out.push_back(spec.id);
    }
    return out;
}

void MeterPoller::markAnswered(MeterId id, std::int64_t nowMs)
{
    const MeterSpec* spec = meterSpecFor(id);
    State& s = stateFor(id);
    s.inFlight = false;
    // The interval runs from the ANSWER, not from the request. Pacing off the
    // request would let a slow link stack polls back-to-back the moment each
    // reply lands, which is the contention this scheduler exists to avoid.
    s.nextDueMs = nowMs + (spec ? spec->intervalMs : 200);
    stateFor(id).answeredAtMs = nowMs;
}

std::int64_t MeterPoller::lastReadingAtMs(MeterId id) const
{
    return stateFor(id).answeredAtMs;
}

void MeterPoller::reset() noexcept
{
    for (auto& s : m_state)
        s = State{};
    m_transmitting = false;
    m_quietUntilMs = 0;
}

}  // namespace AetherSDR::icom
