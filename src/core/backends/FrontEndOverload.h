#pragma once

// What the receive front end is doing, for the operator to see. RFC #5535 allows
// an automatic RF-gain loop only if it is VISIBLE: the HL2's usable LNA range is
// ~18 dB and clean-to-clipped is 3-5 dB wide, so the loop will sometimes be wrong.
// Two things are shown: clipping, and how far the regulator holds gain below the
// operator's setting (#5625). Carries an already-classified LEVEL; the family
// backend owns the classification (Hl2AutoGainPolicy.h). No family name or wire
// concept; a family that cannot observe its converter never emits this.

#include <QString>

namespace AetherSDR {

enum class FrontEndLevel {
    // This radio cannot see its converter, or has not looked yet. NOT the same
    // as Clean: an indicator must be able to say "no reading" rather than
    // showing a reassuring green for a measurement nobody took.
    Unobserved,
    Clean,
    Marginal,   // the converter railed in some observations
    Hot,        // it railed in more of them than not
    // Still clipping with the loop as deep as it is allowed to go. The one
    // state software cannot fix: it wants attenuation or a filter ahead of the
    // radio, and saying so is the only useful thing left to do.
    AtFloor
};

struct FrontEndOverload {
    FrontEndLevel level = FrontEndLevel::Unobserved;

    // Whether an automatic loop is running at all. An offset of 0 means
    // something different when it is armed (the loop is holding) than when it
    // is not (there is no loop), and the indicator has to say which.
    bool autoArmed = false;

    // How far BELOW the operator's own setting the loop currently holds the
    // gain, in dB, as a positive number. This is the regulator's action, and it
    // is the half that must not be silent.
    int autoOffsetDb = 0;

    // The backend's own words for what it is doing. Carried as text rather than
    // reconstructed above the seam because the set of reasons is a property of
    // the control law, and #5535 left the HL2's release condition explicitly
    // open to argument from the bench.
    QString reason;

    [[nodiscard]] bool operator==(const FrontEndOverload& o) const
    {
        return level == o.level && autoArmed == o.autoArmed
            && autoOffsetDb == o.autoOffsetDb && reason == o.reason;
    }
    [[nodiscard]] bool operator!=(const FrontEndOverload& o) const { return !(*this == o); }
};

}  // namespace AetherSDR
