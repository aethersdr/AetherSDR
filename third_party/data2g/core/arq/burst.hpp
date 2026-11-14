// data2g/arq/burst.py: what a burst is to the PHY (docs/arq.md §2, §3): its
// slots, each a payload at an RV under a codeword identity (MaskId), and
// the received side's interface. Shared by the ARQ (link, session, engine)
// and KISS, which has no Station; phy.hpp turns a MaskId into the CRC mask
// value (mask_value) and a TxBurst into audio.
#pragma once

#include <compare>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "arq/frames.hpp"

namespace data2g::arq {

// (session key, direction, 7-bit seq or SEQ_MOD + control index)
struct MaskId {
    int key = 0, direction = 0, seq = 0;
    bool operator==(const MaskId&) const = default;
};

inline MaskId ctl_mask(int direction, int i, int key = 0) { return {key, direction, SEQ_MOD + i}; }
inline const MaskId COMPACT_CONNECT = ctl_mask(0, 4);  // a compact CONNECT's mask (frames pack_connect); ctl_mask's i is 0-3
inline constexpr int EPOCH_MOD = 64;  // abandon epochs in a data codeword's identity
// comp: deflated (T_COMP); epoch: the sender's abandon epoch (its slicing).
// Both folded into the direction byte, so a codeword decoded under the
// wrong compression or slicing assumption fails its CRC.
inline MaskId data_mask(int direction, std::int64_t seq, int key = 0, bool comp = false, int epoch = 0) {
    return {key, direction | (comp ? 2 : 0) | static_cast<int>(pmod(epoch, EPOCH_MOD)) << 2,
            static_cast<int>(pmod(seq, SEQ_MOD))};
}

// T_INLINE's 2-byte check (frames.inline_check): CRC-32 of the codeword's
// identity and its padded payload, low 16 bits, big-endian.
Bytes inline_check(const MaskId& m, ByteView payload);

struct Slot {
    MaskId mask_id;
    int rv = 0;
    Bytes payload;
};

struct TxBurst {
    std::string submode;
    std::vector<Slot> slots;
    std::int64_t burst_seq = 0;  // absolute count of this station's bursts
    int cap = 0;                 // bandwidth cap code the TX filter is for (cpm::tx_filter; modes cap_hz)
    // submode "cw": a CW ID (cwid::audio(cw_text, cw_wpm)), no slots (engine.py's cwid.CwBurst)
    std::string cw_text;
    double cw_wpm = 0.0;
};
using TxBurstPtr = std::shared_ptr<const TxBurst>;

bool dup_ctl(const TxBurst& b);  // its control sent twice (ARQ_DUP)

// A soft-bit store key: data seq `seq` of direction `peer` (Python's
// (peer, seq)), or control codeword `index` of this burst, combined as a
// pair (Python's ("ctl", id(rx), index)).
struct SoftKey {
    bool ctl = false;
    int peer = 0;
    std::int64_t seq = 0;
    int index = 0;
    auto operator<=>(const SoftKey&) const = default;
};
// Failed controls kept across bursts for decode_repeat (Python's ("rep", 1, 0) and ("rep", 2, 0)): the link's,
// the session's connect frames. The burst's own pair key is {true, 0, 0, i}.
inline constexpr SoftKey REPEAT_KEY{true, 1, 0, 0}, REPEAT_SESSION_KEY{true, 2, 0, 0};

// One decode() the caller is about to ask for.
struct DecodeRequest {
    int slot;
    MaskId mask;
    int rv;
    SoftKey key;
};

class RxBurst {
public:
    virtual ~RxBurst() = default;
    virtual const std::string& submode() const = 0;
    virtual int n_cw() const = 0;
    // Payload if slot decodes with this mask at this RV (combined with soft
    // bits under key, which a failed decode adds to).
    virtual std::optional<Bytes> decode(int slot, const MaskId& mask, int rv, const SoftKey* key) = 0;
    virtual void forget(const SoftKey& key) = 0;
    // The decode()s the caller is about to make, in this order, offered up front so an implementation can run
    // their independent parts at once (a slow machine's idle cores). Optional, and the answers are the same.
    virtual void prepare(std::span<const DecodeRequest>) {}
    // A primed retry (docs/arq.md §3a): control slot `slot` (`pair`: with slot + 1 at RV 1, combined) decoded
    // again with the payload bits `tmask` sets known to be `tmpl`'s; its CRC and score still decide. A burst
    // that can't (can_prime() false) is never asked.
    virtual bool can_prime() const { return false; }
    virtual std::optional<Bytes> decode_known(int /*slot*/, const MaskId& /*mask*/, ByteView /*tmpl*/,
                                              ByteView /*tmask*/, bool /*pair*/) {
        return std::nullopt;
    }
    // Control slot `slot` combined with the same slot of the bursts before it kept under `key` (their decodes
    // failed; a failure here keeps this one too): bit-identical repeats. Its payload must also score on this
    // burst's own soft bits. A burst that can't (can_repeat() false) is never asked.
    virtual bool can_repeat() const { return false; }
    virtual std::optional<Bytes> decode_repeat(int /*slot*/, const MaskId& /*mask*/, const SoftKey& /*key*/) {
        return std::nullopt;
    }
    // CPM: slots its header says are control (2: duplicated); OFDM: 0.
    virtual int n_ctl_slots() const { return 0; }
};

}  // namespace data2g::arq
