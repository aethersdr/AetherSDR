#pragma once

// Two LNA numbers: the operator's baseline (Hl2Backend::m_lnaGainDb, also the
// per-band memory and persisted `rfGain`) and what reaches the wire. Automatic
// action is a separate non-negative attenuation below the baseline, so it never
// leaks into persisted state and never makes the radio louder than asked. The
// applied offset is returned too: `baseline - offset` can hit the register
// floor, and a controller must see it ran out of range.

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
