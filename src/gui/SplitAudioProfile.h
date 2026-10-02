#pragma once

#include <QJsonObject>
#include <QPointer>

namespace AetherSDR {

// Audio arrangement from the operator's last split, restored on the next (#2242).
// The four audio values are learned; each has a has* flag because "never
// touched" must not be replayed as "set to centre". monitor is a chosen
// preference and survives "Forget remembered audio" (forgetLearnedState()).
// Free of AppSettings (the caller stores it as one JSON key) so parsing is testable.
struct SplitAudioProfile {
    // What a momentary Monitor TX hold does.
    //   Solo — mute RX, unmute TX. The Icom XFC / Kenwood TF-SET / Yaesu TXW
    //          behaviour: one receiver, moved. Unambiguous about what you are
    //          hearing, and it gives you something the persistent arrangement
    //          below cannot.
    //   Both — unmute TX and RX for the hold: the sub-receiver convention.
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

// Marks an audio write as the operator's own while in scope. SliceModel emits
// audiotyped operator audio intents for every setter caller (TCI, SmartCAT, rigctld, Mute All,
// RADE, memory recall...); only operator edits are preferences, so the recorder
// notes changes only while a scope is live. GUI thread only: remote paths reach
// the setters via queued invocations, outside any scope.
class SplitAudioOperatorEdit {
public:
    SplitAudioOperatorEdit() { ++s_depth; }
    ~SplitAudioOperatorEdit() { --s_depth; }
    SplitAudioOperatorEdit(const SplitAudioOperatorEdit&) = delete;
    SplitAudioOperatorEdit& operator=(const SplitAudioOperatorEdit&) = delete;
    static bool active() { return s_depth > 0; }

private:
    static inline int s_depth = 0;
};

// What the operator did to the two slices during one split. MainWindow forwards
// typed operator audio intents into the note* methods and stores merge()'s result. It must
// outlive the TX slice: when the radio removes it, onSliceRemoved runs after the
// model object is destroyed, so this recorder is the only record.
class SplitAudioRecorder {
public:
    // rxPanBefore is the RX slice's pan from BEFORE the remembered arrangement
    // was applied (-1 when there is no RX slice); rxPanMovedByApply says the
    // apply itself moved it. Both are needed because the apply is deliberately
    // not learned: without them, a split whose RX pan was moved only by the
    // replay would never be put back.
    void arm(int rxPanBefore, bool rxPanMovedByApply);
    void disarm() { *this = SplitAudioRecorder{}; }
    bool armed() const { return m_armed; }

    // "Forget remembered audio" pressed mid-split: drop what this split has
    // learned so far, or merge() would write it straight back at split end.
    // The RX-pan restore is kept — the split still moved it.
    void forgetTouched()
    {
        m_txMuteTouched = m_txGainTouched = m_txPanTouched = m_rxPanTouched = false;
    }
    // This split has operator edits that merge() will store when it ends.
    bool hasPendingLearning() const
    {
        return m_armed && (m_txMuteTouched || m_txGainTouched
                           || m_txPanTouched || m_rxPanTouched);
    }

    // Operator-issued changes only. Every caller is an operator-origin intent,
    // which does not fire for radio status echoes — so a pan moved by another
    // client on the radio, or the front panel, never arrives here. And a note
    // counts only inside a SplitAudioOperatorEdit, which drops the writes this
    // app makes on someone else's behalf: TCI, CAT, rigctld, Mute All, RADE,
    // memory recall (Principle II).
    void noteTxMute(bool muted) { if (noting()) { m_txMuteTouched = true; m_txMuted = muted; } }
    void noteTxGain(int gain)   { if (noting()) { m_txGainTouched = true; m_txGain  = gain; } }
    void noteTxPan(int pan)     { if (noting()) { m_txPanTouched  = true; m_txPan   = pan; } }
    void noteRxPan(int pan)
    {
        if (!noting()) return;
        m_rxPanTouched = true;
        m_rxPanMovedByOperator = true;   // survives forgetTouched(): restore duty
        m_rxPan = pan;
    }

    // Keep this GUI-owned recorder independent of the engine request header,
    // as with the slice-generic apply/monitor helpers below. Production and
    // tests use the same typed-intent translation, not copied signal wiring.
    template<class AudioRequest>
    void noteTxAudioIntent(const AudioRequest& request)
    {
        if (!request.valid() || request.origin != AudioRequest::Origin::Operator) {
            return;
        }
        switch (request.field) {
        case AudioRequest::Field::Gain: noteTxGain(request.value); break;
        case AudioRequest::Field::Mute: noteTxMute(request.value != 0); break;
        case AudioRequest::Field::Pan: noteTxPan(request.value); break;
        }
    }

    template<class AudioRequest>
    void noteRxAudioIntent(const AudioRequest& request)
    {
        if (request.valid() && request.origin == AudioRequest::Origin::Operator
            && request.field == AudioRequest::Field::Pan) {
            noteRxPan(request.value);
        }
    }

    // The profile to store. Learned fields CARRY FORWARD: `existing` was
    // replayed onto this split, so a field the operator left alone is still
    // what they are listening to, and only a field they touched is replaced.
    // One exception: ending the split with the TX slice muted clears the whole
    // arrangement — muting it once is how an operator returns to the pre-#2242
    // behaviour. The chosen monitor mode is never learned and always kept.
    SplitAudioProfile merge(const SplitAudioProfile& existing) const;

    // The pan to put the RX slice back to, or -1 for "leave it alone". The RX
    // pan is restored whenever this split moved it — by the operator or by the
    // replay — because the arrangement is for the split, not for everything
    // afterwards.
    int rxPanToRestore() const
    {
        return ((m_rxPanMovedByOperator || m_rxPanMovedByApply) && m_rxPanBefore >= 0)
            ? m_rxPanBefore : -1;
    }

private:
    bool noting() const { return m_armed && SplitAudioOperatorEdit::active(); }

