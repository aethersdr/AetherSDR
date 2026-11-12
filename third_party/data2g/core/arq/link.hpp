// data2g/arq/link.py: one station's data transfer state for both
// directions, independent of the PHY (docs/arq.md §3-§6, §9a, §10).
//
// A line-for-line port: the same frames for the same inputs, the same calls
// on the policy and the burst in the same order. Seqs are unbounded
// (int64) here, 7 bits on the wire.
#pragma once

#include <algorithm>
#include <array>
#include <compare>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "arq/burst.hpp"
#include "arq/frames.hpp"
#include "arq/measured.hpp"

namespace data2g::arq {

inline constexpr int NO_PROGRESS_TURNS = 8;
inline constexpr int RESYNCS_BEFORE_FAIL = 3;
inline constexpr int REPEATS_BEFORE_SHRINK = 1;
inline constexpr int MAX_ESCALATION = 4;
inline constexpr int FLOOR_DECAY_TURNS = 4;  // clean turns (no escalation) that lower the escalation floor by one
inline constexpr int LINK_LOST_MISSES = 12;
// Primed retries of a failed control codeword at most (link.PRIME_TRIES; DATA2G_PRIME_TRIES, read once).
int prime_tries();
// link.PRIME: DATA2G_PRIME=0 turns primed reception off (read once).
bool prime_default();
// link.REPEAT: DATA2G_REPEAT=0 turns the combining of repeated controls off (read once).
bool repeat_default();

// Python's logging, by logger name ("data2g.link", "data2g.session") and
// level (20 info, 30 warning). Unset: silent. Set once, before any use.
struct LogSink {
    std::function<bool(const char* logger, int level)> enabled;
    std::function<void(const char* logger, int level, const std::string& msg)> write;
};
void set_log_sink(LogSink sink);
bool log_enabled(const char* logger, int level);
void log_write(const char* logger, int level, const std::string& msg);
std::string format(const char* fmt, ...);

// The absolute seq = s7 (mod SEQ_MOD) nearest anchor.
inline std::int64_t unwrap(std::int64_t s7, std::int64_t anchor) {
    std::int64_t d = pmod(s7 - anchor, SEQ_MOD);
    if (d >= SEQ_MOD / 2) d -= SEQ_MOD;
    return anchor + d;
}

class Station;

struct Recommendation {
    int rec = 0, hint = 1;
    std::optional<int> reply;
};

// link.Policy: everything Station, Session and Engine ask of the mode
// choice (GearPolicy on air), with the same defaults; has_*() say whether
// a policy keeps the default (outcome: none; airtime: left out of the log).
class Policy {
public:
    virtual ~Policy() = default;
    virtual std::pair<std::string, int> choose(Station& st, int escalation) = 0;
    virtual int payload_bytes(const std::string& submode) = 0;
    virtual int ctl_payload_bytes(const std::string& submode) { return payload_bytes(submode); }
    virtual int max_ctl(const std::string&) { return MAX_CTL; }
    virtual std::optional<Recommendation> recommend(Station&) { return std::nullopt; }  // nullopt: none (0, 1, -)
    virtual bool want_dup() { return false; }
    virtual int rv_cycle(const std::string& submode) = 0;
    virtual bool has_outcome() { return false; }
    virtual void outcome(const std::string&, int /*decoded*/, int /*sent*/, bool /*usable*/) {}
    // for the log
    virtual std::string mode_name(int rec) { return std::to_string(rec); }
    virtual bool has_airtime() { return false; }
    virtual std::optional<double> snr_est() { return std::nullopt; }
    virtual std::string measured_extra() { return {}; }  // after snr_est: spread, delay, MI
    // session
    virtual double airtime(const std::string&, int /*n_cw*/, bool /*dup*/) { return 0.0; }
    virtual std::string connect_mode(int /*cap*/, int /*tries*/) { return {}; }
    // seconds past t_turn to wait for a reply to `burst` (nullopt: none, REPLY_START_S alone)
    virtual std::optional<double> reply_hold(Station&, const TxBurst&) { return std::nullopt; }
    // engine: the receiver's measurements of a peer burst
    virtual void observe(const Measured&, const std::string& /*submode*/, double /*now*/) {}
    // engine: a peer burst's in-band SNR from power alone (the energy inputs)
    virtual void observe_energy(double /*snr_db*/, double /*now*/) {}
    // engine: the burst I asked the peer for last, its mode and seconds on air (nullopt: none)
    virtual std::optional<std::pair<std::string, double>> expected_reply() const { return std::nullopt; }
    // payload bytes the next data burst will carry (nullopt: unknown; the host's BUFFER, the session's coalescing)
    virtual std::optional<int> next_capacity(const Station&) const { return std::nullopt; }
};

struct Codeword {
    std::int64_t seq = 0, start = 0, length = 0;
    std::string submode;
    Bytes payload;
    int heard = 0;
    bool comp = false;
    std::int64_t cstart = 0;
    std::int64_t first_bn = -1;
    bool comp_known = false;
    bool inlined = false;  // first_bn carried it in its control (T_INLINE): no soft bits held, a resend is RV 0
};

struct ProtocolError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

class TxSide {
public:
    Bytes buf;  // stream bytes from buf_off on
    std::int64_t buf_off = 0, stream_end = 0;
    std::map<std::int64_t, Codeword> cws;  // outstanding
    std::int64_t base = 0, next = 0;
    std::optional<std::pair<std::int64_t, std::set<std::int64_t>>> ack;
    std::int64_t acked = 0, acked_wire = 0, acked_plain = 0;
    Bytes hist;
    std::int64_t hist_off = 0;
    // stream offsets where records start, from the one buf_off falls in
    // (clean() cuts only there), and each one's host offset
    std::vector<std::int64_t> recs, rec_host;
    std::int64_t written = 0;  // host bytes written: the next one's host offset
    std::int64_t mark = 0;     // stream offset of the first byte written after the last clean()
    using Offsets3 = std::tuple<std::int64_t, std::int64_t, std::int64_t>;
    // clean(): host offsets [from, to) on air, not acked, and to's stream offset
    std::optional<Offsets3> limbo;
    // the limbo settled, for the host: host offsets (from, split, to), delivered before split
    std::optional<Offsets3> cleaned;

