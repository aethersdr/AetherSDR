#include "arq/link.hpp"
#include "arq/modes.hpp"

#include <algorithm>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <tuple>
#include <utility>

namespace data2g::arq {

// --- logging ------------------------------------------------------------------------

namespace {
LogSink g_sink;
constexpr const char* LOG = "data2g.link";
constexpr int INFO = 20, WARNING = 30;

std::string join(const std::vector<std::string>& parts, const char* sep) {
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) out += (i ? sep : "") + parts[i];
    return out;
}

template <typename T>
std::string str(T v) {
    return std::to_string(v);
}
}  // namespace

void set_log_sink(LogSink sink) { g_sink = std::move(sink); }
bool log_enabled(const char* logger, int level) { return g_sink.enabled && g_sink.write && g_sink.enabled(logger, level); }
void log_write(const char* logger, int level, const std::string& msg) {
    if (log_enabled(logger, level)) g_sink.write(logger, level, msg);
}

std::string format(const char* fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    const int n = std::vsnprintf(nullptr, 0, fmt, ap);
    va_end(ap);
    std::string out(static_cast<std::size_t>(std::max(n, 0)), '\0');
    std::vsnprintf(out.data(), out.size() + 1, fmt, ap2);
    va_end(ap2);
    return out;
}

// --- the two directions -------------------------------------------------------------

void TxSide::write(ByteView data) {
    const Bytes r = to_records(data);
    for (std::int64_t x = end(); x < end() + static_cast<std::int64_t>(r.size()); x += 256) recs.push_back(x);
    for (std::int64_t x = written; x < written + static_cast<std::int64_t>(data.size()); x += 255) rec_host.push_back(x);
    written += static_cast<std::int64_t>(data.size());
    buf.insert(buf.end(), r.begin(), r.end());
}

bool TxSide::pending() const { return !cws.empty() || unsent() > 0; }

bool TxSide::on_ack(std::int64_t cum, const std::set<std::int64_t>& received) {
    if (!(base <= cum && cum <= next))
        throw ProtocolError("peer cumulative " + str(cum) + " outside [" + str(base) + ", " + str(next) + "]");
    const bool advanced = cum > base;
    for (std::int64_t s = base; s < cum; ++s) {
        auto it = cws.find(s);
        if (it == cws.end()) continue;
        const Codeword& c = it->second;
        acked += c.length;
        acked_wire += static_cast<std::int64_t>(c.payload.size());
        acked_plain += c.comp ? c.length : static_cast<std::int64_t>(c.payload.size());
        cws.erase(it);
    }
    base = cum;
    std::set<std::int64_t> held;
    for (auto s : received)
        if (cum < s && s < next) held.insert(s);
    ack = std::make_pair(cum, std::move(held));
    std::int64_t keep = stream_end;
    if (!cws.empty()) {
        keep = cws.begin()->second.start;
        for (const auto& [_, c] : cws) keep = std::min(keep, c.start);
    }
    buf.erase(buf.begin(), buf.begin() + std::clamp<std::int64_t>(keep - buf_off, 0, static_cast<std::int64_t>(buf.size())));
    buf_off = keep;
    if (auto it = std::upper_bound(recs.begin(), recs.end(), keep); it != recs.begin()) {
        const auto n = it - 1 - recs.begin();
        recs.erase(recs.begin(), recs.begin() + n);
        rec_host.erase(rec_host.begin(), rec_host.begin() + n);
    }
    keep = hist_off + static_cast<std::int64_t>(hist.size());
    if (!cws.empty()) {
        keep = cws.begin()->second.cstart;
        for (const auto& [_, c] : cws) keep = std::min(keep, c.cstart);
    }
    keep -= HIST;
    if (keep > hist_off) {
        hist.erase(hist.begin(), hist.begin() + std::min<std::int64_t>(keep - hist_off, static_cast<std::int64_t>(hist.size())));
        hist_off = keep;
    }
    if (limbo && limbo_start(std::get<2>(*limbo)) == std::get<2>(*limbo)) settle(std::get<2>(*limbo));  // all of it acked
    return advanced;
}

Codeword& TxSide::new_codeword(const std::string& submode, int pb, bool compress, std::int64_t bn) {
    const auto i = static_cast<std::size_t>(stream_end - buf_off);
    auto n = static_cast<std::int64_t>(std::min<std::size_t>(static_cast<std::size_t>(pb), buf.size() - i));
    Bytes payload(buf.begin() + static_cast<std::ptrdiff_t>(i), buf.begin() + static_cast<std::ptrdiff_t>(i + n));
    payload.resize(static_cast<std::size_t>(pb), 0);
    bool comp = false;
    if (compress) {
        const std::size_t h0 = hist.size() > HIST ? hist.size() - HIST : 0;
        const std::size_t end = std::min(buf.size(), i + 16 * static_cast<std::size_t>(pb));
        auto fit = deflate_fit(ByteView(hist).subspan(h0), ByteView(buf).subspan(i, end - i), pb);
        if (fit) {
            n = fit->first;
            payload = std::move(fit->second);
            payload.resize(static_cast<std::size_t>(pb), 0);
            comp = true;
        }
    }
    Codeword c;
    c.seq = next;
    c.start = stream_end;
    c.length = n;
    c.submode = submode;
    c.payload = payload;
    c.comp = comp;
    c.cstart = hist_off + static_cast<std::int64_t>(hist.size());
    c.first_bn = bn;
    if (comp)
        hist.insert(hist.end(), buf.begin() + static_cast<std::ptrdiff_t>(i), buf.begin() + static_cast<std::ptrdiff_t>(i + n));
    else
        hist.insert(hist.end(), payload.begin(), payload.end());
    auto& ref = cws[c.seq] = std::move(c);
    next += 1;
    stream_end += n;
    return ref;
}

std::vector<std::int64_t> TxSide::missing() const {
    std::vector<std::int64_t> out;
    if (!ack) return out;
    for (std::int64_t s = ack->first; s < next; ++s)
        if (!ack->second.count(s)) out.push_back(s);
    return out;
}

std::int64_t TxSide::abandon() {
    const std::int64_t a = base;
    const std::optional<std::int64_t> lo = limbo ? std::optional(limbo_start(std::get<2>(*limbo))) : std::nullopt;
    if (auto it = cws.find(a); it != cws.end()) {
        stream_end = it->second.start;
        hist.resize(static_cast<std::size_t>(std::clamp<std::int64_t>(it->second.cstart - hist_off, 0,
                                                                      static_cast<std::int64_t>(hist.size()))));
    }
    cws.clear();
    next = a;
    ack.reset();
    if (lo) settle(*lo);  // abandons are made against an exact ACK: nothing from the base on arrived
    return a;
}

std::optional<std::tuple<std::int64_t, std::int64_t, std::int64_t, std::int64_t>> TxSide::clean(
    std::optional<std::int64_t> upto) {
    const std::int64_t c = rec_at(stream_end), drop = hoff(c);
    if (upto && *upto < drop) return std::nullopt;
    const std::int64_t drop_to = upto ? std::min(*upto, written) : written;
    cut(c, split(drop_to));
    if (!limbo)
        if (const std::int64_t lo = limbo_start(c); lo < c) limbo = Offsets3{hoff(lo), drop, c};
    mark = c;
    const auto [lf, lt, _] = limbo.value_or(Offsets3{drop, drop, 0});
    return std::make_tuple(drop, drop_to, lf, lt);
}

std::optional<TxSide::Offsets3> TxSide::end_limbo() {
    if (!limbo) return std::nullopt;
    const auto [f, t, _] = *limbo;
    auto it = cws.find(base);
    const std::int64_t acked = it != cws.end() ? it->second.start : stream_end;
    const auto i = std::upper_bound(recs.begin(), recs.end(), acked) - recs.begin() - 1;
    limbo.reset();
    return Offsets3{f, std::max(f, rec_host[static_cast<std::size_t>(i)]), t};
}

std::int64_t TxSide::rec_at(std::int64_t x) const {
    auto it = std::lower_bound(recs.begin(), recs.end(), x);
    return it != recs.end() ? *it : end();
}

std::int64_t TxSide::hoff(std::int64_t x) const {
    const auto i = std::lower_bound(recs.begin(), recs.end(), x) - recs.begin();
    return i < static_cast<std::ptrdiff_t>(recs.size()) ? rec_host[static_cast<std::size_t>(i)] : written;
}

std::int64_t TxSide::split(std::int64_t h) {
    if (h >= written) return end();
    const auto i = static_cast<std::size_t>(std::upper_bound(rec_host.begin(), rec_host.end(), h) - rec_host.begin() - 1);
    if (rec_host[i] == h) return recs[i];
    const std::int64_t s = recs[i], k = h - rec_host[i];
    const auto j = static_cast<std::size_t>(s - buf_off);
    const std::uint8_t n = buf[j];
    buf[j] = static_cast<std::uint8_t>(k);
    buf.insert(buf.begin() + static_cast<std::ptrdiff_t>(j + 1 + static_cast<std::size_t>(k)), static_cast<std::uint8_t>(n - k));
    for (auto r = recs.begin() + static_cast<std::ptrdiff_t>(i) + 1; r != recs.end(); ++r) *r += 1;
    recs.insert(recs.begin() + static_cast<std::ptrdiff_t>(i) + 1, s + 1 + k);
    rec_host.insert(rec_host.begin() + static_cast<std::ptrdiff_t>(i) + 1, h);
    return s + 1 + k;
}

