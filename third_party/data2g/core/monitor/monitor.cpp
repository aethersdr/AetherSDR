#include "monitor/monitor.hpp"

#include <algorithm>
#include <cstdio>
#include <stdexcept>

#include "arq/modes.hpp"
#include "kisslink/kisslink.hpp"
#include "tables/zdict.hpp"

namespace data2g::monitor {

namespace {

using namespace arq;

constexpr std::size_t MAX_KEYS = 16;  // sessions followed at once: each is CRC checks on every burst

std::string hex4(int v) {
    char b[8];
    std::snprintf(b, sizeof b, "%04x", v & 0xFFFF);
    return b;
}

bool printable(std::uint8_t c) { return c >= 0x20 && c < 0x7F; }

std::string ext_desc(const Ext& ext) {
    std::vector<std::string> out;
    for (const auto& [t, v] : ext) {
        std::string s = ext_name(t);
        if (t == T_NEW && !v.empty()) s += " " + std::to_string(v[0]);
        if (t == T_ABANDON && v.size() >= 2) s += " " + std::to_string(v[0]) + " epoch " + std::to_string(v[1] >> 1);
        if (t == T_BUFFER && v.size() == 2) s += " " + std::to_string(v[0] << 8 | v[1]);
        if (t == T_REPLY && !v.empty()) {
            const Mode* m = decode(v[0]);
            s += " " + (m ? std::string(m->name) : "?");
        }
        out.push_back(s);
    }
    std::string s;
    for (const auto& x : out) s += (s.empty() ? "" : ", ") + x;
    return s;
}

// An ARQ_DUP control pair (RV 0 then RV 1) combined.
std::optional<Bytes> pair(PlainRx& rx, int slot, const MaskId& mask) {
    std::optional<SoftEntry> st;
    rx.decode_stored(slot, mask, 0, st);
    return rx.decode_stored(slot + 1, mask, 1, st);
}

// A burst of ours, read back from what was sent: a slot decodes, at any RV,
// under a mask with the CRC value it was sent under.
class SentRx : public PlainRx {
public:
    explicit SentRx(const TxBurst& b) : b_(b) {}
    const std::string& submode() const override { return b_.submode; }
    int n_cw() const override { return static_cast<int>(b_.slots.size()); }
    std::optional<Bytes> decode_stored(int slot, const MaskId& mask, int, std::optional<SoftEntry>&) override {
        return decode_plain(slot, mask);
    }
    std::optional<Bytes> decode_plain(int slot, const MaskId& mask) override {
        if (slot < 0 || slot >= n_cw() || mask_value(b_.slots[static_cast<std::size_t>(slot)].mask_id) != mask_value(mask))
            return std::nullopt;
        return b_.slots[static_cast<std::size_t>(slot)].payload;
    }
    std::vector<Bytes> raw(int slot) override {
        if (slot < 0 || slot >= n_cw()) return {};
        return {b_.slots[static_cast<std::size_t>(slot)].payload};
    }
    std::vector<Bytes> raw_pair(int) override { return {}; }  // raw() always reads: never asked
    bool converged(int slot, bool pair) override { return !pair && slot >= 0 && slot < n_cw(); }
    std::optional<Bytes> decode_pair(int slot, const MaskId& mask) override { return decode_plain(slot, mask); }
    int n_ctl_slots() const override {  // CPM: what its header would say
        if (!mode_at(b_.submode).is_cpm()) return 0;
        return static_cast<int>(std::count_if(b_.slots.begin(), b_.slots.end(),
                                              [](const auto& s) { return s.mask_id.seq >= SEQ_MOD; }));
    }

private:
    const TxBurst& b_;
};

std::uint16_t be16(ByteView b, std::size_t i) { return static_cast<std::uint16_t>(at(b, i) << 8 | at(b, i + 1)); }

}  // namespace

std::string text_dump(ByteView b, const std::vector<bool>& unknown) {
    std::string out;
    for (std::size_t i = 0; i < b.size(); ++i) {
        const auto c = b[i];
        if (i < unknown.size() && unknown[i]) {
            out += "<?" "?>";  // split: "??>" is a trigraph
            continue;
        }
        if (printable(c)) {
            out += static_cast<char>(c);
            continue;
        }
        char e[8];
        std::snprintf(e, sizeof e, "<%02X>", c);
        out += e;
        if (c == '\n') out += '\n';
    }
    return out;
}

std::string hex_dump(ByteView b, const std::vector<bool>& unknown) {
    auto unk = [&](std::size_t j) { return j < unknown.size() && unknown[j]; };
    std::string out;
    char buf[24];
    for (std::size_t i = 0; i < b.size(); i += 16) {
        std::snprintf(buf, sizeof buf, "%08zx:", i);
        out += buf;
        for (std::size_t j = i; j < i + 16; ++j) {
            if ((j - i) % 2 == 0) out += ' ';
            if (j >= b.size()) {
                out += "  ";
            } else if (unk(j)) {
                out += "??";
            } else {
                std::snprintf(buf, sizeof buf, "%02x", b[j]);
                out += buf;
            }
        }
        out += "  ";
        for (std::size_t j = i; j < std::min(i + 16, b.size()); ++j)
            out += unk(j) ? '?' : printable(b[j]) ? static_cast<char>(b[j]) : '.';
        out += '\n';
    }
    return out;
}

std::string render(const Dump& d, Format f) {
    std::string out = d.head;
    for (const auto& [label, bytes, unknown] : d.payloads) {
        const auto n_unk = std::count(unknown.begin(), unknown.end(), true);
        out += "  " + label + ", " + std::to_string(bytes.size()) + " B" +
               (n_unk ? " (" + std::to_string(n_unk) + " unknown)" : "") + ":\n";
        const std::string body = f == Format::HEX ? hex_dump(bytes, unknown) : text_dump(bytes, unknown);
        for (std::size_t i = 0; i < body.size();) {
            const std::size_t nl = std::min(body.find('\n', i), body.size());
            out += "    " + body.substr(i, nl - i) + "\n";
            i = nl + 1;
        }
    }
    return out;
}

std::string Monitor::who(int key, int direction) const {
    const auto it = keys_.find(key);
    const Pair p = it == keys_.end() ? Pair{} : it->second;
    auto name = [](const std::string& s) { return s.empty() ? std::string("?") : s; };
    std::string s = direction == 0 ? name(p.caller) + ">" + name(p.callee) : name(p.callee) + ">" + name(p.caller);
    if (p.caller.empty() && !p.id.empty()) s += " (" + p.id + "'s session)";
    return s;
}

void Monitor::learn(int key, const std::string& caller, const std::string& callee, const std::string& id) {
    Pair& p = keys_[key];
    if (!caller.empty()) p.caller = caller, p.callee = callee;
    if (!id.empty()) p.id = id;
    p.seen = ++seen_;
    if (keys_.size() <= MAX_KEYS) return;
    // ponytail: the least recently heard session is forgotten; a ring per band if many run at once
    const auto old = std::min_element(keys_.begin(), keys_.end(), [](const auto& a, const auto& b) { return a.second.seen < b.second.seen; });
    sides_.erase({old->first, 0});
    sides_.erase({old->first, 1});
    keys_.erase(old);
}

Dump Monitor::burst(const BurstHeard& b, const std::string& when) {
    Dump d;
    char line[200];
    std::snprintf(line, sizeof line, "%s  %s x%d", when.c_str(), b.submode.c_str(), b.n_cw);
    d.head = line;
    if (b.snr_db) {
        std::snprintf(line, sizeof line, "  SNR %.1f dB", *b.snr_db);
        d.head += line;
    }
    d.head += "\n";
    if (b.lost) {
        d.head += "  header only: burst lost\n";
        return d;
    }
    ModemRx rx(b.heard, nullptr, DD_BUDGET_S, b.soft);
    read(rx, d);
    return d;
}

Dump Monitor::sent(const TxBurst& b, const std::string& when) {
    Dump d;
    d.tx = true;
    d.head = when + "  TX " + b.submode + " x" + std::to_string(b.slots.size()) + "\n";
    SentRx rx(b);
    read(rx, d);
    return d;
}

void Monitor::read(PlainRx& rx, Dump& d) {
    try {
        if (broadcast(rx, d) || keyed(rx, d)) return;
    } catch (const std::exception& e) {  // passed a CRC but doesn't parse
        d.head += std::string("  malformed: ") + e.what() + "\n";
        return;
    }
    d.head += "  unidentified (no CRC passes under a mask known here); raw codewords, unverified\n";
    std::string none;
    for (int i = 0; i < rx.n_cw(); ++i) {
        const auto r = rx.raw(i);
        if (r.empty()) none += " " + std::to_string(i);
        else d.payloads.push_back({"slot " + std::to_string(i), r[0], {}});
    }
    if (!none.empty()) d.head += "  no decode: slot" + none + "\n";
}

bool Monitor::broadcast(PlainRx& rx, Dump& d) {
    const auto ctl = kisslink::KissLink::read_burst_control(rx, rx.n_cw());
    if (!ctl) return false;
    const auto& [c, start] = *ctl;
    auto [frames, ok, lost] = kisslink::KissLink::read_burst_frames(rx, start, kisslink::group_key(c.group));
    d.head += "  BCAST \"" + c.group + "\"" + (c.call ? " from " + *c.call : "") + ": " + std::to_string(frames.size()) +
              " frame(s)" + (lost ? ", " + std::to_string(lost) + " lost" : "") + (c.reports ? ", rate reports" : "") + "\n";
    for (std::size_t i = 0; i < frames.size(); ++i) {
        std::string label = "frame " + std::to_string(i + 1);
        if (const auto ax = kisslink::parse_ax25(frames[i]))
            label += " (AX.25 " + ax->src + ">" + ax->dst + (ax->connected ? ", connected" : ", UI") + ")";
        d.payloads.push_back({label, std::move(frames[i]), {}});
    }
    return true;
}

bool Monitor::keyed(PlainRx& rx, Dump& d) {
    // a compact CONNECT (CPM's one short control codeword): its own mask
    const Mode& m = mode_at(rx.submode());
    if (ceil_div(CONNECT_CTL_BYTES, ctl_payload_bytes(m)) > max_ctl(m))
        if (auto p = rx.decode_plain(0, COMPACT_CONNECT)) {
            session_frame(unpack_connect(*p), 0, 0, d);
            return true;
        }
    std::vector<std::pair<int, int>> masks{{0, 0}, {1, 0}};  // (key, direction); key 0: CONNECT, CQ, ID
    for (const auto& [key, p] : keys_)
        for (int dir : {0, 1}) masks.emplace_back(key, dir);
    // slot 0 alone under every mask (CRC checks of one decode) before any
    // ARQ_DUP pair (a decode per mask)
    for (const bool paired : {false, true}) {
        if (paired && rx.n_cw() < 2) break;
        for (const auto& [key, dir] : masks) {
            const auto first = paired ? pair(rx, 0, ctl_mask(dir, 0, key)) : rx.decode_plain(0, ctl_mask(dir, 0, key));
            if (!first) continue;
            const Core c0 = Core::unpack(*first);
            const int dup = c0.ftype == ARQ_DUP ? 2 : 1;
            if (paired && dup == 1) continue;  // combined as a pair, but not sent as one
            if (key) learn(key);
            const std::string kind = key ? "key " + hex4(key) + " " + who(key, dir) : "mask 0";
            if (dup * c0.n_ctl > rx.n_cw())
                throw std::invalid_argument(kind + ": " + std::to_string(c0.n_ctl) + " control codewords x" +
                                            std::to_string(dup) + " in a burst of " + std::to_string(rx.n_cw()));
            std::vector<Bytes> payloads{*first};
            for (int i = 1; i < c0.n_ctl; ++i) {
                auto p = rx.decode_plain(dup * i, ctl_mask(dir, i, key));
                if (!p && dup == 2) p = pair(rx, 2 * i, ctl_mask(dir, i, key));
                if (!p) {
                    d.head += "  " + kind + ": control codeword " + std::to_string(i + 1) + " of " +
                              std::to_string(c0.n_ctl) + " lost, burst not read\n";
                    return true;
                }
                payloads.push_back(std::move(*p));
            }
            const Control ctl = Control::unpack(payloads);
            if (c0.ftype == SESSION) {
                if (const auto it = ctl.ext.find(T_SESS); it != ctl.ext.end()) session_frame(it->second, key, dir, d);
                else if (const auto id = ctl.ext.find(T_ID); id != ctl.ext.end() && id->second.size() >= 10) {
                    const auto [call, k] = unpack_id(id->second);
                    learn(k, {}, {}, call);
                    d.head += "  ID " + call + " (session key " + hex4(k) + ")\n";
                } else if (const auto cq = ctl.ext.find(T_CQ); cq != ctl.ext.end() && cq->second.size() >= 9) {
                    const auto [call, cap] = unpack_cq(cq->second);
                    d.head += "  CQ " + call + " bw " + std::to_string(cap) + "\n";
                } else {
                    d.head += "  session frame (" + kind + "), no body: " + ext_desc(ctl.ext) + "\n";
                }
            } else if (key) {
                arq_burst(rx, ctl, key, dir, dup, d);
            } else {
                d.head += "  frame type " + std::to_string(c0.ftype) + " under mask 0: " + ext_desc(ctl.ext) + "\n";
            }
            return true;
        }
    }
    return false;
}

void Monitor::session_frame(const Bytes& body, int key, int dir, Dump& d) {
    const int sub = at(body, 0);
    std::string s;
    if (sub == CONNECT && body.size() < Connect::BYTES) {  // another VERSION's: not ours to decode
        d.head += "  CONNECT of " + std::to_string(body.size()) + " B, not read\n";
        return;
    }
    if (sub == CONNECT) {
        const Connect c = Connect::unpack(body);
        const std::string &caller = c.caller, &callee = c.callee;
        const int nonce = static_cast<int>(c.nonce);
        const int k = session_key(caller, callee, nonce);
        connects_[nonce] = {caller, callee};
        if (connects_.size() > MAX_KEYS) connects_.erase(connects_.begin());  // ponytail: by nonce, not age
        learn(k, caller, callee);
        for (int dir : {0, 1})  // a retry, or a late copy, leaves a stream under way alone
            if (Side& x = sides_[{k, dir}]; !x.started) x.fresh = true;
        char t[96];
        std::snprintf(t, sizeof t, " v%d nonce %04x cap %d t_turn %.1f s -> key %04x", c.version, nonce, c.cap,
                      c.t_turn10 / 10.0, k);
        s = "CONNECT " + caller + ">" + callee + t;
    } else if (sub == CONNECT_ACK || sub == CONNECT_NAK) {
        const int nonce = be16(body, 1);
        const auto it = connects_.find(nonce);
        s = std::string(sub == CONNECT_ACK ? "CONNECT_ACK " : "CONNECT_NAK ") +
            (it == connects_.end() ? "?>?" : it->second.second + ">" + it->second.first) + " nonce " + hex4(nonce) +
            (sub == CONNECT_ACK ? " cap " : " reason ") + std::to_string(at(body, 3));
    } else if (sub == DISC || sub == DISC_ACK) {
        s = std::string(sub == DISC ? "DISC " : "DISC_ACK ") + who(key, dir) + " key " + hex4(key);
    } else {
        s = "session frame " + std::to_string(sub);
    }
    d.head += "  " + s + "\n";
}

void Monitor::arq_burst(PlainRx& rx, const Control& ctl, int key, int dir, int dup, Dump& d) {
    const Core& c = ctl.core;
    const Ext& ext = ctl.ext;
    static const char* hints[] = {"shrink", "hold", "grow", "max"};
    const Mode* rec = decode(c.recommend);
    char t[200];
    std::snprintf(t, sizeof t, "  %s %s key %04x  b%d acted %d cum %d K %d rec %s %s%s\n",
                  c.ftype == PROBE ? "POLL" : c.ftype == ARQ_DUP ? "ARQ (dup ctl)" : "ARQ", who(key, dir).c_str(), key,
                  c.burst_seq, c.acted_on, c.cum, c.k, rec ? std::string(rec->name).c_str() : "?", hints[c.size_hint],
                  c.reply_lost ? " reply-lost" : "");
    d.head += t;
    if (!ext.empty()) d.head += "  ext: " + ext_desc(ext) + "\n";

    Side& s = sides_[{key, dir}];
    Side& o = sides_[{key, 1 - dir}];
    // this station's ACK: what it holds of the other's stream
    std::set<int> got;
    if (const auto b = ext.find(T_BITMAP); b != ext.end()) got = unpack_bitmap(b->second, c.cum);
    s.snaps[c.burst_seq] = {c.cum, got};
    if (!o.started) {
        start(o, c.cum);
    } else if (const std::int64_t cum = unwrap(c.cum, o.rx.cum); cum > o.rx.cum) {
        skip(o, key, 1 - dir, cum, d);  // its receiver has codewords we never heard
    }
    flush(o, key, 1 - dir, d);

    if (const auto e = ext.find(T_ABANDON); e != ext.end() && e->second.size() >= 2) {
        const int epoch = e->second[1] >> 1;
        if (!s.epoch_known || epoch != s.epoch) {
            s.epoch = epoch;
            s.epoch_known = true;
            if (s.started) {
                const std::int64_t a = unwrap(e->second[0], s.rx.cum);
                // below cum: we heard codewords its receiver missed, and they come again re-sliced
                s.rx.abandon(a);
                s.soft.clear();
                if (a != s.rx.cum) skip(s, key, dir, a, d);
            }
        }
    }

    // the slots: K resends of what the other's acted-on ACK lacked, then new from T_NEW
    const int ctl_slots = dup * c.n_ctl;
    const int n_data = rx.n_cw() - ctl_slots;
    if (c.k > n_data) throw std::invalid_argument("K " + std::to_string(c.k) + " past the burst");
    std::vector<std::optional<int>> seqs;
    if (const auto snap = o.snaps.find(c.acted_on); c.k && snap != o.snaps.end()) {
        const auto& [cum, held] = snap->second;
        for (int q = cum; static_cast<int>(seqs.size()) < c.k; ++q)
            if (!held.count(q % SEQ_MOD)) seqs.push_back(q % SEQ_MOD);
    } else {
        seqs.resize(static_cast<std::size_t>(c.k));
    }
    const auto e_new = ext.find(T_NEW);
    for (int j = 0; j < n_data - c.k; ++j)
        seqs.push_back(e_new != ext.end() && !e_new->second.empty() ? std::optional((e_new->second[0] + j) % SEQ_MOD) : std::nullopt);
    const auto e_rv = ext.find(T_RV);
    const auto rvs = unpack_rv(e_rv == ext.end() ? ByteView{} : ByteView(e_rv->second), c.k);
    const auto e_comp = ext.find(T_COMP);
    const auto bits = unpack_flags(e_comp == ext.end() ? ByteView{} : ByteView(e_comp->second), n_data);

    std::string states;
    for (int j = 0; j < n_data; ++j) {
        const int slot = ctl_slots + j;
        const int rv = j < c.k ? rvs[static_cast<std::size_t>(j)] : 0;
        std::string st = j < c.k ? "r" : "";
        if (!seqs[j]) {
            states += " ?" + st;
            continue;
        }
        const int s7 = *seqs[j];
        std::int64_t seq = s.started ? unwrap(s7, s.rx.cum) : s7;
        if (bits[j] && (!s.started || seq >= s.rx.cum)) s.rx.comp_seqs.insert(seq);
        const bool hint = bits[j] || s.rx.comp_seqs.count(seq);
        st = std::to_string(s7) + st;
        if (s.started && seq < s.rx.cum) {
            states += " " + st + " had";
            continue;
        }
        std::optional<Bytes> p;
        bool comp = hint;
        if (!s.epoch_known && rv == 0) {
            // joined mid-session: the sender's abandon epoch is in every data CRC
            for (int e = 0; e < EPOCH_MOD && !p; ++e)
                for (const bool z : {hint, !hint})
                    if ((p = rx.decode_plain(slot, data_mask(dir, s7, key, z, e)))) {
                        s.epoch = e, s.epoch_known = true, comp = z;
                        break;
                    }
        } else if (s.epoch_known) {
            auto mask = [&](bool z) { return data_mask(dir, s7, key, z, s.epoch); };
            const auto it = s.soft.find(seq);
            if (rv == 0 && it == s.soft.end())  // one decode, checked under either compression
                for (const bool z : {hint, !hint})
                    if ((p = rx.decode_plain(slot, mask(z)))) {
                        comp = z;
                        break;
                    }
            if (!p) {
                // combined with what earlier sends left (IR), and kept for the next resend
                std::optional<SoftEntry> stored;
                if (it != s.soft.end()) stored = it->second;
                p = rx.decode_stored(slot, mask(hint), rv, stored);  // an entry from an older mode is dropped
                if (!p && stored) s.soft[seq] = std::move(*stored);
            }
            if (p) s.soft.erase(seq);
        }
        if (!p) {
            states += " " + st + " lost";
            continue;
        }
        states += " " + st + (comp ? " ok(z)" : " ok");
        if (!s.started) start(s, seq);
        deliver(s, key, dir, seq, std::move(*p), comp, d);
    }
    // a codeword in the control's padding (T_INLINE): seq | comp, payload, its identity's check
    if (const auto e = ext.find(T_INLINE); e != ext.end() && e->second.size() >= 3) {
        const Bytes& b = e->second;
        const int s7 = b[0] & 0x7F;
        const bool comp = b[0] >> 7;
        const std::int64_t seq = s.started ? unwrap(s7, s.rx.cum) : s7;
        Bytes p(b.begin() + 1, b.end() - 2);
        p.resize(static_cast<std::size_t>(payload_bytes(mode_at(rx.submode()))), 0);
        const Bytes chk(b.end() - 2, b.end());
        auto ok = [&](int ep) { return inline_check(data_mask(dir, s7, key, comp, ep), p) == chk; };
        std::string st = " " + std::to_string(s7) + "i";
        if (!s.epoch_known)  // joined mid-session: the epoch is in the check, as in every data CRC
            for (int ep = 0; ep < EPOCH_MOD && !s.epoch_known; ++ep)
                if (ok(ep)) s.epoch = ep, s.epoch_known = true;
        if (s.started && seq < s.rx.cum) {
            states += st + " had";
        } else if (!s.epoch_known || !ok(s.epoch)) {
            states += st + " lost";
        } else {
            states += st + (comp ? " ok(z)" : " ok");
            if (!s.started) start(s, seq);
            deliver(s, key, dir, seq, std::move(p), comp, d);
        }
    }
    if (!states.empty()) d.head += "  data:" + states + "\n";
    flush(s, key, dir, d);
}

void Monitor::start(Side& s, std::int64_t cum) {
    s.rx.cum = cum;
    s.started = true;
    if (!(s.fresh && cum == 0)) forget(s);  // joined late: what went before is unknown
}

void Monitor::forget(Side& s) {
    s.hist.assign(HIST, 0);
    s.unk.assign(HIST, true);
    s.zdict_sure = false;
    s.framed = false;
    s.left = 0;
}

void Monitor::take(Side& s, const Bytes& plain, const std::vector<bool>& unknown) {
    s.rx.plain += static_cast<std::int64_t>(plain.size());
    s.hist.insert(s.hist.end(), plain.begin(), plain.end());
    s.unk.insert(s.unk.end(), unknown.begin(), unknown.end());
    if (s.hist.size() > HIST) {
        s.hist.erase(s.hist.begin(), s.hist.end() - HIST);
        s.unk.erase(s.unk.begin(), s.unk.end() - HIST);
    }
    // records: [length 1-255][bytes], a zero byte is padding (docs/arq.md §9a)
    auto show = [&](std::size_t i) {
        s.out.bytes.push_back(plain[i]);
        s.out.unknown.push_back(unknown[i]);
        s.raw |= !s.framed;
    };
    for (std::size_t i = 0; i < plain.size(); ++i) {
        if (!s.framed) {
            show(i);
        } else if (s.left) {
            show(i);
            --s.left;
        } else if (unknown[i]) {  // a length never heard: framing lost
            s.framed = false;
            show(i);
        } else {
            s.left = plain[i];
        }
    }
    // zero padding ends a burst's last codeword, between records: the next starts one
    // ponytail: binary whose record ends a codeword in a zero byte reframes wrongly, until the next padding
    if (!s.framed && !plain.empty() && plain.back() == 0 && !unknown.back()) s.framed = true, s.left = 0;
}

void Monitor::deliver(Side& s, int key, int dir, std::int64_t seq, Bytes p, bool comp, Dump& d) {
    RxSide& r = s.rx;
    if (seq < r.cum || r.buf.count(seq) || seq >= r.cum + WINDOW) return;
    r.buf[seq] = {std::move(p), comp};
    for (auto it = r.buf.find(r.cum); it != r.buf.end(); it = r.buf.find(r.cum)) {
        auto [q, z] = std::move(it->second);
        r.buf.erase(it);
        r.comp_seqs.erase(r.cum);
        ++r.cum;
        if (!z) {
            take(s, q, std::vector<bool>(q.size(), false));
            continue;
        }
        // Inflated twice, the history's unknown bytes 0x00, then 0xFF: an
        // output byte copied from one differs, every other is exact. With
        // the sender's history length unknown (fewer than HIST bytes
        // delivered in all), ZDICT's place is too: a prefix stands in for it.
        const bool sure = s.zdict_sure || r.plain >= HIST;
        auto filled = [&](std::uint8_t f) {
            Bytes h(sure ? 0 : tables::ZDICT.size(), f);
            for (std::size_t i = 0; i < s.hist.size(); ++i) h.push_back(s.unk[i] ? f : s.hist[i]);
            return h;
        };
        try {
            const Bytes a = inflate(filled(0x00), q), b = inflate(filled(0xFF), q);
            if (a.size() != b.size()) throw std::invalid_argument("inflate: length depends on the history");  // can't: lengths are coded
            std::vector<bool> unknown(a.size());
            for (std::size_t i = 0; i < a.size(); ++i) unknown[i] = a[i] != b[i];
            take(s, a, unknown);
        } catch (const std::invalid_argument& e) {  // its length unknown: what follows has no history
            d.head += "  " + who(key, dir) + " stream: seq " + std::to_string((r.cum - 1) % SEQ_MOD) + ": " + e.what() + "\n";
            forget(s);
        }
    }
}

void Monitor::skip(Side& s, int key, int dir, std::int64_t to, Dump& d) {
    RxSide& r = s.rx;
    d.head += "  " + who(key, dir) + " stream: " +
              (to > r.cum ? "seq " + std::to_string(r.cum % SEQ_MOD) + "-" + std::to_string((to - 1) % SEQ_MOD) + " not all heard"
                          : "re-sliced below what was shown (seq " + std::to_string(to % SEQ_MOD) + ")") +
              "; bytes copied from it show as <?" "?>\n";
    forget(s);
    if (to < r.cum) {
        r.cum = to;
        r.buf.clear();
    }
    // step over what was never heard, delivering what is held
    while (r.cum < to) {
        const auto it = r.buf.lower_bound(r.cum);
        if (it == r.buf.end() || it->first >= to) {
            r.cum = to;
            break;
        }
        r.cum = it->first;
        auto [p, z] = std::move(it->second);
        r.buf.erase(it);
        deliver(s, key, dir, r.cum, std::move(p), z, d);
    }
    if (const auto it = r.buf.find(r.cum); it != r.buf.end()) {  // held right at the new cum
        auto [p, z] = std::move(it->second);
        r.buf.erase(it);
        deliver(s, key, dir, r.cum, std::move(p), z, d);
    }
    for (auto it = s.soft.begin(); it != s.soft.end();) it = it->first < r.cum ? s.soft.erase(it) : std::next(it);
}

void Monitor::flush(Side& s, int key, int dir, Dump& d) {
    if (s.out.bytes.empty()) return;
    s.out.label = who(key, dir) + " stream" + (s.raw ? ", framing lost (record lengths inline)" : "");
    d.payloads.push_back(std::move(s.out));
    s.out = {};
    s.raw = false;
}

std::vector<Dump> Monitor::feed(std::span<const double> x, const std::function<std::string(double t)>& when) {
    if (!receiver_) receiver_ = std::make_unique<receiver::Receiver>(receive_accept(), all_grids());
    std::vector<Dump> out;
    for (auto& ev : receiver_->feed(x)) {
        auto* b = std::get_if<receiver::BurstEvent>(&ev);
        if (!b) continue;
        BurstHeard h;
        h.t = static_cast<double>(b->header.start()) / config::FS;
        h.submode = spec_name(b->header);
        h.n_cw = b->header.n_cw();
        h.lost = !b->rx;
        if (b->rx) {
            h.heard = heard_of(std::move(*b->rx));
            h.snr_db = measure(h.heard).snr_est;
            h.soft = soft_bits(h.heard);
        }
        out.push_back(burst(h, when(h.t)));
    }
    return out;
}

}  // namespace data2g::monitor
