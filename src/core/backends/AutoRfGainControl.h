#pragma once

// A backend's own automatic receive-gain control, as a seam vocabulary with no
// wire concept and no family name. A family that has one returns it from
// IRadioBackend::autoRfGainControl(); others return nullptr (docs/HERMES.md,
// "keep bring-up inside the family backend").
// An interface rather than a capability bool: RadioCapabilities' bool population
// is frozen (#5262 M2, tools/check_capability_records.py), and the control needs
// its floor bound and laws alongside the switch.
// BORROWED, NEVER OWNED, NEVER CACHED: valid only for the duration of the call
// that obtained it; a disconnect destroys the backend. Switch, bound and law
// only; observed levels reach the operator through the health snapshot.

#include <QString>
#include <QStringList>

namespace AetherSDR {

class IAutoRfGainControl {
public:
    virtual ~IAutoRfGainControl() = default;

    // Arm or disarm, RADIO-WIDE (there is one front end). Disarming restores the
    // operator's own gain to the hardware in one action, from any state. A backend
    // may decline to arm (the HL2 refuses above a baseline where its gain axis is
    // untrustworthy), so callers read isArmed() back. The backend emits
    // IRadioBackend::autoRfGainArmSettled after EVERY outcome, refusal included,
    // so a view learns of arms settled elsewhere (connect-time restore, bridge).
    virtual void setArmed(bool on) = 0;
    [[nodiscard]] virtual bool isArmed() const = 0;

    // Why the last arm request was refused, in a sentence for the operator. Empty
    // after success or when the backend gives no reason. Shown only after an
    // isArmed() readback already showed the refusal; not a status line. (#5817)
    [[nodiscard]] virtual QString lastArmRefusalReason() const { return {}; }

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