void TxSide::cut(std::int64_t lo, std::int64_t hi) {  // host offsets stay
    buf.erase(buf.begin() + (lo - buf_off), buf.begin() + (hi - buf_off));
    const auto i = std::lower_bound(recs.begin(), recs.end(), lo) - recs.begin();
    const auto j = std::lower_bound(recs.begin(), recs.end(), hi) - recs.begin();
    for (auto k = recs.begin() + j; k != recs.end(); ++k) *k -= hi - lo;
    recs.erase(recs.begin() + i, recs.begin() + j);
    rec_host.erase(rec_host.begin() + i, rec_host.begin() + j);
}

std::int64_t TxSide::limbo_start(std::int64_t c) const {
    auto it = cws.find(base);
    return it != cws.end() ? std::min(rec_at(it->second.start), c) : c;
}

void TxSide::settle(std::int64_t lo) {
    const auto [f, t, c] = *limbo;
    const std::int64_t split = lo < c ? hoff(lo) : t;
    cut(lo, c);
    mark -= c - lo;
    limbo.reset();
    cleaned = Offsets3{f, split, t};
}

bool RxSide::accept(std::int64_t seq, Bytes payload, bool comp) {
    if (seq < cum || buf.count(seq) || seq >= cum + WINDOW) return false;
    buf[seq] = {std::move(payload), comp};
    bool delivered = false;
    while (true) {
        auto it = buf.find(cum);
        if (it == buf.end()) break;
        auto [p, z] = std::move(it->second);
        buf.erase(it);
        comp_seqs.erase(cum);
        wire += static_cast<std::int64_t>(p.size());
        if (z) {
            try {
                p = inflate(hist, p);
            } catch (const std::invalid_argument& e) {
                throw ProtocolError("seq " + str(cum) + ": " + e.what());
            }
        }
        plain += static_cast<std::int64_t>(p.size());
        hist.insert(hist.end(), p.begin(), p.end());
        if (hist.size() > HIST) hist.erase(hist.begin(), hist.end() - HIST);
        const Bytes got = reader.feed(p);
        out.insert(out.end(), got.begin(), got.end());
        cum += 1;
        delivered = true;
    }
    return delivered;
}

std::vector<std::int64_t> RxSide::abandon(std::int64_t a) {
    const std::int64_t lo = std::max(a, cum);
    std::vector<std::int64_t> gone;
    for (auto it = buf.lower_bound(lo); it != buf.end();) {
        gone.push_back(it->first);
        it = buf.erase(it);
    }
    comp_seqs.erase(comp_seqs.lower_bound(lo), comp_seqs.end());
    return gone;
}

// --- one station ------------------------------------------------------------------------

namespace {
bool env_on(const char* name) {
    const char* v = std::getenv(name);
    return !(v && std::strcmp(v, "0") == 0);
}

constexpr int OPTIONAL_TLVS[] = {T_BUFFER, T_CHAT, T_REPLY, T_DUPCTL};

// Control bytes with T_RV, T_NEW and a T_COMP covering the first zk data
// slots, as they will be packed.
std::int64_t ctl_bytes(Ext e, int k, bool has_new, int zk) {
    if (k) e[T_RV] = Bytes(static_cast<std::size_t>(ceil_div(2 * k, 8)));
    if (has_new) e[T_NEW] = Bytes(1);
    if (zk) e[T_COMP] = Bytes(static_cast<std::size_t>(ceil_div(zk, 8)));
    std::int64_t n = 4;
    for (const auto& [_, v] : e) n += 2 + static_cast<std::int64_t>(v.size());
    return n;
}

int ctl_size(const Ext& e, int pb, int k, bool has_new, int zk = 0) {
    return static_cast<int>(std::max<std::int64_t>(1, ceil_div(ctl_bytes(e, k, has_new, zk), pb)));
}
}  // namespace

Station::Station(int direction_, std::shared_ptr<Policy> policy_, bool master_, int key_, std::optional<int> max_misses_,
                 int cap_, bool chat_)
    : direction(direction_), policy(std::move(policy_)), master(master_), key(key_), max_misses(max_misses_), cap(cap_),
      chat(chat_), compress(env_on("DATA2G_COMPRESS")), inline_data(env_on("DATA2G_INLINE")), prime(env_on("DATA2G_PRIME")) {}

bool repeat_default() {
    static const bool on = env_on("DATA2G_REPEAT");
    return on;
}

bool nod_default() {
    static const bool on = env_on("DATA2G_NOD");
    return on;
}

bool prime_default() {
    static const bool on = env_on("DATA2G_PRIME");
    return on;
}

int prime_tries() {
    static const int n = [] {
        const char* v = std::getenv("DATA2G_PRIME_TRIES");
        return v ? std::atoi(v) : 6;
    }();
    return n;
}

Bytes Station::read() {
    Bytes out = std::move(rx.out);
    rx.out.clear();
    return out;
}

std::string Station::mode(std::optional<int> rec) { return rec ? policy->mode_name(*rec) : "-"; }

std::string Station::burst_desc(const std::string& submode, int n_cw, bool dup) {
    std::string out = submode + " x" + str(n_cw);
    if (policy->has_airtime()) out += format(" %.1fs", policy->airtime(submode, n_cw, dup));
    return out;
}

std::string Station::snr() {
    auto s = policy->snr_est();
    return s ? format(" | snr %.1f dB", *s) + policy->measured_extra() : "";
}

bool Station::comp_bit(std::int64_t seq) const {
    const Codeword& c = tx.cws.at(seq);
    return c.comp && !c.comp_known;
}

