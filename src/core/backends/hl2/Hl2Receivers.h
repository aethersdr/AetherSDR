#pragma once

#include <QString>

#include <optional>
#include <vector>

namespace AetherSDR::hl2 {

// One receiver's four index spaces, none computable from another
// (docs/HERMES.md §12.5):
//   ddcIndex    hardware DDC: NCO register (RX1 = 0 = 0x02) and EP6 slot;
//               contiguous from zero (gateware requirement).
//   dspChannel  WDSP channel from the process-wide pool of 32 shared with TX
//               and other backends: whatever was free at open().
//   analyzerId  spectrum instance; a receiver may have none.
//   uiNumber    the seam's slice id and the panadapter title number.
// Diversity (two DDCs into one channel) and PureSignal (feedback DDCs with no
// slice) break any 1:1 mapping.
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
// Roles stored as "the receiver at DDC n" (TX owner, shared-control target) must
// follow remove()'s renumbering: role < removed is unchanged, role > removed
// shifts down by one, role == removed returns -1 (caller picks a new home).
// Skipping the shift silently retargets TX; nothing reads the TX NCO back.
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
    // Not size(): survivors {0, 2} would collide at 2. Not monotonic: the UI
    // number is the slice id, bounded by slice capacity.
    int append();

    // Remove the receiver at `ddcIndex`, RENUMBERING the DDC indices of those
    // after it so they stay contiguous from zero — the gateware requires that,
    // because a receiver's DDC index IS its slot in the interleaved EP6 round.
    //
    // UI numbers and pan ids are left alone, so open panes keep their numbers.
    // Returns false if the index does not exist.
    bool remove(int ddcIndex);

private:
    std::vector<Hl2ReceiverIds> m_rx;
};

}  // namespace AetherSDR::hl2