    void write(ByteView data);
    bool pending() const;
    // stream bytes written but in no codeword yet
    std::int64_t unsent() const { return buf_off + static_cast<std::int64_t>(buf.size()) - stream_end; }
    // stream bytes the peer has not acked, sent or not (buf holds them from the first unacked codeword on)
    std::int64_t unacked() const { return static_cast<std::int64_t>(buf.size()); }
    bool on_ack(std::int64_t cum, const std::set<std::int64_t>& received);
    Codeword& new_codeword(const std::string& submode, int pb, bool compress, std::int64_t bn);
    std::vector<std::int64_t> missing() const;
    std::int64_t abandon();
    // CLEANTXBUFFER: drop the bytes never on air, but finish a record begun
    // on air; `upto`: only those before host offset upto. -> host offsets
    // (dropped from, to, limbo from, to), nullopt for LATE (a byte at or past
    // upto on air, or finishing a record that is). The limbo is on air, not
    // acked: settled by the next ACK that covers it or the next abandon.
    // Without one it is empty, at the drop.
    std::optional<std::tuple<std::int64_t, std::int64_t, std::int64_t, std::int64_t>> clean(
        std::optional<std::int64_t> upto = std::nullopt);
    // The session ended with the limbo unsettled. -> host offsets (from, split,
    // to): delivered before split (records acked whole), the rest unknown.
    std::optional<Offsets3> end_limbo();
    // (unsent, unacked) host bytes, as unsent() and unacked() count stream
    // bytes. `vara`: only those written after the last clean().
    std::pair<std::int64_t, std::int64_t> queued(bool vara = false) const {
        const std::int64_t lo = vara ? mark : 0;
        return {host_from(std::max(stream_end, lo)), host_from(std::max(buf_off, lo))};
    }

private:
    std::int64_t end() const { return buf_off + static_cast<std::int64_t>(buf.size()); }
    std::int64_t rec_at(std::int64_t x) const;          // the first record start at or after x (end() if none)
    std::int64_t hoff(std::int64_t x) const;            // the host offset of x, a record start or end()
    // host bytes from x to end(): the length bytes of records starting there left out
    std::int64_t host_from(std::int64_t x) const {
        return end() - x - (recs.end() - std::lower_bound(recs.begin(), recs.end(), x));
    }
    std::int64_t split(std::int64_t h);                 // h's stream offset, never on air: its record split there
    void cut(std::int64_t lo, std::int64_t hi);         // delete whole records [lo, hi), past stream_end
    std::int64_t limbo_start(std::int64_t c) const;     // where the unacked records before c begin (c: none)
    void settle(std::int64_t lo);                       // the limbo from lo on never arrived: drop it
};

class RxSide {
public:
    std::int64_t cum = 0;
    std::map<std::int64_t, std::pair<Bytes, bool>> buf;  // held above cum: (payload, compressed)
    RecordReader reader;
    Bytes out;
    Bytes hist;
    std::int64_t wire = 0, plain = 0;
    std::set<std::int64_t> comp_seqs;

