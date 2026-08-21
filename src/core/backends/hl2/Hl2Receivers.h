#pragma once

#include <QString>

#include <optional>
#include <vector>

namespace AetherSDR::hl2 {

// One running receiver's identity across the four index spaces it appears in,
// and the lookup table that owns them.
//
// WHY THIS IS A TABLE AND NOT ARITHMETIC
//
// docs/HERMES.md §12.5 and consolidated-backlog item 20 both say the same thing, and
// this is the file that honours it: an HL2 receiver has FOUR distinct indices
// and none of them may be computed from another.
//
//   ddcIndex    the hardware DDC. Selects the NCO register (RX1 = 0 = 0x02) and
//               the receiver's slot in the interleaved EP6 round. Contiguous
//               from zero, because the gateware requires it.
//
//   dspChannel  the WDSP channel id. Allocated from a PROCESS-WIDE pool of 32
//               shared with the transmit chain and every other backend, so it
//               is whatever was free at open() time. On a reconnect after a TX
//               channel has come and gone, receiver 0 is routinely not channel
//               0. This is the index that most obviously cannot be derived.
//
//   analyzerId  the spectrum instance. Its own space because a receiver can run
//               without a panadapter and (with diversity) two DDCs can feed one
//               display.
//
//   uiNumber    what the client sees: the slice id on the seam, and the number
//               in the operator's panadapter title. Stable across a reconnect
//               even when the DDC and DSP indices below it are not.
//
// The arithmetic breaks for real reasons rather than hypothetical ones.
// Diversity (§12.5, backlog 21) is a PRE-channel combiner: two DDCs feed one
// WDSP channel, so ddcIndex and dspChannel stop being 1:1. PureSignal (backlog
// 23) consumes four receivers of which two are transmit feedback and have no
// slice at all, so ddcIndex and uiNumber stop being 1:1 as well. Building the
// table at one receiver -- when every index happens to be 0 and any of the four
// would "work" -- is the only time it is cheap.
struct Hl2ReceiverIds {
    int ddcIndex = 0;
    int dspChannel = -1;      // -1 until the WDSP channel is actually open
    int analyzerId = -1;      // -1 when this receiver has no panadapter
    int uiNumber = 0;
    QString panId;            // the seam's pan identifier, e.g. "hl2-0"
};

// The pan-id string for a UI receiver number. One place, because it is parsed
// back by setPanCenter/setPanBandwidth/setPanRfGain and a mismatch between the
// two spellings is a control that silently does nothing.
QString hl2PanId(int uiNumber);

// Parse a pan id back to its UI number, or nullopt if it is not one of ours.
// A pan id we did not issue must not resolve to receiver 0 by accident -- that
// would point every unrecognised control at the first receiver.
std::optional<int> hl2PanNumber(const QString& panId);

// Where a stored DDC-index ROLE ends up after remove(removedDdc).
//
// Some things are remembered as "the receiver at DDC n" rather than by identity
// — which receiver owns transmit, which one the client's shared controls act on.
// remove() renumbers every index after the closed one, so those stored indices
// have to move with it. There are three cases and skipping the middle one is a
// SILENT misdirection:
//
//   role  <  removed   unchanged
//   role  >  removed   shifts down by one
//   role  == removed   the receiver is GONE; the caller must choose a new home,
//                      so this returns -1 rather than inventing one
//
// Closing DDC 0 of three with transmit on DDC 2 is the case that bites: without
// the shift, transmit still names index 2, which is now a different receiver —
// and nothing reads a TX NCO back to contradict it.
int hl2RoleAfterRemove(int role, int removedDdc);

class Hl2ReceiverMap {
public:
    // Build `count` receivers with contiguous DDC and UI indices. That identity
    // mapping is the STARTING state, not an invariant: dspChannel and analyzerId
    // are filled in as the DSP opens, and diversity/PureSignal will break the
    // ddc<->ui correspondence later. Nothing may assume it holds.
    void reset(int count);
    // Drop everything from `count` upward, KEEPING the survivors as they are.
    // reset(count) is not a substitute: it rebuilds from scratch, so every
    // surviving receiver's dspChannel and analyzerId go back to -1 — the ids
    // that are only knowable once the DSP has opened, and that this type exists
    // to stop anyone deriving from the index instead.
    void truncate(int count);
    void clear() { m_rx.clear(); }

    [[nodiscard]] int size() const noexcept { return static_cast<int>(m_rx.size()); }
    [[nodiscard]] bool empty() const noexcept { return m_rx.empty(); }

    [[nodiscard]] const std::vector<Hl2ReceiverIds>& all() const noexcept { return m_rx; }

    // Lookups. Each returns nullptr when nothing matches, and callers must check
    // -- an unknown index means a control arrived for a receiver that is not
    // running, which is a no-op, never "use the first one".
    [[nodiscard]] const Hl2ReceiverIds* byDdc(int ddcIndex) const;
    [[nodiscard]] const Hl2ReceiverIds* byUi(int uiNumber) const;
    [[nodiscard]] const Hl2ReceiverIds* byDspChannel(int dspChannel) const;
    [[nodiscard]] const Hl2ReceiverIds* byPanId(const QString& panId) const;

    // Mutating access by DDC index, for filling in ids as resources open.
    [[nodiscard]] Hl2ReceiverIds* mutableByDdc(int ddcIndex);

    // Append a receiver at the next DDC index, with the LOWEST UI number not
    // currently in use. Returns the new record's DDC index.
    //
    // Lowest-free rather than size(): after removing the middle of three the
    // surviving UI numbers are {0, 2}, so size() would be 2 and collide. And
    // lowest-free rather than monotonic, because the UI number is the seam's
    // slice id and that space is bounded by the radio's slice capacity — see
    // append()'s definition for what monotonic cost.
    int append();

    // Remove the receiver at `ddcIndex`, RENUMBERING the DDC indices of those
    // after it so they stay contiguous from zero — the gateware requires that,
    // because a receiver's DDC index IS its slot in the interleaved EP6 round.
    //
    // UI numbers and pan ids are deliberately left ALONE. Closing the middle
    // pane of three must not renumber the pane the operator still has open:
    // their third panadapter stays "the third one" even though it is now DDC 1.
    // That divergence is the whole reason these are separate index spaces.
    //
    // Returns false if the index does not exist.
    bool remove(int ddcIndex);

private:
    std::vector<Hl2ReceiverIds> m_rx;
};

}  // namespace AetherSDR::hl2