TxBurstPtr Station::build(bool fresh, bool nod_ok) {
    if (latest - confirmed >= BURST_MOD - 1 && last_sent) {
        // 7 of my bursts unconfirmed: repeat the latest instead
        log_write(LOG, INFO, format("TX b%d repeat: 7 bursts unconfirmed", static_cast<int>(pmod(latest, BURST_MOD))));
        auto it = sent_seqs.find(latest);
        stats["cw_resend"] += it == sent_seqs.end() ? 0 : static_cast<std::int64_t>(it->second.size());
        repeated_ = true;
        return last_sent;
    }
    const std::int64_t bn = latest + 1;
    const int esc = sent_esc_ = escalation();
    auto [submode, max_cw] = policy->choose(*this, esc);
    if (!data_allowed(fresh, submode)) max_cw = 0;  // control only
    const auto r = policy->recommend(*this);
    const int rec = r ? r->rec : 0, hint = r ? r->hint : 1;
    const std::optional<int> reply = r ? r->reply : std::nullopt;
    asked_ = {rec, reply};
    Ext ext = control_tlvs(bn, reply);
    Core core;
    core.ftype = fresh ? ARQ : PROBE;
    core.burst_seq = static_cast<int>(pmod(bn, BURST_MOD));
    core.acted_on = acted_on_;
    core.cum = static_cast<int>(pmod(rx.cum, SEQ_MOD));
    core.reply_lost = reply_lost;
    core.recommend = rec;
    core.size_hint = hint;
    reply_lost = false;

    const int cpb = policy->ctl_payload_bytes(submode);
    const std::vector<std::int64_t> missing = fresh ? tx.missing() : std::vector<std::int64_t>{};
    auto [k, has_new, bitmap, n_ctl, dup] = fit(ext, core.cum, missing, fresh && new_available() > 0, max_cw, submode, fresh);
    // what this burst's ACK conveys, exactly (the snapshot must match it)
    if (!bitmap.empty()) ext[T_BITMAP] = bitmap;
    std::set<std::int64_t> conveyed;
    if (!bitmap.empty())
        for (int x : unpack_bitmap(bitmap, core.cum)) conveyed.insert(unwrap(x, rx.cum));

    const std::vector<std::int64_t> resend(missing.begin(), missing.begin() + k);
    const int cycle = policy->rv_cycle(submode);
    std::vector<int> rvs;
    for (auto x : resend) rvs.push_back(static_cast<int>(pmod(tx.cws.at(x).heard, cycle)));
    const int room = has_new ? std::max(0, max_cw - dup * n_ctl - static_cast<int>(resend.size())) : 0;
    // a new codeword is compressed only when its T_COMP bit fits the
    // control's padding: compression never costs a control codeword
    const std::int64_t spare = static_cast<std::int64_t>(n_ctl) * cpb - ctl_bytes(ext, k, has_new, 0);
    auto fresh_cws = new_codewords(submode, room, k, spare, bn);
    core.k = static_cast<int>(resend.size());
    Ext data = data_tlvs(resend, rvs, fresh_cws);
    Codeword* inl = nullptr;
    if (inline_data && !fresh_cws.empty() && new_available() <= 0) {
        // the last codeword of a write fits the control's padding: it goes
        // there, one codeword fewer on air (docs/arq.md §5a)
        const Codeword& c = *fresh_cws.back();
        Bytes body{static_cast<std::uint8_t>(pmod(c.seq, SEQ_MOD) | (c.comp ? 0x80 : 0))};
        std::size_t n = c.payload.size();
        while (n && !c.payload[n - 1]) --n;
        body.insert(body.end(), c.payload.begin(), c.payload.begin() + static_cast<std::ptrdiff_t>(n));
        const Bytes chk = inline_check(data_mask(direction, c.seq, key, c.comp, abandon_epoch), c.payload);
        body.insert(body.end(), chk.begin(), chk.end());
        const std::vector<const Codeword*> rest(fresh_cws.begin(), fresh_cws.end() - 1);
        Ext d = data_tlvs(resend, rvs, rest);
        d[T_INLINE] = std::move(body);
        // hints and requests make room for it, as for resends in fit()
        // (CPM's 20 B control: a whole data codeword, 3-10 s, saved)
        Ext kept = ext;
        auto size = [&] {
            Ext e = kept;
            for (const auto& [t, v] : d) e[t] = v;
            return ctl_bytes(e, 0, false, 0);
        };
        const std::int64_t room_b = static_cast<std::int64_t>(n_ctl) * cpb;
        for (int t : OPTIONAL_TLVS)
            if (size() > room_b) kept.erase(t);
        if (d[T_INLINE].size() < 256 && size() <= room_b) {  // a TLV holds 255 B
            inl = &tx.cws.at(c.seq);
            inl->inlined = true;
            fresh_cws.pop_back();
            data = std::move(d);
            ext = std::move(kept);
        }
    }
    for (auto& [t, v] : data) ext[t] = std::move(v);
    if (dup == 2 && resend.empty() && fresh_cws.empty() && !inl) dup = 1;  // nothing but control after all
    if (dup == 2) core.ftype = ARQ_DUP;
    if ((!fresh_cws.empty() || inl) && new_available() <= 0 &&
        ctl_bytes(ext, 0, false, 0) + 2 <= static_cast<std::int64_t>(n_ctl) * cpb) {
        // my host's data ends here: the peer may hold its reply for its
        // host's answer (session REPLY_HOLD_S). Only in the padding, like T_COMP
        ext[T_DRAINED] = Bytes{};
    }
    if (nod_default() && nod && !nods_told_ && ctl_bytes(ext, 0, false, 0) + 2 <= static_cast<std::int64_t>(n_ctl) * cpb)
        ext[T_NODS] = Bytes{};  // I hear nods: in the padding, until the peer acted on it
    Control control{core, ext};
    const auto ctl = control.pack(cpb);
    core.n_ctl = control.core.n_ctl;
    if (core.n_ctl > n_ctl) throw std::logic_error("T_COMP outgrew the control's padding");
    asked_dup_ = ext.count(T_DUPCTL) > 0;

    auto burst = std::make_shared<TxBurst>();
    burst->submode = submode;
    burst->burst_seq = bursts_sent;
    burst->cap = cap;
    add_slots(*burst, ctl, dup, resend, rvs, fresh_cws);
    std::vector<const Codeword*> carried = fresh_cws;
    if (inl) carried.push_back(inl);
    std::optional<std::int64_t> new_end;
    for (const auto* c : carried) new_end = std::max(new_end.value_or(c->seq + 1), c->seq + 1);
    if (nod_ok && fresh && resend.empty() && !new_end && dup == 1 && ctl.size() == 1) burst->nod = nod_of(bn, submode, core, ext);
    if (!burst->nod && ext.count(T_NODS)) nods_in_.insert(bn);
    sent_ctl_[bn] = {rx.cum, core.recommend, core.acted_on, new_end, kept(core, ext)};
    record(bn, burst, std::move(conveyed), resend, carried);
    if (burst->nod) {
        stats["tx_nod"] += 1;
        log_write(LOG, INFO, format("TX b%d nod %d: %s", static_cast<int>(pmod(bn, BURST_MOD)), burst->nod->first,
                                    nod::describe(burst->nod->first).c_str()));
    }
    if (log_enabled(LOG, INFO)) log_tx(bn, *burst, fresh, core, ext, dup, resend, rvs, fresh_cws, esc, hint, reply, inl);
    return burst;
}

// T_RV, T_NEW and T_COMP for these data slots.
Ext Station::data_tlvs(const std::vector<std::int64_t>& resend, const std::vector<int>& rvs,
                       const std::vector<const Codeword*>& fresh_cws) const {
    Ext d;
    if (!resend.empty()) d[T_RV] = pack_rv(rvs);
    if (!fresh_cws.empty()) d[T_NEW] = Bytes{static_cast<std::uint8_t>(pmod(fresh_cws[0]->seq, SEQ_MOD))};
    std::vector<bool> flags;
    for (auto x : resend) flags.push_back(comp_bit(x));
    for (auto* c : fresh_cws) flags.push_back(c->comp);
    if (Bytes comp = pack_flags(flags); !comp.empty()) d[T_COMP] = comp;
    return d;
}

// My own timeouts (master), or the peer telling me my replies are lost (it
// repeats or polls: §6), climbing from the last recovery's floor.
int Station::escalation() const {
    const int raw = std::max(misses, reply_escalation);
    return raw ? std::min(std::max(raw, esc_floor + raw - 1), MAX_ESCALATION) : 0;
}

// Whether this burst may carry data codewords. Abandons the outstanding ones
// first when a resync is due or they are in another submode (§4).
bool Station::data_allowed(bool fresh, const std::string& submode) {
    const bool stale = std::exchange(stale_, false);
    if (!fresh) return false;
    const bool other_mode =
        std::any_of(tx.cws.begin(), tx.cws.end(), [&](const auto& kv) { return kv.second.submode != submode; });
    // CLEANTXBUFFER's limbo: the exact ACK says the rest never arrived (TxSide::abandon drops it)
    const bool in_limbo = tx.limbo.has_value();
    // answering a repeat of the peer burst I last answered: its ACK may
    // predate my last burst, so it may not abandon (§4)
    if (stale && (resync_due || other_mode || in_limbo)) return false;
    if (resync_due) {
        resync_due = false;
        start_abandon(1);
    } else if (other_mode || in_limbo) {
        start_abandon(0);  // a resend must keep its submode (§4)
    }
    return true;
}

// `reset` (the TLV's low bit, and T_RESYNC with it): deprecated, sent and never read (docs/arq.md §10)
void Station::start_abandon(int reset) {
    const std::int64_t a = tx.abandon();
    abandon_epoch = (abandon_epoch + 1) % 128;
    abandon_tlv = Bytes{static_cast<std::uint8_t>(pmod(a, SEQ_MOD)), static_cast<std::uint8_t>(reset | abandon_epoch << 1)};
    abandon_bursts.clear();
}

// The TLVs that do not depend on what data the burst carries.
Ext Station::control_tlvs(std::int64_t bn, std::optional<int> reply) {
    Ext ext;
    if (abandon_tlv) {
        ext[T_ABANDON] = *abandon_tlv;
        abandon_bursts.insert(bn);
        if ((*abandon_tlv)[1] & 1) ext[T_RESYNC] = {};
    }
    if (reply) ext[T_REPLY] = Bytes{static_cast<std::uint8_t>(*reply)};
    if (chat) ext[T_CHAT] = {};
    if (policy->want_dup()) ext[T_DUPCTL] = {};
    if (chat || peer_chat) {
        const std::int64_t queued = new_available();
        if (queued > CHAT_LINE_BYTES) {
            const auto q = std::min<std::int64_t>(queued, 65535);
            ext[T_BUFFER] = Bytes{static_cast<std::uint8_t>(q >> 8), static_cast<std::uint8_t>(q)};
        }
    }
    return ext;
}

// Shrink the burst until control and resends fit: resends first, then
// optional TLVs (erased from `ext`), the bitmap, new data.
Station::Fit Station::fit(Ext& ext, int cum, const std::vector<std::int64_t>& missing, bool has_new, int max_cw,
                          const std::string& submode, bool fresh) {
    const int cpb = policy->ctl_payload_bytes(submode);
    const int max_ctl = policy->max_ctl(submode);
    std::set<int> held7;
    for (const auto& [s, _] : rx.buf) held7.insert(static_cast<int>(pmod(s, SEQ_MOD)));
    Bytes bitmap = pack_bitmap(held7, cum);
    int k = static_cast<int>(std::min<std::int64_t>(static_cast<std::int64_t>(missing.size()), std::max(0, max_cw - 1)));
    while (true) {
        Ext e = ext;
        if (!bitmap.empty()) e[T_BITMAP] = bitmap;
        int zk = 0;
        for (int j = 0; j < k; ++j)
            if (comp_bit(missing[static_cast<std::size_t>(j)])) zk = j + 1;
        const int n_ctl = ctl_size(e, cpb, k, has_new, zk);
        const int dup = (fresh && peer_wants_dup && (k || has_new)) ? 2 : 1;
        if (n_ctl <= max_ctl && dup * n_ctl + k <= std::max(max_cw, dup * n_ctl)) return {k, has_new, bitmap, n_ctl, dup};
        const int* opt = std::find_if(std::begin(OPTIONAL_TLVS), std::end(OPTIONAL_TLVS), [&](int t) { return ext.count(t) > 0; });
        if (k) {
            k -= 1;
        } else if (opt != std::end(OPTIONAL_TLVS)) {
            ext.erase(*opt);  // hints and requests: a later burst carries them
        } else if (!bitmap.empty()) {
            bitmap.clear();  // costs extra resends, never wrong ones
        } else if (has_new) {
            has_new = false;
        } else {
            throw std::invalid_argument("control does not fit " + submode);
        }
    }
}

