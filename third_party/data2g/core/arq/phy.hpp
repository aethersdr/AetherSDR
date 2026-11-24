// data2g/arq/phy.py: the ARQ layer on the real modem. A TxBurst -> audio;
// a received burst -> an RxBurst (masked CRCs, soft-bit combining across
// resends under a SoftKey), decision-directed re-estimation (DD), and the
// gear shifter's measurements.
//
// Threads: a ModemRx is one burst's decoder state, used by one thread at a
// time; separate ModemRx share nothing mutable (the burst and its soft bits
// are const and shared, codes:: caches are thread-safe), so a whole burst
// can decode on a worker while the engine keeps searching. Within a burst,
// the split points are marked "split:" in phy.cpp.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "arq/link.hpp"
#include "arq/modes.hpp"
#include "arq/predictor.hpp"
#include "codes/codes.hpp"
#include "cpm/cpm.hpp"
#include "modem/modem.hpp"

namespace data2g::arq {

// DATA2G_DD (default 1; 0 turns it off), read once: when a slot fails, its
// decoder's a-posteriori LLRs and every decoded codeword of the burst
// become soft pilots (equalizer::refine) and it decodes again, up to
// DD_ITERS times.
bool dd_default();
inline constexpr int DD_ITERS = 2;
// CPM data slots (GF(M)): DD makes the metric coherent (cpm::dd_metric, a
// channel per row from the decoder's soft decisions), up to CPM_DD_ITERS
// re-decodes. DATA2G_CPM_DD (default 1), read once, turns it off alone.
bool cpm_dd_default();
inline constexpr int CPM_DD_ITERS = 4;
// phy.cpm_pass: a CPM data slot's mask-free decode, the same whenever it is
// made: the square law (`soft`, the slot's), then while it fails up to
// CPM_DD_ITERS re-decodes on cpm::dd_metric from `earlier` (converged
// posteriors of earlier slots' passes) and its own last posterior. `late`:
// stop DD (the budget); empty: never.
cpm::Pass cpm_pass(const cpm::Received& r, const codes::Spec& s, std::span<const double> soft,
                   const cpm::Posts& earlier, int slot, const std::function<bool()>& late = {});
// cpm_pass as a receiver's cpm::Stream makes it (no budget: the burst is still arriving).
cpm::PassFn cpm_pass_fn();
// A live receiver's DD starts no refines this long after the burst's decode began.
inline constexpr double DD_BUDGET_S = 1.0;
// A control slot that fails is decoded again with the control stream past
// each of these byte counts in turn known zero (codes::decode_raw_pinned;
// phy.PIN_CUTS, POLAR_PIN_CUTS). LDPC (phy.PIN_EST; DATA2G_PIN_EST=0, read
// once, turns it off): once, leaning toward the padding its soft bits show
// (codes::pad_prior), offset EST in place of the cuts.
bool pin_est_default();
inline constexpr int EST = 0;
inline constexpr int PIN_CUTS[] = {16, 32};
inline constexpr int POLAR_PIN_CUTS[] = {12, 16};
// A primed control (docs/arq.md §3a) must also score this many nats over random bits (codes::score).
inline constexpr double PRIMED_SCORE = 10.0;
// ... and passes the CRC on one of the polar list's first PRIMED_PATHS paths (phy.PRIMED_PATHS).
inline constexpr int PRIMED_PATHS = 4;

// A slot's CRC mask from its identity (burst.hpp): zlib.crc32 of key,
// direction, seq; key 0: mask 0, but a compact CONNECT's.
std::uint32_t mask_value(const MaskId& m);

// A TxBurst -> unit-RMS audio. Throws std::out_of_range for an unknown mode.
std::vector<double> tx_audio(const TxBurst& burst);

// A received burst: modem::receive's (OFDM) or cpm.receive's.
struct Heard {
    std::shared_ptr<const modem::Received> ofdm;
    std::shared_ptr<const cpm::Received> cpm;  // reads spec, dup, soft, E
    const Mode& mode() const;
    int n_cw() const;
};

// Per-slot soft bits in mapping order (phy.soft_bits): computed once per
// burst, shared by everyone who decodes it (KISS, then ARQ).
using SlotSoft = std::vector<std::vector<double>>;
std::shared_ptr<const SlotSoft> soft_bits(const Heard& r);

// The gear shifter's inputs (phy.measure).
Measured measure(const Heard& r);

// A codeword's stored soft bits: Python's (buffer, highest RV, submode, where).
struct SoftEntry {
    std::vector<double> buf;  // buffer_len, code order
    int top = 0;
    std::string submode;
    int slot = 0, rv = 0;  // where it was last stored from
    MaskId mask;
};

using SoftStore = std::map<SoftKey, SoftEntry>;

// A burst read under masks, with no store of its own: what a monitor (and
// a broadcast's reader) needs. ModemRx decodes one heard; a monitor shown
// our own transmission reads the payloads it sent (monitor::Monitor::sent).
class PlainRx {
public:
    virtual ~PlainRx() = default;
    virtual const std::string& submode() const = 0;
    virtual int n_cw() const = 0;
    // ModemRx's, below.
    virtual std::optional<Bytes> decode_stored(int slot, const MaskId& mask, int rv, std::optional<SoftEntry>& stored) = 0;
    virtual std::optional<Bytes> decode_plain(int slot, const MaskId& mask) = 0;
    virtual std::vector<Bytes> raw(int slot) = 0;
    virtual std::vector<Bytes> raw_pair(int slot) = 0;
    virtual std::optional<Bytes> decode_pair(int slot, const MaskId& mask) = 0;
    virtual int n_ctl_slots() const = 0;  // CPM: slots its header says are control; OFDM: 0
    // The slot decoded, mask open and nothing pinned, to a codeword (LDPC: every parity check satisfied): alone,
    // or with `pair` combined with slot + 1 as raw_pair does. Polar never says: false.
    virtual bool converged(int slot, bool pair) = 0;
};
using Clock = std::function<double()>;  // seconds, monotonic
double monotonic();

// One decode of a slot's soft bits combined with what is stored for it: ModemRx::decode_stored's work for one
// DD pass, as a value, so it can be made ahead (prepare) and used where the sequential decode wants it.
struct DecodePass {
    std::vector<double> buf;   // the combined buffer
    bool ok = false;
    Bytes data;                // the payload, if ok
    std::vector<double> post;  // DD: the coded bits' a-posteriori LLRs, mapping order
};

class ModemRx : public RxBurst, public PlainRx {
public:
    // store: this station's soft bits across bursts (nullptr: decode() with
    // a key keeps nothing). dd_budget: seconds from here after which DD
    // starts no more refines (nullopt: no limit). soft: soft_bits(r), if
    // already made.
    ModemRx(Heard r, SoftStore* store = nullptr, std::optional<double> dd_budget = std::nullopt,
            std::shared_ptr<const SlotSoft> soft = nullptr, bool dd = dd_default(), Clock clock = monotonic);