    bool accept(std::int64_t seq, Bytes payload, bool comp = false);
    std::vector<std::int64_t> abandon(std::int64_t a);
};

enum class LinkState { ACTIVE, FAILED };

class Station {
public:
    Station(int direction, std::shared_ptr<Policy> policy, bool master = false, int key = 0,
            std::optional<int> max_misses = LINK_LOST_MISSES, int cap = 2, bool chat = false);
    virtual ~Station() = default;

    int direction;
    std::shared_ptr<Policy> policy;
    bool master;
    int key;
    std::optional<int> max_misses;
    int cap;
    bool last_rx_data = false;
    std::optional<int> peer_recommend;
    int peer_size_hint = 1;
    bool chat;
    bool peer_chat = false;
    int peer_queued = 0;
    bool peer_drained = false;  // the peer's last burst ended its queued data (T_DRAINED)
    bool peer_wants_dup = false;
    bool peer_reply_lost = false;  // the peer's last burst said its previous reply was lost
    bool peer_abandon = false;  // the peer's last burst carried T_ABANDON: it holds re-sliced data
    std::optional<int> peer_reply_recommend;
    TxSide tx;
    std::map<std::string, std::int64_t> stats;
    RxSide rx;
    LinkState state = LinkState::ACTIVE;
    std::string fail_reason;
    std::int64_t bursts_sent = 0;
    TxBurstPtr last_sent;
    std::optional<int> peer_burst;
    bool reply_lost = false;
    int misses = 0;
    int reply_escalation = 0;
    // the escalation the last recovery took: the next drop starts there
    int esc_floor = 0;
    int clean_ = 0;     // clean turns since the floor last moved
    int sent_esc_ = 0;  // the escalation my last built burst went at
    int no_progress = 0;
    int resyncs = 0;
    bool resync_due = false;
    // link.COMPRESS (DATA2G_COMPRESS=0 turns it off at construction)
    bool compress;
    // link.INLINE (DATA2G_INLINE=0 turns it off at construction)
    bool inline_data;

    std::map<std::int64_t, std::pair<std::int64_t, std::set<std::int64_t>>> snapshots;
    std::map<std::int64_t, std::vector<std::int64_t>> sent_seqs;
    int acted_on_ = BURST_MOD - 1;
    std::int64_t latest = -1, confirmed = -1;
    std::optional<Bytes> abandon_tlv;
    std::set<std::int64_t> abandon_bursts;
    int abandon_epoch = 0, peer_epoch = 0;
    bool answered_ = false;
    bool stale_ = false;  // the last burst handled repeated one already answered: its ACK may be stale
    // link.PRIME (DATA2G_PRIME=0 turns it off at construction): a peer control that fails blind is retried
    // primed (docs/arq.md §3a)
    bool prime;
    // primed reception: the peer's last control stream per kind (true: with data), its last one with its
    // header (submode, n_cw), the seq its next new codeword takes, whether my latest burst went again
    // identical, whether it carried T_DUPCTL
    std::map<bool, Bytes> peer_ctl_;
    std::optional<std::pair<Bytes, std::pair<std::string, int>>> peer_last_;
    std::optional<std::int64_t> peer_next_;
    bool repeated_ = false;
    bool asked_dup_ = false;
    std::pair<std::optional<int>, std::optional<int>> asked_;  // my latest build's recommendations: data, reply
    // The modes the peer's next burst likely comes in (link.Station.expected_modes).
    std::vector<std::string> expected_modes() const;
    Bytes last_stream_;  // the control stream read_control read last
    bool primed_ctl_ = false;  // ... and only a primed retry read it

    // What I know of the control of `rx`, a burst from my peer (link.Station.expect, §3a): hypotheses,
    // likeliest first, at most prime_tries().
    struct Hypothesis {
        Bytes tmpl, mask;  // control stream template, and the bits of it known
        bool pair = false;  // decode as an ARQ_DUP pair
        bool operator==(const Hypothesis&) const = default;
    };
    std::vector<Hypothesis> expect(const RxBurst& rx) const;