// Up to `room` new codewords; `spare` control padding bytes decide compression.
std::vector<const Codeword*> Station::new_codewords(const std::string& submode, int room, int k, std::int64_t spare,
                                                    std::int64_t bn) {
    const int pb = policy->payload_bytes(submode);
    std::vector<const Codeword*> out;
    while (static_cast<int>(out.size()) < room && tx.next - tx.base < WINDOW && new_available() > 0) {
        const bool fits = compress && 2 + ceil_div(k + static_cast<std::int64_t>(out.size()) + 1, 8) <= spare;
        out.push_back(&tx.new_codeword(submode, pb, fits, bn));
    }
    return out;
}

void Station::add_slots(TxBurst& burst, const std::vector<Bytes>& ctl, int dup, const std::vector<std::int64_t>& resend,
                        const std::vector<int>& rvs, const std::vector<const Codeword*>& fresh_cws) {
    for (std::size_t i = 0; i < ctl.size(); ++i)
        for (int rv = 0; rv < dup; ++rv) burst.slots.push_back({ctl_mask(direction, static_cast<int>(i), key), rv, ctl[i]});
    // every outstanding codeword was sliced in the current epoch (abandon clears them)
    for (std::size_t j = 0; j < resend.size(); ++j)
        burst.slots.push_back(
            {data_mask(direction, resend[j], key, tx.cws.at(resend[j]).comp, abandon_epoch), rvs[j],
             tx.cws.at(resend[j]).payload});
    for (auto* c : fresh_cws)
        burst.slots.push_back({data_mask(direction, c->seq, key, c->comp, abandon_epoch), 0, c->payload});
}

// Remember what burst `bn` conveyed and carried, for the peer's ACK of it.
void Station::record(std::int64_t bn, const TxBurstPtr& burst, std::set<std::int64_t> conveyed,
                     const std::vector<std::int64_t>& resend, const std::vector<const Codeword*>& fresh_cws) {
    snapshots[bn] = {rx.cum, std::move(conveyed)};
    auto& sent = sent_seqs[bn] = resend;
    for (auto* c : fresh_cws) sent.push_back(c->seq);
    latest = bn;
    repeated_ = false;
    for (auto it = snapshots.begin(); it != snapshots.end() && it->first < confirmed;) {
        sent_seqs.erase(it->first);
        it = snapshots.erase(it);
    }
    for (auto it = sent_ctl_.begin(); it != sent_ctl_.end() && it->first < std::min(confirmed, latest) - 1;)
        it = sent_ctl_.erase(it);
    bursts_sent += 1;
    last_sent = burst;
    std::int64_t n_comp = 0;
    for (auto* c : fresh_cws) n_comp += c->comp;
    stats["cw_new"] += static_cast<std::int64_t>(fresh_cws.size());
    stats["cw_comp"] += n_comp;
    stats["cw_resend"] += static_cast<std::int64_t>(resend.size());
}

void Station::log_tx(std::int64_t bn, const TxBurst& burst, bool fresh, const Core& core, const Ext& ext, int dup,
                     const std::vector<std::int64_t>& resend, const std::vector<int>& rvs,
                     const std::vector<const Codeword*>& fresh_cws, int escalation, int hint, std::optional<int> reply,
                     const Codeword* inl) {
    const char* kind = (!resend.empty() || !fresh_cws.empty() || inl) ? "data" : (fresh ? "ack" : "poll");
    std::vector<std::string> parts{std::string(kind) + " " + burst_desc(burst.submode, static_cast<int>(burst.slots.size()), dup == 2)};
    if (!resend.empty()) {
        std::vector<std::string> rs;
        for (std::size_t j = 0; j < resend.size(); ++j) rs.push_back(str(resend[j]) + "/rv" + str(rvs[j]));
        parts.push_back("resend " + join(rs, " "));
    }
    if (!fresh_cws.empty()) {
        std::int64_t n_comp = 0, n_bytes = 0;
        for (auto* c : fresh_cws) {
            n_comp += c->comp;
            n_bytes += c->length;
        }
        std::string s = "new " + str(fresh_cws.front()->seq);
        if (fresh_cws.size() > 1) s += "-" + str(fresh_cws.back()->seq);
        s += " " + str(n_bytes) + " B";
        if (n_comp) s += " (" + str(n_comp) + " compressed)";
        parts.push_back(s);
    }
    if (inl) parts.push_back("inline " + str(inl->seq) + " " + str(inl->length) + " B" + (inl->comp ? " compressed" : ""));
    if (tx.pending()) parts.push_back("unacked " + str(tx.next - tx.base) + ", queued " + str(new_available()) + " B");
    parts.push_back("ack cum " + str(rx.cum) + (rx.buf.empty() ? "" : " +" + str(rx.buf.size()) + " held"));
    parts.push_back("ask data " + mode(core.recommend) + " size " + str(hint) + ", reply " + mode(reply));
    if (escalation) parts.push_back("escalated " + str(escalation));
    if (misses) parts.push_back("timeout " + str(misses));
    if (ext.count(T_ABANDON)) parts.push_back(std::string(ext.count(T_RESYNC) ? "resync" : "abandon") + " at " + str(tx.base));
    if (dup == 2) parts.push_back("dup ctl");
    if (core.reply_lost) parts.push_back("reply lost");
    log_write(LOG, INFO, format("TX b%d %s", static_cast<int>(pmod(bn, BURST_MOD)), join(parts, " | ").c_str()));
}

// --- nods (docs/arq.md §3b) ---------------------------------------------------------

Station::Kept Station::kept(const Core& core, const Ext& ext) {
    const auto r = ext.find(T_REPLY);
    return {core.size_hint, r != ext.end() && !r->second.empty() ? std::optional<int>(r->second[0]) : std::nullopt,
            ext.count(T_CHAT) > 0};
}

// Control-only reply bn as a nod: (meaning, family key), or nullopt when the peer does not hear nods or its
// content is not in the codebook against anchors the peer holds too (link.Station._nod_of).
std::optional<std::pair<int, nod::FamilyKey>> Station::nod_of(std::int64_t bn, const std::string& submode,
                                                              const Core& core, const Ext& ext) const {
    const Mode* md = arq::mode(submode);
    if (!(nod_default() && nod && nod_peer && md && md->is_cpm() && !core.reply_lost && core.k == 0 && latest >= 0))
        return std::nullopt;
    for (const auto& [t, v] : ext)
        if (t != T_REPLY && t != T_CHAT && t != T_DUPCTL && t != T_NODS) return std::nullopt;
    const auto pa = peer_ctl_core_.find(core.acted_on);
    if (pa == peer_ctl_core_.end() || pa->second.acted != latest) return std::nullopt;
    const SentCtl& r = sent_ctl_.at(latest);
    std::optional<bool> all;
    if (pa->second.new_end && rx.cum == *pa->second.new_end)
        all = true;
    else if (rx.cum == r.cum)
        all = false;
    const int step = (core.recommend & 15) - (r.rec & 15);
    const auto idx = nod::index(all.value_or(false), step, ext.count(T_DUPCTL) > 0);
    if (!all || (core.recommend >> 4) != (r.rec >> 4) || !idx || kept(core, ext) != r.kept) return std::nullopt;
    return std::make_pair(*idx, nod::FamilyKey{key, direction, static_cast<int>(pmod(bn, BURST_MOD)), core.acted_on});
}

std::optional<nod::FamilyKey> Station::nod_family() const {
    const auto mine = sent_ctl_.find(latest);
    if (!(nod_default() && nod) || !peer_burst || mine == sent_ctl_.end()) return std::nullopt;
    return nod::FamilyKey{key, peer(), static_cast<int>(pmod(mine->second.acted + 1, BURST_MOD)),
                          static_cast<int>(pmod(latest, BURST_MOD))};
}

std::optional<Bytes> Station::nod_payload(int meaning) const {
    const auto mine = sent_ctl_.find(latest);
    if (mine == sent_ctl_.end() || meaning < 0 || meaning >= nod::N_MEANINGS) return std::nullopt;
    const auto r = peer_ctl_core_.find(mine->second.acted);
    if (r == peer_ctl_core_.end()) return std::nullopt;
    const nod::Meaning m = nod::meaning(meaning);
    Core core;
    if (m.all) {
        if (!mine->second.new_end) return std::nullopt;
        core.cum = static_cast<int>(pmod(*mine->second.new_end, SEQ_MOD));
    } else {
        core.cum = r->second.cum;
    }
    const int idx = (r->second.rec & 15) + m.step;
    if (idx < 0 || idx >= 16) return std::nullopt;
    core.ftype = ARQ;
    core.burst_seq = static_cast<int>(pmod(mine->second.acted + 1, BURST_MOD));
    core.acted_on = static_cast<int>(pmod(latest, BURST_MOD));
    core.reply_lost = false;
    core.k = 0;
    core.recommend = (r->second.rec & 0x30) | idx;
    core.size_hint = peer_size_hint;
    Ext ext;
    if (m.dup) ext[T_DUPCTL] = Bytes{};
    if (peer_reply_recommend) ext[T_REPLY] = Bytes{static_cast<std::uint8_t>(*peer_reply_recommend)};
    if (peer_chat) ext[T_CHAT] = Bytes{};
    const auto p = Control{core, ext}.pack(policy->ctl_payload_bytes(NOD_MODE));
    if (p.size() != 1) return std::nullopt;
    return p[0];
}

