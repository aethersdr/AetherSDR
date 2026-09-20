#pragma once

// A BACKEND'S OWN AUTOMATIC RECEIVE-GAIN CONTROL, as a seam vocabulary.
//
// Implementations live under `src/core/backends/<family>/`. Nothing here knows
// how one talks to a radio, and nothing here is allowed to: this interface
// carries no wire concept at all, because the moment it carries one it has
// started to describe a particular front end.
//
// NO FAMILY NAME APPEARS ABOVE THE SEAM. A family that has such a control
// returns one from `IRadioBackend::autoRfGainControl()`; a family that does not
// returns nullptr and never learns the concept exists. Shared code asks the
// backend for the pointer and never names a family — the shape
// `OfflineHealthRegistry` established for a different capability and the same
// reason (docs/HERMES.md, "keep bring-up inside the family backend").
//
// WHY THIS AND NOT A CAPABILITY BOOL. Two reasons, and the first is now
// mechanical:
//
//   * `RadioCapabilities`' boolean population is FROZEN and shrink-only
//     (#5262 M2, tools/check_capability_records.py, enforced by the required
//     Static checks job). A new loose bool cannot land.
//   * The rule behind that freeze applies here on its merits. A
//     `hasAutoRfGain` bool would have fissioned immediately: the first GUI to
//     draw the control needed the floor bound and the set of laws as well,
//     and with a bool both had to be fetched from somewhere else — which in
//     practice meant reading an untyped health row by string key. An
//     interface carries the shape with the capability.
//
// BORROWED, NEVER OWNED, NEVER CACHED. The pointer belongs to the backend and
// is valid only for the duration of the call that obtained it. A disconnect
// destroys the backend, and a stored pointer would outlive it.
//
// DISPLAY-AND-COMMAND, NOT A DATA FEED. Nothing here reports levels. What the
// control is observing, and how well, is the backend's business and reaches an
// operator through the health snapshot; this interface is the switch, the
// bound, and the choice of law.

#include <QString>
#include <QStringList>

namespace AetherSDR {

class IAutoRfGainControl {
public:
    virtual ~IAutoRfGainControl() = default;

    // Arm or disarm. RADIO-WIDE rather than per-pan, like setPanRfGain's target
    // on a single-converter radio: there is one front end.
    //
    // DISARMING MUST RESTORE THE OPERATOR'S OWN GAIN TO THE HARDWARE IN ONE
    // ACTION, from whatever state the control was in. A control that left the
    // radio attenuated after its switch was turned off would be one that does
    // not undo itself.
    //
    // A backend may DECLINE to arm — the HL2 refuses above a baseline where its
    // gain axis is not trustworthy — so a caller must read `isArmed()` back
    // rather than assuming the request took.
    virtual void setArmed(bool on) = 0;
    [[nodiscard]] virtual bool isArmed() const = 0;

    // How far below the operator's own gain the control may go, in dB. The
    // second of exactly two numbers the operator owns; the first is the switch.
    // Everything else about such a loop is a decision they have no evidence to
    // make.
    virtual void setFloorDb(int floorDb) = 0;
    [[nodiscard]] virtual int floorDb() const = 0;
    // The backend's bound on that number, because how deaf a receiver may be
    // made is a property of its gain axis and not of the operator's taste.
    [[nodiscard]] virtual int maxFloorDb() const = 0;

    // Which control law is running, and which this backend has.
    //
    // STRINGS RATHER THAN AN ENUM ON PURPOSE: the set of laws is a backend's
    // private business and this seam carries no opinion about it. The HL2's
    // release condition is a genuinely open question (#5535) and the bench has
    // to be able to argue with the answer without a rebuild.
    //
    // `setLaw` returns false and changes nothing when the name is not one this
    // backend has. Callers report that rather than guessing.
    virtual bool setLaw(const QString& name) = 0;
    [[nodiscard]] virtual QString law() const = 0;
    [[nodiscard]] virtual QStringList laws() const = 0;
};

}  // namespace AetherSDR