    int peer() const { return 1 - direction; }
    void write(ByteView data) { tx.write(data); }
    Bytes read();
    // Virtual so a binding can see a test's override of the Python method.
    virtual TxBurstPtr build(bool fresh = true);
    TxBurstPtr on_timeout(bool allow_repeat = true);  // nullptr: link lost
    bool handle(RxBurst& rx);
    // After handle(): CLEANTXBUFFER's limbo settles against its ACK when that
    // is exact (not a repeat's), so the host can report SETTLED before the
    // reply is built; else the next fresh build settles it.
    void settle_limbo() {
        if (tx.limbo && !stale_) start_abandon(0);
    }
    void answered() { answered_ = true; }
    std::optional<Bytes> ctl_pair(RxBurst& rx, int slot, int i);
    std::int64_t new_available() const { return tx.unsent(); }  // bytes my next new codewords can take
    // my latest burst carried data codewords and the peer has not acted on it yet
    bool data_in_flight() const {
        auto it = sent_seqs.find(latest);
        return it != sent_seqs.end() && !it->second.empty();
    }

private:
    // build()'s phases, as link.py splits Station.build
    struct Fit {
        int k;  // resends
        bool has_new;
        Bytes bitmap;
        int n_ctl, dup;
    };
    int escalation() const;
    bool data_allowed(bool fresh, const std::string& submode);
    void start_abandon(int reset);
    Ext control_tlvs(std::int64_t bn, std::optional<int> reply);
    Fit fit(Ext& ext, int cum, const std::vector<std::int64_t>& missing, bool has_new, int max_cw,
            const std::string& submode, bool fresh);
    std::vector<const Codeword*> new_codewords(const std::string& submode, int room, int k, std::int64_t spare,
                                               std::int64_t bn);
    void add_slots(TxBurst& burst, const std::vector<Bytes>& ctl, int dup, const std::vector<std::int64_t>& resend,
                   const std::vector<int>& rvs, const std::vector<const Codeword*>& fresh_cws);
    void record(std::int64_t bn, const TxBurstPtr& burst, std::set<std::int64_t> conveyed,
                const std::vector<std::int64_t>& resend, const std::vector<const Codeword*>& fresh_cws);
    void log_tx(std::int64_t bn, const TxBurst& burst, bool fresh, const Core& core, const Ext& ext, int dup,
                const std::vector<std::int64_t>& resend, const std::vector<int>& rvs,
                const std::vector<const Codeword*>& fresh_cws, int escalation, int hint, std::optional<int> reply,
                const Codeword* inl);
    Ext data_tlvs(const std::vector<std::int64_t>& resend, const std::vector<int>& rvs,
                  const std::vector<const Codeword*>& fresh_cws) const;
    // handle()'s phases, as link.py splits Station._handle
    using PeerAsk = std::pair<std::optional<int>, int>;  // the peer's recommendation and size hint
    struct DataTaken {
        std::vector<std::pair<std::optional<std::int64_t>, int>> slots;
        int n_dec = 0, n_old = 0;  // decoded; already had
        bool progress = false;
    };
    bool handle_inner(RxBurst& rx);
    std::optional<std::pair<Control, int>> read_control(RxBurst& rx, bool outcome);
    // primed reception (§3a), as link.py's
    struct Field {
        int value = 0, mask = 0;
    };
    struct Tlv {
        int type;
        Bytes value, mask;
        bool operator==(const Tlv&) const = default;
    };
    struct Way {
        std::array<Field, 9> core;  // frames Core's fields in wire order (CORE_BITS)
        std::vector<Tlv> ext;
    };
    std::vector<Way> variants(bool data, int n_cw, int ctl_slots, bool pair) const;
    static std::optional<Hypothesis> pack_hypothesis(const Way& w, int cpb, int max_ctl, bool pair, int n_ctl);
    std::pair<std::optional<Bytes>, bool> primed(RxBurst& rx, int slot, int i, const std::vector<Hypothesis>& hyps);
    std::optional<std::string> implausible(const Core& core, const Ext& ext) const;
    std::pair<PeerAsk, bool> note_peer(const Core& core, const Ext& ext);
    std::pair<std::int64_t, bool> take_ack(const Core& core, const Ext& ext);
    std::vector<std::int64_t> decoded_bursts(std::int64_t acted, std::int64_t cum, const std::set<std::int64_t>& received) const;
    bool take_abandon(RxBurst& rx, const Ext& ext);
    DataTaken take_data(RxBurst& rx, const Core& core, const Ext& ext, int dup, std::int64_t acted, bool outcome);
    void log_rx(RxBurst& rx, const Core& core, int dup, const DataTaken& data, std::int64_t base0, std::int64_t next0,
                std::int64_t cum0, const PeerAsk& was, bool repeat, bool abandoned);
    // What is wrong with a CRC-valid control, checked before any state changes.
    static std::optional<std::string> check(const Core& core, const Ext& ext, int n_data, int pb);
    bool malformed(const std::string& why);  // log, -> false: dropped like a failed control
    bool comp_bit(std::int64_t seq) const;
    std::vector<std::pair<std::optional<std::int64_t>, int>> map_slots(const Core& core, const Ext& ext, int n_cw,
                                                                      std::int64_t acted);
    void forget_all(RxBurst& rx);
    void fail(const std::string& why);
    void watchdog(bool progress);
    std::string mode(std::optional<int> rec);
    std::string burst_desc(const std::string& submode, int n_cw, bool dup = false);
    std::string snr();
};

}  // namespace data2g::arq