    const std::string& submode() const override { return submode_; }
    int n_cw() const override { return n_cw_; }
    std::optional<Bytes> decode(int slot, const MaskId& mask, int rv, const SoftKey* key) override;
    void forget(const SoftKey& key) override;
    // Runs the first pass of each request's slot at once on the pool (only on the burst's own estimate, and
    // with every key distinct), for decode() to use. Same answers as decoding one at a time.
    void prepare(std::span<const DecodeRequest> reqs) override;

    // How the decodes so far went, for the log: channel re-estimates made (DD passes) and whether the DD
    // budget ran out.
    int dd_refines() const { return refines_; }
    bool dd_spent() const { return late(); }
    // Slots decoded so far (under any mask), and slots tried that never did, for a burst log.
    int n_decoded() const { return static_cast<int>(ok_.size()); }
    int n_failed() const { return static_cast<int>(tried_.size() - ok_.size()); }

    // decode() without a store: `stored` is what the key holds (nullopt:
    // nothing); a failed decode replaces it, a miss before decoding (slot
    // out of range, CPM control/data mismatch) leaves it. An entry stored under
    // another submode is dropped (logged) and the decode starts fresh.
    std::optional<Bytes> decode_stored(int slot, const MaskId& mask, int rv, std::optional<SoftEntry>& stored) override;
    std::optional<Bytes> decode_plain(int slot, const MaskId& mask) override;  // no key
    // The slot's mask-free decode as payload bytes, CRC unchecked: polar's
    // list best first, LDPC's one candidate if it converged. A broadcast
    // control reads its group from these (docs/broadcast.md §2).
    std::vector<Bytes> raw(int slot) override;
    // raw() of a codeword sent twice: `slot` at RV 0 and `slot + 1` at RV 1,
    // combined (a broadcast control under DUP). Empty if the two aren't both
    // there, or (CPM) not both control.
    std::vector<Bytes> raw_pair(int slot) override;
    // The pair raw_pair reads, CRC-checked under `mask` (its padding pinned if it fails plain).
    std::optional<Bytes> decode_pair(int slot, const MaskId& mask) override;
    int n_ctl_slots() const override { return n_ctl_slots_; }
    // phy.ModemRx.decode_known (RxBurst's).
    bool can_prime() const override { return true; }
    std::optional<Bytes> decode_known(int slot, const MaskId& mask, ByteView tmpl, ByteView tmask, bool pair) override;
    bool converged(int slot, bool pair) override;
    // phy.ModemRx.decode_repeat (RxBurst's). own_score: its check, `payload` against this burst's own soft bits.
    bool can_repeat() const override { return true; }
    std::optional<Bytes> decode_repeat(int slot, const MaskId& mask, const SoftKey& key) override;
    bool own_score(int slot, const MaskId& mask, ByteView payload);