TxBurstPtr Station::on_timeout(bool allow_repeat) {
    if (!master) throw std::logic_error("on_timeout: not the master");
    misses += 1;
    stats["timeouts"] += 1;
    if (max_misses && misses > *max_misses) {
        fail("link lost");
        return nullptr;
    }
    // a nod is not repeated: the peer looks for it in its reply slot alone
    if (allow_repeat && !esc_floor && misses <= REPEATS_BEFORE_SHRINK && last_sent && !last_sent->nod) {
        log_write(LOG, INFO, format("TX b%d repeat: timeout %d", static_cast<int>(pmod(latest, BURST_MOD)), misses));
        auto it = sent_seqs.find(latest);
        stats["cw_resend"] += it == sent_seqs.end() ? 0 : static_cast<std::int64_t>(it->second.size());
        repeated_ = true;
        return last_sent;  // identical, same burst seq (§6 step 1)
    }
    return build(false);
}

bool Station::handle(RxBurst& rx_) {
    const bool ok = handle_inner(rx_);
    stats[ok ? "rx_ok" : "rx_lost"] += 1;
    if (!ok && log_enabled(LOG, INFO))
        log_write(LOG, INFO, "RX " + burst_desc(rx_.submode(), rx_.n_cw()) + snr() + " | control lost, discarded");
    return ok;
}

bool Station::handle_inner(RxBurst& rxb) {
    const bool outcome = policy->has_outcome();
    auto got = read_control(rxb, outcome);
    if (!got) return false;
    const auto& [ctl, dup] = *got;
    const Core& core = ctl.core;
    const Ext& ext = ctl.ext;
    const auto [was, repeat] = note_peer(core, ext);
    const std::int64_t base0 = tx.base, next0 = tx.next, cum0 = rx.cum;
    bool progress = false, abandoned = false;
    std::int64_t acted = 0;
    DataTaken data;
    try {
        // never guess on a protocol error: a blind resync can corrupt
        std::tie(acted, progress) = take_ack(core, ext);
        abandoned = take_abandon(rxb, ext);
        data = take_data(rxb, core, ext, dup, acted, outcome);
    } catch (const ProtocolError& e) {
        fail(std::string("protocol: ") + e.what());
        return true;
    }
    if (log_enabled(LOG, INFO)) log_rx(rxb, core, dup, data, base0, next0, cum0, was, repeat, abandoned);
    reply_lost = repeat;
    stale_ = repeat;
    peer_burst = core.burst_seq;
    peer_ctl_core_[core.burst_seq] = {core.cum, core.recommend, acted, rx_new_end_};
    if (ext.count(T_NODS)) nod_peer = true;  // latched for the session
    if (nods_in_.count(acted)) nods_told_ = true, nods_in_.clear();
    peer_ctl_[!data.slots.empty()] = last_stream_;
    peer_last_ = std::make_pair(last_stream_, std::make_pair(rxb.submode(), rxb.n_cw()));
    answered_ = false;
    watchdog(progress || data.progress);
    return true;
}

// The burst's control and its dup factor; nullopt if lost or malformed. A control codeword that fails blind
// (alone, then as an ARQ_DUP pair) is retried primed with what I expect of it (§3a), then (the first one)
// combined with the failed ones of the bursts before it: a repeat's is the same codeword.
std::optional<std::pair<Control, int>> Station::read_control(RxBurst& rxb, bool outcome) {
    std::optional<Bytes> first = rxb.decode(0, ctl_mask(peer(), 0, key), 0, nullptr);
    bool paired = false, was_primed = false, combined = false;
    const bool repeat = repeat_default() && rxb.can_repeat();
    if (!first && rxb.n_cw() >= 2) {
        // an ARQ_DUP burst's control pair, combined
        first = ctl_pair(rxb, 0, 0);
        paired = first.has_value();
    }
    std::optional<std::vector<Hypothesis>> hyps;
    if (!first && prime && rxb.can_prime()) {
        hyps = expect(rxb);
        auto [p, pair] = primed(rxb, 0, 0, *hyps);
        first = std::move(p);
        paired = pair;
        was_primed = first.has_value();
    }
    if (first) {
        if (repeat) rxb.forget(REPEAT_KEY);  // what failed before this burst is not its repeat
    } else if (repeat) {
        // codeword 0 only (link.py)
        first = rxb.decode_repeat(0, ctl_mask(peer(), 0, key), REPEAT_KEY);
        if (first) {
            rxb.forget(REPEAT_KEY);
            was_primed = combined = true;  // as a primed one: checked against my state, not scored as usable
        }
    }
    if (!first) {
        if (outcome) policy->outcome(rxb.submode(), 0, 0, false);
        return std::nullopt;
    }
    Core core0 = Core::unpack(*first);
    const int dup = core0.ftype == ARQ_DUP ? 2 : 1;
    if (paired && dup == 1) return std::nullopt;  // combined as a pair, but not sent as one
    if (dup * core0.n_ctl > rxb.n_cw()) {
        malformed(format("%d control codewords x%d in a burst of %d", core0.n_ctl, dup, rxb.n_cw()));
        return std::nullopt;
    }
    std::vector<Bytes> payloads{*first};
    for (int i = 1; i < core0.n_ctl; ++i) {
        auto p = rxb.decode(dup * i, ctl_mask(peer(), i, key), 0, nullptr);
        if (!p && dup == 2) p = ctl_pair(rxb, 2 * i, i);
        if (!p && prime && rxb.can_prime()) {
            if (!hyps) hyps = expect(rxb);
            // only hypotheses with this burst's layout and core
            std::vector<Hypothesis> hs;
            for (const auto& h : *hyps) {
                bool same = h.pair == (dup == 2);
                for (std::size_t j = 0; same && j < 4; ++j)
                    same = j < h.tmpl.size() && j < first->size() &&
                           h.tmpl[j] == static_cast<std::uint8_t>(((*first)[j] & h.mask[j]) | (h.tmpl[j] & ~h.mask[j] & 255));
                if (same) hs.push_back(h);
            }
            auto got = primed(rxb, dup * i, i, hs).first;
            was_primed |= got.has_value();
            p = std::move(got);
        }
        if (!p) return std::nullopt;
        payloads.push_back(std::move(*p));
    }
    Control ctl;
    try {
        ctl = Control::unpack(payloads);
        last_stream_.clear();
        for (const auto& p : payloads) last_stream_.insert(last_stream_.end(), p.begin(), p.end());
    } catch (const std::invalid_argument& e) {
        malformed(e.what());
        return std::nullopt;
    }
    if (auto why = check(ctl.core, ctl.ext, rxb.n_cw() - dup * ctl.core.n_ctl, policy->payload_bytes(rxb.submode()))) {
        malformed(*why);
        return std::nullopt;
    }
    const bool nodded = rxb.is_nod();
    if (was_primed || nodded) {  // checked against my state, as a primed one: dropped, never a failure
        if (auto why = implausible(ctl.core, ctl.ext)) {
            log_write(LOG, INFO, std::string("RX ") + (nodded ? "nod" : "primed control") + " contradicts my state (" +
                                     *why + "): dropped");
            return std::nullopt;
        }
        stats[nodded ? "rx_nod" : combined ? "rx_repeat" : "rx_primed"] += 1;
    }
    primed_ctl_ = was_primed;
    return std::make_pair(std::move(ctl), dup);
}

// Control codeword i (from `slot`) under each hypothesis in turn: -> (payload, decoded as a pair).
std::pair<std::optional<Bytes>, bool> Station::primed(RxBurst& rxb, int slot, int i, const std::vector<Hypothesis>& hyps) {
    const std::size_t cpb = static_cast<std::size_t>(policy->ctl_payload_bytes(rxb.submode()));
    const std::size_t at0 = static_cast<std::size_t>(i) * cpb;
    for (const auto& h : hyps) {
        if (h.tmpl.size() <= at0) continue;
        const std::size_t n = std::min(cpb, h.tmpl.size() - at0);
        auto p = rxb.decode_known(slot, ctl_mask(peer(), i, key), ByteView(h.tmpl).subspan(at0, n),
                                  ByteView(h.mask).subspan(at0, n), h.pair);
        if (p) return {std::move(p), h.pair};
    }
    return {std::nullopt, false};
}

// What a primed control (§3a) says that my state can't explain. Checked before any state changes: a blind
// control saying so fails the session (ProtocolError), a primed one is only dropped.
std::optional<std::string> Station::implausible(const Core& core, const Ext& ext) const {
    int n = 0;
    for (std::int64_t k = confirmed; k <= latest; ++k) n += pmod(k, BURST_MOD) == core.acted_on;
    if (n != 1) return "acted-on " + str(core.acted_on);
    const std::int64_t cum = unwrap(core.cum, tx.base);
    if (cum < tx.base || cum > tx.next) return "cumulative " + str(core.cum);
    if (auto e = ext.find(T_ABANDON); e != ext.end() && e->second.size() >= 2 && (e->second[1] >> 1) != peer_epoch &&
                                      unwrap(e->second[0], rx.cum) != rx.cum)
        return std::string("abandon point");
    return std::nullopt;
}

