#pragma once

// WHAT THE RECEIVE FRONT END IS DOING, for an operator to look at.
//
// RFC #5535 approved an automatic RF-gain loop on the condition that it is
// VISIBLE. The reasoning in that ruling is the whole justification for this
// header, so it is worth repeating where the code is: the HL2's usable LNA range
// is about 18 dB, the day-to-night signal swing is of the same order, and the
// transition from clean to clipped is only 3-5 dB wide. A regulator working in
// that little room will sometimes be wrong. Wrong AND INVISIBLE is a radio that
// behaves strangely; wrong and visible is an event the operator can see and act
// on.
//
// TWO THINGS HAVE TO BE VISIBLE, NOT ONE. The clipping is the obvious half. The
// other is the REGULATOR'S OWN ACTION -- how far it has pulled the gain below
// what the operator set -- because without it "my noise floor moved and I
// touched nothing" comes back through the side door, which is the complaint
// #5625 exists to prevent.
//
// ALREADY CLASSIFIED WHEN IT GETS HERE. This struct carries a LEVEL, not a rate
// and not a threshold. Deciding when a converter is clipping "occasionally"
// rather than "most of the time" depends on what the flag means on a particular
// front end -- on the HL2 it is a three-event threshold sampled at 10 Hz, not a
// rate -- and that judgement belongs in the family backend beside the register
// it reads. Hl2AutoGainPolicy.h already makes it; this seam carries the answer
// rather than the evidence, for the same reason IAutoRfGainControl carries the
// switch rather than the levels.
//
// NO FAMILY NAME APPEARS HERE, and no wire concept. A family that cannot observe
// its converter never emits this and its indicator never appears.

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
