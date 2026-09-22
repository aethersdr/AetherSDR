#pragma once

#include <QJsonObject>

namespace AetherSDR {

// The audio arrangement an operator left behind at the end of their last split,
// so the next split can restore it without a setup step (#2242).
//
// Two kinds of field live here and they are NOT the same kind of state:
//
//   • the four audio values are LEARNED — never typed in, never defaulted.
//     Each carries a has* companion because "the operator never touched the TX
//     pan" and "the operator centred the TX pan" are different facts, and only
//     the second one should be replayed. A bare int cannot tell them apart, and
//     replaying a value nobody chose is the failure this feature exists to
//     avoid: it would move slice audio the operator never asked us to move.
//
//   • monitor is a CHOSEN preference. It survives "Forget remembered audio",
//     which clears only what was learned — see forgetLearnedState().
//
// The struct is deliberately free of AppSettings and of any Qt GUI type: the
// storage decision (one JSON key, Principle V) belongs to the caller, and this
// way the parsing rules below are unit-testable on their own.
struct SplitAudioProfile {
    // What a momentary Monitor TX hold does while the TX slice is muted.
    //   Solo — mute RX, unmute TX. The Icom XFC / Kenwood TF-SET / Yaesu TXW
    //          behaviour: one receiver, moved. Unambiguous about what you are
    //          hearing, and it gives you something the persistent arrangement
    //          below cannot.
    //   Both — unmute TX and leave RX audible: the sub-receiver convention.
    enum class Monitor { Solo, Both };

    bool hasTxMute{false};  bool txMuted{true};
    bool hasTxGain{false};  int  txGain{0};
    bool hasTxPan{false};   int  txPan{50};
    bool hasRxPan{false};   int  rxPan{50};
    Monitor monitor{Monitor::Solo};

    // True when at least one audio value was learned. False means "behave
    // exactly as AetherSDR did before this feature existed" — the caller mutes
    // the TX slice and sends nothing else.
    bool hasLearnedState() const
    {
        return hasTxMute || hasTxGain || hasTxPan || hasRxPan;
    }

    // Drop the learned values, keep the chosen preference. "Forget remembered
    // audio" must not silently reset the operator's monitor mode: they are
    // separate decisions reached in separate places, and only one of them is
    // something the app inferred on their behalf.
    void forgetLearnedState()
    {
        const Monitor keep = monitor;
        *this = SplitAudioProfile{};
        monitor = keep;
    }

    // An unrecognised version, a non-object, or a field of the wrong JSON type
    // all yield a default-constructed profile rather than a partial one. A
    // half-understood arrangement replayed onto live slice audio is worse than
    // no arrangement, and the operator re-teaches it in one split.
    static SplitAudioProfile fromJson(const QJsonObject& o);
    QJsonObject toJson() const;

    static constexpr int kVersion = 1;
    // AppSettings key. One object, not five scalars (Principle V).
    static constexpr const char* kSettingsKey = "SplitAudio";
};

// What the operator did to the two slices during ONE split.
//
// Split lives in MainWindow, but the decision of what counts as a preference —
// and what the RX pan has to be put back to — does not need a window, a radio,
// or a slice, so it is here where it can be tested directly. MainWindow owns
// one of these, forwards the *CommandIssued signals into the note* methods,
// and asks merge() for the profile to store.
//
// It also has to outlive the TX slice: when the radio removes it out of band,
// MainWindow's onSliceRemoved runs with the model object already destroyed, so
// there is nothing left to read the values off. This recorder IS the record.
class SplitAudioRecorder {
public:
    // Seed from the slices as they are the moment split begins. Pass the values
    // read through SliceModel's flexAudio*() accessors, never the plain ones:
    // while DAX/TCI own a slice's audio the plain accessors return that
    // subsystem's replacement values, and a seed taken from those would hand
    // another subsystem's routing back to the operator as their own preference.
    void arm(int rxPanBefore, bool txMuted, int txGain, int txPan);
    void disarm() { *this = SplitAudioRecorder{}; }
    bool armed() const { return m_armed; }

    // Operator-issued changes only. Every caller is a *CommandIssued signal,
    // which does not fire for radio status echoes — so a pan moved by another
    // client, a profile load, or the front panel never becomes "what the
    // operator likes" (Principle II).
    void noteTxMute(bool muted) { if (m_armed) { m_txMuteTouched = true; m_txMuted = muted; } }
    void noteTxGain(int gain)   { if (m_armed) { m_txGainTouched = true; m_txGain  = gain; } }
    void noteTxPan(int pan)     { if (m_armed) { m_txPanTouched  = true; m_txPan   = pan; } }
    void noteRxPan(int pan)     { if (m_armed) { m_rxPanTouched  = true; m_rxPan   = pan; } }

    // The profile to store: `existing` supplies the chosen monitor mode, and
    // every learned field is replaced wholesale. Replaced and not merged on
    // purpose — a value the operator has stopped setting should stop coming
    // back, or the arrangement silently accretes across splits and can never be
    // reduced except by forgetting all of it.
    SplitAudioProfile merge(const SplitAudioProfile& existing) const;

    // The pan to put the RX slice back to, or -1 for "leave it alone". The
    // operator panned it away for the split, not for everything afterwards —
    // but only if they actually moved it.
    int rxPanToRestore() const
    {
        return (m_rxPanTouched && m_rxPanBefore >= 0) ? m_rxPanBefore : -1;
    }

private:
    bool m_armed{false};
    int  m_rxPanBefore{-1};
    bool m_rxPanTouched{false}; int  m_rxPan{50};
    bool m_txMuteTouched{false}; bool m_txMuted{true};
    bool m_txGainTouched{false}; int  m_txGain{0};
    bool m_txPanTouched{false};  int  m_txPan{50};
};

}  // namespace AetherSDR