namespace {
enum CoreField { F_FTYPE, F_NCTL, F_SEQ, F_ACTED, F_CUM, F_LOST, F_K, F_REC, F_HINT };
constexpr int CORE_BITS[9] = {2, 2, 3, 3, 7, 1, 6, 6, 2};
}  // namespace

std::vector<std::string> Station::expected_modes() const {
    std::vector<std::string> out;
    const auto add = [&](std::string m) {
        if (std::find(out.begin(), out.end(), m) == out.end()) out.push_back(std::move(m));
    };
    if (last_sent) add(last_sent->submode);
    for (const auto& r : {asked_.first, asked_.second})
        if (r) add(policy->mode_name(*r));
    return out;
}

std::vector<Station::Hypothesis> Station::expect(const RxBurst& rxb) const {
    const int cpb = policy->ctl_payload_bytes(rxb.submode());
    const int max_ctl = policy->max_ctl(rxb.submode());
    const int ctl_slots = rxb.n_ctl_slots();  // CPM: its header says how many (2: duplicated)
    const int n_cw = rxb.n_cw();
    std::vector<Hypothesis> out;
    std::optional<Hypothesis> repeat;
    if (!master && peer_last_ && peer_last_->second == std::make_pair(rxb.submode(), n_cw)) {
        // the caller repeated the burst I answered: my reply was lost (§6). Second (after the likeliest
        // fresh one): a header like the last is mostly a fixed mode
        const Bytes& t = peer_last_->first;
        repeat = Hypothesis{t, Bytes(t.size(), 0xFF), Core::unpack(t).ftype == ARQ_DUP};
    }
    std::vector<std::pair<bool, bool>> kinds;  // (data, pair)
    if (ctl_slots) {
        kinds.emplace_back(n_cw > ctl_slots, ctl_slots == 2);
    } else {
        if (n_cw <= max_ctl) kinds.emplace_back(false, false);
        if (n_cw >= 2) kinds.emplace_back(true, asked_dup_);
        // the kind the peer sent last first
        std::stable_sort(kinds.begin(), kinds.end(), [&](const auto& a, const auto& b) {
            return (a.first != last_rx_data) < (b.first != last_rx_data);
        });
    }
    for (const auto& [data, pair] : kinds) {
        std::vector<Way> ways = variants(data, n_cw, ctl_slots, pair);
        // then the same with T_DUPCTL the other way, and with T_REPLY's recommendation unknown
        const Tlv dupctl{T_DUPCTL, {}, {}};
        const std::size_t n0 = ways.size();
        for (std::size_t j = 0; j < n0; ++j) {
            Way w = ways[j];
            if (std::find(w.ext.begin(), w.ext.end(), dupctl) != w.ext.end())
                std::erase_if(w.ext, [](const Tlv& t) { return t.type == T_DUPCTL; });
            else
                w.ext.push_back(dupctl);
            ways.push_back(std::move(w));
        }
        for (std::size_t j = 0; j < n0; ++j) {
            Way w = ways[j];
            bool has = false;
            for (auto& t : w.ext)
                if (t.type == T_REPLY) {
                    t.mask = Bytes{0xC0};
                    has = true;
                }
            if (has) ways.push_back(std::move(w));
        }
        for (const auto& w : ways) {
            // an OFDM control-only burst is its control: n_cw codewords, the last ones maybe all padding
            auto h = pack_hypothesis(w, cpb, max_ctl, pair, (ctl_slots || data) ? 0 : n_cw);
            if (!h || std::find(out.begin(), out.end(), *h) != out.end()) continue;
            const int n_ctl = static_cast<int>(h->tmpl.size()) / cpb;
            const int dup = pair ? 2 : 1;
            if (ctl_slots || (!data ? n_ctl == n_cw : dup * n_ctl < n_cw)) out.push_back(std::move(*h));
        }
    }
    if (repeat && std::find(out.begin(), out.end(), *repeat) == out.end())
        out.insert(out.begin() + static_cast<std::ptrdiff_t>(std::min<std::size_t>(1, out.size())), *repeat);
    const int tries = std::max(0, prime_tries());
    if (static_cast<int>(out.size()) > tries) out.resize(static_cast<std::size_t>(tries));
    return out;
}

std::vector<Station::Way> Station::variants(bool data, int n_cw, int ctl_slots, bool pair) const {
    const Bytes* base = nullptr;
    if (auto it = peer_ctl_.find(data); it != peer_ctl_.end() && !it->second.empty())
        base = &it->second;
    else if (auto it2 = peer_ctl_.find(!data); it2 != peer_ctl_.end() && !it2->second.empty())
        base = &it2->second;
    Ext pe;
    if (base) pe = Control::unpack({*base}).ext;
    // the peer's cumulative lies in [base, next]: the bits all of those agree on
    int diff = 0;
    for (std::int64_t x = tx.base; x <= tx.next; ++x) diff |= static_cast<int>(pmod(x ^ tx.next, SEQ_MOD));
    Way core;
    auto& c = core.core;
    // a caller's control-only burst may be a poll (PROBE: 2), a callee's never is
    c[F_FTYPE] = {pair ? ARQ_DUP : ARQ, (data || master) ? 3 : 1};
    c[F_NCTL] = {0, 3};  // set by pack_hypothesis
    // one past the last I decoded, unless I missed some
    c[F_SEQ] = {peer_burst ? static_cast<int>(pmod(*peer_burst + 1, BURST_MOD)) : 0, (misses || !(data || master)) ? 0 : 7};
    // my latest burst, or (a caller's poll: my reply to it was lost) the one before
    c[F_ACTED] = {static_cast<int>(pmod(latest, BURST_MOD)),
                  (data || master) ? 7 : static_cast<int>(7 & ~(latest ^ (latest - 1)))};
    c[F_CUM] = {static_cast<int>(pmod(tx.next, SEQ_MOD)), 127};
    c[F_LOST] = {0, repeated_ ? 0 : 1};
    c[F_K] = {0, 63};
    c[F_REC] = {0, 0};  // the peer's recommendation moves with every measurement: unknown
    c[F_HINT] = {peer_size_hint, base ? 3 : 0};
    std::vector<Tlv> common;
    // (T_NODS: sent until I acted on it, so mostly as last time)
    for (int t : {T_CHAT, T_DUPCTL, T_NODS})
        if (auto e = pe.find(t); e != pe.end()) common.push_back({t, e->second, Bytes(e->second.size(), 0xFF)});
    // T_REPLY, a recommendation: as last time
    if (auto e = pe.find(T_REPLY); e != pe.end() && e->second.size() == 1) common.push_back({T_REPLY, e->second, Bytes{0xFF}});
    if (auto e = pe.find(T_BUFFER); e != pe.end()) common.push_back({T_BUFFER, e->second, Bytes(e->second.size(), 0)});
    if (!base) common = {{T_REPLY, Bytes{0}, Bytes{0xC0}}, {T_DUPCTL, {}, {}}};  // its first ARQ burst: the shifter's
    std::vector<Way> ways;
    if (!data) {
        core.ext = common;
        if (tx.base == tx.next) return {core};
        Way gap = core;
        gap.core[F_CUM].mask = 127 & ~diff;
        Way gapb = gap;
        gapb.ext.push_back({T_BITMAP, Bytes{0}, Bytes{0}});
        ways = {core, gapb, gap};
        if (pe.count(T_BITMAP)) std::swap(ways[0], ways[1]);
        return ways;
    }
    // data: the peer resends what my latest burst's ACK reported missing
    std::int64_t nxt = std::max<std::int64_t>(peer_next_.value_or(0), rx.cum);
    if (!rx.buf.empty()) nxt = std::max(nxt, rx.buf.rbegin()->first + 1);
    int missing = 0;
    if (auto sn = snapshots.find(latest); sn != snapshots.end())
        for (std::int64_t x = sn->second.first; x < nxt; ++x) missing += !sn->second.second.count(x);
    const int n_data = n_cw - (ctl_slots ? ctl_slots : (pair ? 2 : 1));
    std::vector<Tlv> extra;
    if (auto e = pe.find(T_DRAINED); e != pe.end()) extra.push_back({T_DRAINED, e->second, Bytes(e->second.size(), 0xFF)});
    std::vector<int> seen;
    for (int k : {std::min(missing, n_data), std::min(missing + 1, n_data), 0}) {
        if (std::find(seen.begin(), seen.end(), k) != seen.end()) continue;
        seen.push_back(k);
        Way w = core;
        w.core[F_K] = {k, 63};
        w.ext = common;
        w.ext.insert(w.ext.end(), extra.begin(), extra.end());
        const auto rv_bytes = static_cast<std::size_t>(ceil_div(2 * k, 8));
        if (k) w.ext.push_back({T_RV, Bytes(rv_bytes, 0), Bytes(rv_bytes, 0)});
        if (n_data > k)
            w.ext.push_back({T_NEW, Bytes{static_cast<std::uint8_t>(pmod(nxt, SEQ_MOD))},
                             Bytes{static_cast<std::uint8_t>(peer_next_ ? 0xFF : 0)}});
        ways.push_back(std::move(w));
    }
    return ways;
}