    bool m_armed{false};
    int  m_rxPanBefore{-1};
    bool m_rxPanMovedByApply{false};
    // Restore duty, kept apart from the learning flag below: forgetting what
    // the split taught must not also forget that the RX pan was moved.
    bool m_rxPanMovedByOperator{false};
    bool m_rxPanTouched{false};  int  m_rxPan{50};
    bool m_txMuteTouched{false}; bool m_txMuted{true};
    bool m_txGainTouched{false}; int  m_txGain{0};
    bool m_txPanTouched{false};  int  m_txPan{50};
};

// What applySplitAudioProfile() found and did, for SplitAudioRecorder::arm().
struct SplitAudioApplyResult {
    int  rxPanBefore{-1};       // RX native pan before anything was applied
    bool rxPanMoved{false};     // the apply changed the RX pan
    bool restored{false};       // a learned arrangement was applied
};

// Apply a remembered arrangement to a new split. With nothing learned, or a TX
// slice whose RX audio is replaced (DAX/TCI/KiwiSDR), it only mutes the TX
// slice. These are app writes: keep them out of the recorder. Templated over
// the slice type so this header needs no models/ include.
template <class Slice>
SplitAudioApplyResult applySplitAudioProfile(const SplitAudioProfile& profile,
                                             Slice* rx, Slice* tx)
{
    SplitAudioApplyResult r;
    if (!tx) return r;
    // Captured BEFORE anything below can move it: this is what the RX pan goes
    // back to when the split ends. flexAudioPan(), not audioPan(), because
    // while DAX/TCI/Kiwi own the slice's audio the plain accessor returns the
    // replacement value.
    if (rx) r.rxPanBefore = rx->flexAudioPan();

    // A slice whose receive audio another subsystem has replaced is not ours to
    // arrange: setAudioPan()/setAudioGain() there write only the replacement
    // state and emit no *CommandIssued, so an apply would be half-applied and
    // never re-learned. Fall back to the historic mute.
    if (!profile.hasLearnedState() || tx->externalReceiveReplacementActive()) {
        tx->setAudioMute(true);   // pre-#2242 behaviour, byte for byte
        return r;
    }

    // Gain and pan before mute, so unmuting never lands as a blast at the
    // previous slice's level or in the wrong ear.
    if (profile.hasTxGain) tx->setAudioGain(static_cast<float>(profile.txGain));
    if (profile.hasTxPan)  tx->setAudioPan(profile.txPan);
    tx->setAudioMute(profile.hasTxMute ? profile.txMuted : true);

    if (rx && profile.hasRxPan && !rx->externalReceiveReplacementActive()
        && profile.rxPan != r.rxPanBefore) {
        rx->setAudioPan(profile.rxPan);
        r.rxPanMoved = true;
    }
    r.restored = true;
    return r;
}

// Momentary Monitor TX hold. Release restores only the native mutes it changed:
//   - only on the same slice objects (a new slice reusing the id is never written);
//   - never on a slice whose RX audio is replaced (DAX/TCI/Kiwi), before or
//     during the hold, since setAudioMute() there writes the replacement mute;
//   - a mute already at the wanted value is not recorded as changed.
template <class Slice>
class SplitMonitorHold {
public:
    // False (and nothing written) when there is no pair or the TX slice's
    // audio is replaced.
    bool begin(Slice* rx, Slice* tx, SplitAudioProfile::Monitor mode)
    {
        if (m_active || !rx || !tx) return false;
        if (tx->externalReceiveReplacementActive()) return false;

        *this = SplitMonitorHold{};
        m_active = true;
        m_rxId   = rx->sliceId();
        m_txId   = tx->sliceId();
        m_rx     = rx;
        m_tx     = tx;

        if (tx->flexAudioMute()) {
            tx->setAudioMute(false);
            m_txUnmuted = true;
        }
        // Solo mutes RX; Both makes sure it is audible. A replaced RX is
        // skipped outright — its native mute belongs to the replacement.
        if (!rx->externalReceiveReplacementActive()) {
            const bool wantMuted = (mode == SplitAudioProfile::Monitor::Solo);
            if (rx->flexAudioMute() != wantMuted) {
                m_rxMuteBefore = rx->flexAudioMute();
                m_rxChanged    = true;
                rx->setAudioMute(wantMuted);
            }
        }
        return true;
    }

    // Pass the CURRENT models for rxId()/txId(); either may be null (removed).
    void end(Slice* rx, Slice* tx)
    {
        if (!m_active) return;
        if (m_txUnmuted && tx && tx == m_tx.data()
            && !tx->externalReceiveReplacementActive())
            tx->setAudioMute(true);
        if (m_rxChanged && rx && rx == m_rx.data()
            && !rx->externalReceiveReplacementActive())
            rx->setAudioMute(m_rxMuteBefore);
        *this = SplitMonitorHold{};
    }

    bool active() const { return m_active; }
    int  rxId() const { return m_rxId; }
    int  txId() const { return m_txId; }
    // The held objects (null once destroyed). A caller that finds one alive
    // but absent from the live slice map knows it is parked for a reconnect.
    Slice* rxObject() const { return m_rx.data(); }
    Slice* txObject() const { return m_tx.data(); }

private:
    bool m_active{false};
    int  m_rxId{-1};
    int  m_txId{-1};
    QPointer<Slice> m_rx;       // identity, not just the id (see above)
    QPointer<Slice> m_tx;
    bool m_txUnmuted{false};
    bool m_rxChanged{false};
    bool m_rxMuteBefore{false};
};

}  // namespace AetherSDR
