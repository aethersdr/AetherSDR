#pragma once

// TWO LNA NUMBERS, NOT ONE: the operator's baseline, and what reaches the wire.
//
// WHY THIS EXISTS
//
// `Hl2Backend::m_lnaGainDb` is not only the register value. It is also:
//
//   * what `Hl2Backend::rememberCurrentBandState` writes into `m_lnaDbByBand`
//     through `bandMemoryWriteback` on every band change;
//   * what `Hl2Backend::currentOperatingState` publishes as the `rfGain`
//     extension object, i.e. what `RadioStateMemory` puts on disk — the backend
//     declares `RadioCapabilities::ClientSettingsDomain::RfGain` and
//     `restoreLegacyRfGain` refuses to replay `DisplayRfGain_hl2` for this
//     family precisely because the backend owns the value;
//   * what `Hl2Backend::setPanRfGain` compares against to decide `moved`.
//
// So ANY automatic writer on that path — an ADC-overload servo being the
// obvious one — writes its own transients into the operator's per-band memory
// and into persisted state, where the next band change makes them permanent.
// That is not hypothetical: it is the shape of an already-open defect in this
// lab's acceptance table (`D-lna-overwrite`), where 40 m went -6 -> -12 dB on
// disk with no operator action beyond one band change.
//
// The split is therefore a PREREQUISITE and it is also independently correct:
// with no automatic writer at all, separating "the number the operator chose"
// from "the number currently on the AD9866" costs nothing and removes a class
// of silent data loss before anything can trigger it.
//
// THE AXIS IS AN ATTENUATION, NOT A GAIN
//
// The automatic variable is a NON-NEGATIVE offset in dB BELOW the baseline. It
// has no representation for a gain above the operator's own setting, so:
//
//   * an automatic action can never make the radio louder than the operator
//     asked for — the only automatic action in that direction is undoing one of
//     its own;
//   * there is no "automatic ceiling" to design, document or explain, because
//     the ceiling IS the operator's number;
//   * the AD9866 register region above +19 dB, where this lab measured +48 dB
//     reading identically to +18 dB, is unreachable unless the operator is
//     already in it. Nothing here caps or moves their number to achieve that;
//     the axis simply cannot express it.
//
// WHY THIS RETURNS THE APPLIED OFFSET AS WELL
//
// `baseline - offset` can fall below the register's floor, and then the offset
// that was REQUESTED is not the offset that was APPLIED. A controller that
// assumed otherwise would keep attacking against a clamp, believing it had
// taken gain it never took, and would then have to release through phantom
// decibels before anything moved. So the clamp reports what it actually did and
// the caller can see it has run out of range — which is a real operator-facing
// condition ("the front end needs attenuation ahead of the radio"), not an
// internal detail.
//
// No Qt, no clock, no radio: this is arithmetic, and `Hl2Backend` evaluates it
// rather than keeping a copy, for the reason `Hl2TxLevelPolicy.h` states — a
// test against a re-typed copy of a mapping proves only that two copies agree.

namespace AetherSDR::hl2 {

struct EffectiveLnaGain {
    // What is written to the AD9866, what `Hl2DbReference` is told about, and
    // what every pan is echoed. Always within [minDb, maxDb].
    int effectiveDb = 0;
    // `baselineDb - effectiveDb`. Equal to the requested offset unless the
    // floor clamped, in which case it is smaller — never larger.
    int appliedOffsetDb = 0;
    // TRUE when the requested offset could not be applied in full because the
    // register floor was reached. The controller is out of range here; further
    // attack steps buy nothing.
    bool floorReached = false;
};

// `requestedOffsetDb` is clamped to non-negative first: the split's whole
// guarantee is that the automatic axis is one-directional, and a negative
// offset would be an automatic gain INCREASE above the operator's baseline
// wearing the wrong sign. A caller that wants more gain must move the baseline.
constexpr EffectiveLnaGain effectiveLnaGain(int baselineDb,
                                            int requestedOffsetDb,
                                            int minDb,
                                            int maxDb) noexcept
{
    EffectiveLnaGain out;
    const int offset = requestedOffsetDb < 0 ? 0 : requestedOffsetDb;
    // The baseline itself is clamped into range before the offset is taken, so
    // a baseline outside the register's range cannot manufacture headroom the
    // radio does not have.
    const int base = baselineDb < minDb ? minDb : (baselineDb > maxDb ? maxDb : baselineDb);
    int effective = base - offset;
    if (effective < minDb) {
        effective = minDb;
        out.floorReached = true;
    }
    // Unreachable while `offset >= 0` and `base <= maxDb`, and kept anyway:
    // this function is the one place the invariant "never above the operator's
    // baseline, never outside the register" is asserted, and an invariant
    // enforced only by its callers' good behaviour is not an invariant.
    if (effective > maxDb) {
        effective = maxDb;
    }
    out.effectiveDb = effective;
    out.appliedOffsetDb = base - effective;
    return out;
}

}  // namespace AetherSDR::hl2