std::optional<Station::Hypothesis> Station::pack_hypothesis(const Way& w, int cpb, int max_ctl, bool pair, int n_ctl) {
    std::vector<Tlv> ext = w.ext;
    std::stable_sort(ext.begin(), ext.end(), [](const Tlv& a, const Tlv& b) {
        return std::tie(a.type, a.value, a.mask) < std::tie(b.type, b.value, b.mask);
    });
    Bytes body, bmask;
    for (const auto& t : ext) {
        body.push_back(static_cast<std::uint8_t>(t.type));
        body.push_back(static_cast<std::uint8_t>(t.value.size()));
        body.insert(body.end(), t.value.begin(), t.value.end());
        bmask.push_back(0xFF);
        bmask.push_back(0xFF);
        bmask.insert(bmask.end(), t.mask.begin(), t.mask.end());
    }
    const int need = std::max<int>(1, static_cast<int>(ceil_div(4 + static_cast<std::int64_t>(body.size()), cpb)));
    if (!n_ctl) n_ctl = need;
    if (n_ctl < need || n_ctl > max_ctl) return std::nullopt;
    auto f = w.core;
    f[F_NCTL] = {n_ctl - 1, 3};
    std::uint32_t v = 0, m = 0;
    for (std::size_t j = 0; j < 9; ++j) {
        v = (v << CORE_BITS[j]) | static_cast<std::uint32_t>(f[j].value & f[j].mask);
        m = (m << CORE_BITS[j]) | static_cast<std::uint32_t>(f[j].mask);
    }
    Hypothesis h;
    h.pair = pair;
    for (int sh = 24; sh >= 0; sh -= 8) {
        h.tmpl.push_back(static_cast<std::uint8_t>(v >> sh));
        h.mask.push_back(static_cast<std::uint8_t>(m >> sh));
    }
    h.tmpl.insert(h.tmpl.end(), body.begin(), body.end());
    h.mask.insert(h.mask.end(), bmask.begin(), bmask.end());
    const std::size_t n = static_cast<std::size_t>(n_ctl) * static_cast<std::size_t>(cpb);
    h.tmpl.resize(n, 0);
    h.mask.resize(n, 0xFF);  // the padding: known zero
    return h;
}

// Take the peer's requests and flags, and move my escalation.
// -> (its previous recommendation and size hint, whether this burst repeats one I answered)
std::pair<Station::PeerAsk, bool> Station::note_peer(const Core& core, const Ext& ext) {
    const bool escalated = misses || reply_escalation;
    misses = 0;
    const PeerAsk was{peer_recommend, peer_size_hint};
    peer_recommend = core.recommend;
    peer_size_hint = core.size_hint;
    auto e_reply = ext.find(T_REPLY);
    peer_reply_recommend = (e_reply != ext.end() && !e_reply->second.empty()) ? std::optional<int>(e_reply->second[0]) : std::nullopt;
    peer_chat = ext.count(T_CHAT) > 0;
    peer_drained = ext.count(T_DRAINED) > 0;
    peer_wants_dup = ext.count(T_DUPCTL) > 0;
    peer_reply_lost = core.reply_lost;
    peer_abandon = ext.count(T_ABANDON) > 0;
    auto e_buf = ext.find(T_BUFFER);
    peer_queued = (e_buf != ext.end() && e_buf->second.size() == 2) ? (e_buf->second[0] << 8 | e_buf->second[1]) : 0;
    const bool seen = peer_burst && core.burst_seq == *peer_burst;
    const bool repeat = seen && answered_;
    // a repeat or a poll means my last reply did not get through
    reply_escalation = (repeat || core.ftype == PROBE) ? reply_escalation + 1 : 0;
    if (!reply_escalation) {
        if (escalated) {
            esc_floor = sent_esc_, clean_ = 0;  // recovered: my last burst got through at this escalation
        } else if (esc_floor && ++clean_ >= FLOOR_DECAY_TURNS) {
            --esc_floor, clean_ = 0;
        }
    }
    return {was, repeat};
}

// Their ACK of my data (their cum / bitmap), acted on in my next burst.
// -> (my burst it acted on, progress). Throws ProtocolError.
std::pair<std::int64_t, bool> Station::take_ack(const Core& core, const Ext& ext) {
    // which of my bursts the peer acted on: its 3-bit seq, resolved in [confirmed, latest]
    std::vector<std::int64_t> acted_list;
    for (std::int64_t k = confirmed; k <= latest; ++k)
        if (pmod(k, BURST_MOD) == core.acted_on) acted_list.push_back(k);
    if (acted_list.size() != 1)
        throw ProtocolError("acted-on " + str(core.acted_on) + " outside my bursts " + str(confirmed) + ".." + str(latest));
    const std::int64_t acted = acted_list[0];
    confirmed = acted;
    if (abandon_tlv && abandon_bursts.count(acted)) abandon_tlv.reset();  // the peer has applied it
    const std::int64_t cum = unwrap(core.cum, tx.base);
    std::set<std::int64_t> received;
    if (auto b = ext.find(T_BITMAP); b != ext.end())
        for (int x : unpack_bitmap(b->second, core.cum)) received.insert(unwrap(x, cum));
    for (auto bn : decoded_bursts(acted, cum, received)) {
        auto it = sent_seqs.find(bn);
        for (auto x : it->second) {
            if (auto c = tx.cws.find(x); c != tx.cws.end()) {
                // the peer decoded that burst: it holds soft bits (none for one sent inline)
                c->second.heard += !(c->second.inlined && c->second.first_bn == bn);
                c->second.comp_known = c->second.comp_known || c->second.first_bn == bn;
            }
        }
        sent_seqs.erase(it);
    }
    // (settle_limbo can start one before any burst carries it: every ACK is from before it)
    if (abandon_tlv && (abandon_bursts.empty() || acted < *abandon_bursts.begin()) && unwrap(core.cum, tx.base) > tx.base) {
        // an ACK of a burst from before my pending abandon describes the old
        // slicing: past the abandon point it can't be mapped (§4)
        throw ProtocolError(format("ACK of pre-abandon burst %d past the abandon at %lld", static_cast<int>(pmod(acted, BURST_MOD)),
                                   static_cast<long long>(tx.base)));
    }
    const bool progress = tx.on_ack(cum, received);
    acted_on_ = core.burst_seq;
    return {acted, progress};
}

// My bursts this ACK shows the peer decoded: the one it acted on, and any
// before it whose reply was lost that the ACK vouches for (a seq it newly
// reports received that no other burst up to `acted` carried). link.py's
// _decoded_bursts.
std::vector<std::int64_t> Station::decoded_bursts(std::int64_t acted, std::int64_t cum,
                                                  const std::set<std::int64_t>& received) const {
    const std::int64_t had_cum = tx.ack ? tx.ack->first : tx.base;
    static const std::set<std::int64_t> none;
    const auto& had = tx.ack ? tx.ack->second : none;
    auto fresh = [&](std::int64_t x) {
        return (x < cum || received.count(x)) && !(x < had_cum || had.count(x));
    };
    std::map<std::int64_t, int> times;
    for (const auto& [b, seqs] : sent_seqs)
        if (b <= acted)
            for (auto x : seqs) ++times[x];
    std::vector<std::int64_t> out;
    for (const auto& [b, seqs] : sent_seqs) {
        if (b > acted) continue;
        bool vouched = b == acted;
        for (auto x : seqs) vouched = vouched || (times[x] == 1 && fresh(x));
        if (vouched) out.push_back(b);
    }
    return out;
}

// Apply the peer's abandon if it is a new one. -> whether it was. Throws ProtocolError.
bool Station::take_abandon(RxBurst& rxb, const Ext& ext) {
    auto e_ab = ext.find(T_ABANDON);
    if (e_ab == ext.end() || at(e_ab->second, 1) >> 1 == peer_epoch) return false;
    const std::int64_t a = unwrap(at(e_ab->second, 0), rx.cum);
    if (a != rx.cum) throw ProtocolError("abandon at " + str(a) + ", cumulative " + str(rx.cum));
    peer_epoch = e_ab->second[1] >> 1;
    rx.abandon(a);
    forget_all(rxb);
    return true;
}