    const std::shared_ptr<const SlotSoft>& soft() const { return soft_; }

    // Done with by its session, handed to a reader (a monitor) on another thread: no store, and a slot already
    // read answers from that read, under its mask (another mask: no), without decoding again. A read made
    // primed, combined with stored bits, or as a repeat is so seen by the reader too.
    void for_reader();
    // Reads run (plain, stored, primed), not answered from an earlier one: for_reader's saving, measured.
    int reads_run() const { return reads_run_; }

private:
    struct Raw {
        std::vector<std::uint8_t> cands, usable;
    };
    using Est = std::shared_ptr<const modem::DataEstimate>;  // nullptr: the burst's own
    using SoftCache = std::shared_ptr<std::map<int, std::vector<double>>>;
    struct DdState {
        Est est;
        SoftCache soft;
        // CPM: the posteriors each slot's coherent metric reads (cpm::dd_metric; nullptr: the burst's own soft)
        std::shared_ptr<const cpm::Posts> posts;
        bool refined() const { return est || posts; }
    };

    std::optional<Bytes> plain_read(int slot, const MaskId& mask);
    std::optional<Bytes> stored_read(int slot, const MaskId& mask, int rv, std::optional<SoftEntry>& stored);
    std::optional<Bytes> known_read(int slot, const MaskId& mask, ByteView tmpl, ByteView tmask, bool pair);
    std::optional<std::optional<Bytes>> reused(int slot, std::uint32_t m) const;  // for_reader: a slot's read
    std::optional<Bytes> note(int slot, std::uint32_t m, std::optional<Bytes> p);  // p, kept in reads_ if a read

    const codes::Spec& spec(int slot) const;
    const std::vector<double>& slot_soft(int slot);
    const Raw& decoded(int slot, const codes::Spec& s);
    std::vector<int> offsets(const codes::Spec& s, int i) const;
    const Raw& pinned(int slot, const codes::Spec& s, int off);
    bool pair_ok(int slot) const;
    const Raw& paired(int slot, const codes::Spec& s, int off);  // off -1: nothing pinned
    std::optional<Bytes> check_pinned(int slot, const codes::Spec& s, std::uint32_t m, int i);
    bool dd(const codes::Spec& s) const;
    int iters() const { return n_ctl_slots_ ? CPM_DD_ITERS : DD_ITERS; }
    bool late() const;
    void learn(int slot, const codes::Spec& s, const Bytes& payload, int rv, std::uint32_t m);
    bool refine(int slot, const codes::Spec& s, std::vector<double> post);  // false: the budget ran out inside it
    void remake_estimate(const codes::Spec& s);
    void undo(int slot, const DdState& saved);
    Est dd_estimate() const;

    Heard r_;
    SoftStore* store_;
    std::shared_ptr<const SlotSoft> soft_;
    bool dd_;
    Clock clock_;
    std::optional<double> dd_until_;
    std::string submode_;
    int n_cw_, n_ctl_slots_;
    const codes::Spec* data_spec_;
    const codes::Spec* ctl_spec_;
    std::map<std::pair<int, std::uint32_t>, std::optional<Bytes>> memo_;
    std::map<int, Raw> raw_;
    std::map<std::pair<int, int>, Raw> pinned_;  // (slot, offset): decoded with the payload past offset bytes known zero
    std::map<std::pair<int, int>, Raw> pairs_;   // (slot, offset or -1): slots slot, slot + 1 combined (RV 0, 1)
    std::set<int> tried_, ok_;  // ok_ within tried_
    std::map<int, std::pair<std::uint32_t, Bytes>> reads_;  // slot -> (mask, payload): the first read of it that held
    bool reader_ = false;
    int reads_run_ = 0;
    std::map<int, std::vector<double>> post_;  // DD: slot -> LLRs of its coded bits
    // CPM: data slots whose pass is made (ModemRx._cpm_passes), and the converged ones' posteriors
    std::set<int> passed_;
    cpm::Posts pass_post_;
    void cpm_passes(int upto);
    bool blind_ = false;
    int refines_ = 0;
    struct Prepared {
        int rv;
        std::uint32_t mask;
        bool had_stored;
        DecodePass pass;
    };
    std::map<int, Prepared> prepared_;  // by slot
    // dd_estimate()'s per-symbol results, keyed on the symbol's input LLRs: a DD iteration changes
    // one codeword's posterior, and the rest of the burst's symbols come out the same again.
    struct SymCache {
        std::vector<double> L;               // m LLRs per symbol, as last computed
        std::vector<std::complex<double>> z;  // the soft pilot (0: none)
        std::vector<double> w;                // its weight
        std::vector<std::uint8_t> valid;
    };
    mutable SymCache sym_cache_;
    DdState cur_{nullptr, std::make_shared<std::map<int, std::vector<double>>>(), nullptr};
};

}  // namespace data2g::arq