// Decode and deliver their data slots. Throws ProtocolError.
Station::DataTaken Station::take_data(RxBurst& rxb, const Core& core, const Ext& ext, int dup, std::int64_t acted,
                                      bool outcome) {
    DataTaken out;
    const int n_ctl_slots = dup * core.n_ctl;
    const auto& slots = out.slots = map_slots(core, ext, rxb.n_cw() - n_ctl_slots + core.n_ctl, acted);
    auto e_comp = ext.find(T_COMP);
    const auto bits = unpack_flags(e_comp == ext.end() ? ByteView{} : ByteView(e_comp->second), static_cast<int>(slots.size()));
    for (std::size_t j = static_cast<std::size_t>(core.k); j < slots.size(); ++j)
        if (bits[j] && slots[j].first && *slots[j].first >= rx.cum) rx.comp_seqs.insert(*slots[j].first);
    std::vector<bool> comp;
    for (std::size_t j = 0; j < slots.size(); ++j) comp.push_back(bits[j] || (slots[j].first && rx.comp_seqs.count(*slots[j].first)));
    last_rx_data = !slots.empty();
    int n_ok = 0, n_new = 0;
    {
        std::vector<DecodeRequest> reqs;
        for (std::size_t j = 0; j < slots.size(); ++j) {
            const auto& [seq, rv] = slots[j];
            if (seq && *seq >= rx.cum)
                reqs.push_back({n_ctl_slots + static_cast<int>(j), data_mask(peer(), *seq, key, comp[j], peer_epoch), rv,
                                SoftKey{false, peer(), *seq, 0}});
        }
        rxb.prepare(reqs);
    }
    for (std::size_t j = 0; j < slots.size(); ++j) {
        const int i = n_ctl_slots + static_cast<int>(j);
        const auto& [seq, rv] = slots[j];
        if (!seq || *seq < rx.cum) {
            out.n_old += 1;
            continue;
        }
        const SoftKey skey{false, peer(), *seq, 0};
        auto p = rxb.decode(i, data_mask(peer(), *seq, key, comp[j], peer_epoch), rv, &skey);
        if (i >= n_ctl_slots + core.k) {
            n_new += 1;
            n_ok += p.has_value();
        }
        if (p) {
            out.n_dec += 1;
            rxb.forget(skey);
            out.progress |= rx.accept(*seq, std::move(*p), comp[j]);
        }
    }
    rx_new_end_.reset();
    for (std::size_t j = slots.size(); j-- > static_cast<std::size_t>(core.k);)
        if (slots[j].first) {
            peer_next_ = *slots[j].first + 1;
            rx_new_end_ = *slots[j].first + 1;
            break;
        }
    if (auto e = ext.find(T_INLINE); e != ext.end()) {
        last_rx_data = true;
        const Bytes& b = e->second;
        const std::int64_t seq = unwrap(b[0] & 0x7F, rx.cum);
        peer_next_ = std::max<std::int64_t>(peer_next_.value_or(0), seq + 1);
        rx_new_end_ = std::max<std::int64_t>(rx_new_end_.value_or(0), seq + 1);
        const bool z = (b[0] >> 7) != 0;
        Bytes p(b.begin() + 1, b.end() - 2);
        p.resize(static_cast<std::size_t>(policy->payload_bytes(rxb.submode())), 0);
        // checked under its identity, as a data slot's CRC is (§2): seq, compression, slicing
        if (inline_check(data_mask(peer(), seq, key, z, peer_epoch), p) == Bytes(b.end() - 2, b.end())) {
            out.n_dec += seq >= rx.cum;
            out.progress |= rx.accept(seq, std::move(p), z);
        } else {
            log_write(LOG, INFO, format("RX inline seq %lld: check failed, dropped", static_cast<long long>(seq)));
        }
    }
    // a control only a primed retry read is told to the policy as the blind decode saw it (link.py, §3a)
    // a nod is neither a blind decode nor a loss of one: no turn for the policy
    if (outcome) policy->outcome(rxb.submode(), n_ok, n_new, rxb.is_nod() ? std::nullopt : std::optional<bool>(!primed_ctl_));
    return out;
}

void Station::log_rx(RxBurst& rxb, const Core& core, int dup, const DataTaken& data, std::int64_t base0, std::int64_t next0,
                     std::int64_t cum0, const PeerAsk& was, bool repeat, bool abandoned) {
    const auto& slots = data.slots;
    const char* kind = !slots.empty() ? "data" : (core.ftype == PROBE ? "poll" : "ack");
    std::vector<std::string> parts{std::string(kind) + " " + burst_desc(rxb.submode(), rxb.n_cw(), dup == 2) + snr()};
    if (!slots.empty())
        parts.push_back("resend " + str(core.k) + " + new " + str(static_cast<int>(slots.size()) - core.k) + ", decoded " +
                        str(data.n_dec) + "/" + str(static_cast<int>(slots.size()) - data.n_old) +
                        (data.n_old ? ", " + str(data.n_old) + " already had" : "") + ", cum " + str(cum0) + "->" + str(rx.cum) +
                        (rx.buf.empty() ? "" : " +" + str(rx.buf.size()) + " held"));
    if (tx.base != base0)
        parts.push_back("acked " + str(base0) + "->" + str(tx.base));
    else if (next0 > base0)
        parts.push_back("acked none of " + str(base0) + "-" + str(next0 - 1));
    const PeerAsk now{peer_recommend, peer_size_hint};
    std::string ask = "wants data " + mode(now.first) + " size " + str(now.second);
    if (was.first && was != now) ask += " (was " + mode(was.first) + " size " + str(was.second) + ")";
    parts.push_back(ask + ", reply " + mode(peer_reply_recommend));
    if (repeat) parts.push_back("repeat (my reply lost)");
    if (abandoned) parts.push_back("abandon");
    if (dup == 2) parts.push_back("dup ctl");
    if (peer_wants_dup) parts.push_back("wants dup ctl");
    log_write(LOG, INFO, format("RX b%d %s", core.burst_seq, join(parts, " | ").c_str()));
}

std::optional<std::string> Station::check(const Core& core, const Ext& ext, int n_data, int pb) {
    if (core.k > n_data) return format("K %d in %d data slots", core.k, n_data);
    auto e_rv = ext.find(T_RV);
    const std::size_t rv_bytes = e_rv == ext.end() ? 0 : e_rv->second.size();
    if (static_cast<std::int64_t>(rv_bytes) < ceil_div(2 * core.k, 8))
        return format("T_RV of %d B for K %d", static_cast<int>(rv_bytes), core.k);
    if (auto e = ext.find(T_NEW); e != ext.end() && e->second.empty()) return std::string("empty T_NEW");
    if (auto e = ext.find(T_ABANDON); e != ext.end() && e->second.size() < 2)
        return format("T_ABANDON of %d B", static_cast<int>(e->second.size()));
    if (auto e = ext.find(T_INLINE); e != ext.end() && (e->second.size() < 3 || static_cast<int>(e->second.size()) > 3 + pb))
        return format("T_INLINE of %d B", static_cast<int>(e->second.size()));
    return std::nullopt;
}

bool Station::malformed(const std::string& why) {
    log_write(LOG, WARNING, "RX malformed control (" + why + "): dropped");
    return false;
}

std::optional<Bytes> Station::ctl_pair(RxBurst& rxb, int slot, int i) {
    const SoftKey k{true, 0, 0, i};
    const MaskId mask = ctl_mask(peer(), i, key);
    rxb.decode(slot, mask, 0, &k);
    auto p = rxb.decode(slot + 1, mask, 1, &k);
    rxb.forget(k);
    return p;
}

std::vector<std::pair<std::optional<std::int64_t>, int>> Station::map_slots(const Core& core, const Ext& ext, int n_cw,
                                                                           std::int64_t acted) {
    std::vector<std::pair<std::optional<std::int64_t>, int>> out;
    auto snap = snapshots.find(acted);
    auto e_rv = ext.find(T_RV);
    const auto rvs = unpack_rv(e_rv == ext.end() ? ByteView{} : ByteView(e_rv->second), core.k);
    if (core.k) {
        if (snap == snapshots.end()) {
            out.insert(out.end(), static_cast<std::size_t>(core.k), {std::nullopt, 0});
        } else {
            const auto& [cum, received] = snap->second;
            std::int64_t s = cum;
            for (int j = 0; j < core.k; ++s)
                if (!received.count(s)) out.emplace_back(s, rvs[static_cast<std::size_t>(j++)]);
        }
    }
    const int n_new = n_cw - core.n_ctl - core.k;
    if (n_new > 0) {
        auto e_new = ext.find(T_NEW);
        if (e_new == ext.end()) {
            out.insert(out.end(), static_cast<std::size_t>(n_new), {std::nullopt, 0});
        } else {
            const std::int64_t start = unwrap(at(e_new->second, 0), rx.cum);
            for (int j = 0; j < n_new; ++j) out.emplace_back(start + j, 0);
        }
    }
    return out;
}

void Station::forget_all(RxBurst& rxb) {
    for (std::int64_t s = rx.cum; s < rx.cum + WINDOW; ++s) rxb.forget(SoftKey{false, peer(), s, 0});
}

void Station::fail(const std::string& why) {
    log_write(LOG, WARNING, "link failed: " + why);
    state = LinkState::FAILED;
    fail_reason = why;
}

void Station::watchdog(bool progress) {
    if (progress) {
        no_progress = 0;
        resyncs = 0;
        resync_due = false;  // a deferred resync is moot once data moves
        return;
    }
    if (!tx.pending()) return;
    no_progress += 1;
    if (no_progress >= NO_PROGRESS_TURNS) {
        no_progress = 0;
        resyncs += 1;
        if (resyncs > RESYNCS_BEFORE_FAIL) {
            fail("no progress");
        } else {
            resync_due = true;
            log_write(LOG, INFO, format("watchdog: %d turns without progress, resync %d of %d", NO_PROGRESS_TURNS, resyncs,
                                        RESYNCS_BEFORE_FAIL));
        }
    }
}

}  // namespace data2g::arq
