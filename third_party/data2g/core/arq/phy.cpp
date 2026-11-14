#include "arq/phy.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <set>

#include "constellation/constellation.hpp"
#include "util/cancel.hpp"
#include "util/pool.hpp"
#include "cpm/cpm.hpp"
#include "arq/nod.hpp"
#include "equalizer/equalizer.hpp"

namespace data2g::arq {

namespace {

using cd = std::complex<double>;

const codes::Spec& ctl_code(const Mode& m) {
    return m.is_cpm() ? codes::spec(cpm::ctl(cpm::grid_of(*m.cpm))) : codes::spec(*m.ofdm);
}

const codes::Spec& data_code(const Mode& m) { return m.is_cpm() ? codes::spec(*m.cpm) : codes::spec(*m.ofdm); }

Mat<float> row_f(std::span<const double> v) {
    Mat<float> out(1, v.size());
    std::copy(v.begin(), v.end(), out.data.begin());
    return out;
}

// A control's combined buffer leaning toward its estimated padding (codes::pad_prior).
Mat<double> with_prior(const codes::Spec& s, std::span<const double> buf) {
    Mat<double> out(1, buf.size());
    out.data = codes::add_prior_buffer(s, buf, codes::pad_prior(s, codes::info_llrs_buffer(s, buf)));
    return out;
}

// A control mask's codeword index in the control stream (a compact CONNECT's is its only codeword).
int ctl_index(const MaskId& m) {
    const int i = m.seq - SEQ_MOD;
    return i < MAX_CTL ? i : 0;
}

// phy._sure: a decoded codeword's coded bits (mapping order, as sent) as DD's
// certain posterior: 30 (1 - 2 bit), or GF(M) log-probabilities 0 / -30 per symbol.
std::vector<double> known(const codes::Spec& s, std::span<const std::uint8_t> bits) {
    if (!s.gf) {
        std::vector<double> out(bits.size());
        for (std::size_t i = 0; i < bits.size(); ++i) out[i] = 30.0 * (1 - 2.0 * bits[i]);
        return out;
    }
    const int p = s.bits_per_cu, q = 1 << p;
    const std::size_t n = bits.size() / p;
    std::vector<double> out(n * q, -30.0);
    for (std::size_t i = 0; i < n; ++i) {
        int v = 0;
        for (int b = 0; b < p; ++b) v = v << 1 | (bits[i * p + b] & 1);
        out[i * q + v] = 0.0;
    }
    return out;
}

// The buffer decode_stored decodes: `soft` (this slot's, as received) flipped by the slot's scrambling and
// combined into what is stored (nullptr: nothing) at `rv`.
std::vector<double> combined_buffer(const codes::Spec& s, int slot, int rv, std::span<const double> soft,
                                    const std::vector<double>* stored) {
    const int rvs[1] = {rv};
    Mat<double> buf;
    if (stored) {
        buf = Mat<double>(1, stored->size());
        buf.data = *stored;
    }
    Mat<double> x(1, soft.size());
    x.data = codes::plain(s, soft, slot, rv);
    codes::combine(s, buf, x, rvs);
    return std::move(buf.data);
}

// decode_stored's body for one pass: combine, decode (with the posterior if DD), check the CRC under mask m.
DecodePass decode_pass(const codes::Spec& s, bool dd, int slot, int rv, std::uint32_t m, int top,
                       std::span<const double> soft, const std::vector<double>* stored) {
    DecodePass out;
    out.buf = combined_buffer(s, slot, rv, soft, stored);
    const std::uint32_t masks[1] = {m};
    const int plain[1] = {codes::PLAIN};
    if (!dd) {
        Mat<double> b(1, out.buf.size());
        b.data = out.buf;
        auto p = codes::decode_buffer(s, b, top, masks, plain)[0];
        out.ok = p.ok;
        if (p.ok) out.data = std::move(p.data);
        return out;
    }
    auto d = codes::decode_posterior(s, {}, out.buf, top, rv);
    Mat<std::uint8_t> bits(1, d.bits.size());
    bits.data = d.bits;
    const std::uint8_t conv[1] = {static_cast<std::uint8_t>(d.ok)};
    auto p = codes::payloads(s, bits, conv, masks, plain)[0];
    out.ok = p.ok;
    if (p.ok) out.data = std::move(p.data);
    out.post = std::move(d.post);
    return out;
}

}  // namespace

bool dd_default() {
    static const bool on = [] {
        const char* v = std::getenv("DATA2G_DD");
        return !v || std::strcmp(v, "1") == 0;
    }();
    return on;
}

bool cpm_dd_default() {
    static const bool on = [] {
        const char* v = std::getenv("DATA2G_CPM_DD");
        return !v || std::strcmp(v, "1") == 0;
    }();
    return on;
}

bool pin_est_default() {
    static const bool on = [] {
        const char* v = std::getenv("DATA2G_PIN_EST");
        return !v || std::strcmp(v, "1") == 0;
    }();
    return on;
}

double monotonic() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::uint32_t mask_value(const MaskId& m) {
    if (m.key == 0) return m.seq == COMPACT_CONNECT.seq ? 0xC0C0C0u : 0;  // a compact CONNECT's
    const std::uint8_t b[4] = {static_cast<std::uint8_t>(m.key >> 8), static_cast<std::uint8_t>(m.key & 255),
                               static_cast<std::uint8_t>(m.direction), static_cast<std::uint8_t>(m.seq)};
    const std::uint32_t v = codes::crc32(b);
    return v ? v : 1;
}

std::vector<double> tx_audio(const TxBurst& burst) {
    if (burst.nod) {  // a nod (docs/arq.md §3b): its keyed sequence
        const auto& [meaning, key] = *burst.nod;
        return nod::waveform(nod::family(key).at(static_cast<std::size_t>(meaning)), burst.cap);
    }
    const Mode& md = mode_at(burst.submode);
    const std::size_t n = burst.slots.size();
    if (md.is_cpm()) {
        // control slots (masks >= SEQ_MOD) in the grid's short codeword; two
        // of them are one codeword at RV 0 and 1 (ARQ_DUP)
        const auto n_ctl = std::count_if(burst.slots.begin(), burst.slots.end(),
                                         [](const Slot& s) { return s.mask_id.seq >= SEQ_MOD; });
        std::vector<std::vector<std::uint8_t>> coded;
        for (std::size_t i = 0; i < n; ++i) {
            const auto& s = burst.slots[i];
            coded.push_back(codes::encode(static_cast<std::ptrdiff_t>(i) < n_ctl ? ctl_code(md) : data_code(md),
                                          s.payload, s.rv, mask_value(s.mask_id), static_cast<int>(i)));
        }
        return cpm::bandpass(cpm::grid_of(*md.cpm), cpm::modulate(*md.cpm, coded, n_ctl == 2, burst.cap), burst.cap);
    }
    const auto& s = codes::spec(*md.ofdm);
    std::vector<std::uint8_t> bits;
    for (std::size_t i = 0; i < n; ++i) {
        const auto c = codes::encode(s, burst.slots[i].payload, burst.slots[i].rv, mask_value(burst.slots[i].mask_id),
                                     static_cast<int>(i));
        bits.insert(bits.end(), c.begin(), c.end());
    }
    return modem::modulate_bits(codes::spread<std::uint8_t>(bits, static_cast<int>(n), md.ofdm->bits_per_cu), *md.ofdm);
}

const Mode& Heard::mode() const { return mode_at(ofdm ? ofdm->spec->name : cpm->spec->name); }
int Heard::n_cw() const { return ofdm ? ofdm->n_cw : static_cast<int>(cpm->soft.size()); }

std::shared_ptr<const SlotSoft> soft_bits(const Heard& r) {
    if (r.cpm) return std::make_shared<const SlotSoft>(r.cpm->soft);
    const auto soft = modem::burst_soft(*r.ofdm);
    auto out = std::make_shared<SlotSoft>();
    for (std::size_t c = 0; c < soft.rows; ++c) out->emplace_back(soft[c], soft[c] + soft.cols);
    return out;
}

Measured measure(const Heard& r) {
    Measured out;
    if (r.cpm) {
        const auto& g = cpm::grid_of(*r.cpm->spec);
        const auto m = cpm::measure(g, r.cpm->E, static_cast<int>(r.cpm->E.rows));
        out.snr_est = m.snr_est;
        out.spread_est = m.spread_est;
        out.frames = m.frames;
        std::vector<cd> h(m.snr.size());
        for (std::size_t i = 0; i < h.size(); ++i) h[i] = std::sqrt(m.snr[i]);
        const std::vector<double> ones(h.size(), 1.0);
        for (std::size_t c = 0; c < CONSTS.size(); ++c) out.mi[c] = effective_mi(h, ones, CONSTS[c]);
        return out;
    }
    // effective MI against thermal noise and estimation error, not the
    // transmitter's clip noise (it belongs to the submode sent)
    const auto& R = *r.ofdm;
    const auto& est = R.est;
    std::vector<double> var(est.mse.data.size());
    for (std::size_t i = 0; i < var.size(); ++i) var[i] = est.n0 + est.mse.data[i];
    out.snr_est = modem::snr_db(est, R.band);
    out.spread_est = est.spread_hz;
    out.delay_est_ms = static_cast<double>(R.seen_support.second - R.seen_support.first) / config::FS * 1000;
    out.headroom = R.spec->headroom;
    out.frames = R.n_cw * R.spec->frames_per_cw;
    for (std::size_t c = 0; c < CONSTS.size(); ++c) out.mi[c] = effective_mi(est.h.data, var, CONSTS[c]);
    return out;
}

// --- ModemRx -------------------------------------------------------------------

std::optional<std::optional<Bytes>> ModemRx::reused(int slot, std::uint32_t m) const {
    if (!reader_) return std::nullopt;
    const auto it = reads_.find(slot);
    if (it == reads_.end()) return std::nullopt;
    return it->second.first == m ? std::optional<Bytes>(it->second.second) : std::nullopt;
}

std::optional<Bytes> ModemRx::note(int slot, std::uint32_t m, std::optional<Bytes> p) {
    if (p) reads_.try_emplace(slot, m, *p);
    return p;
}

void ModemRx::for_reader() {
    store_ = nullptr;
    reader_ = true;
}

std::optional<Bytes> ModemRx::decode_plain(int slot, const MaskId& mask) {
    if (auto r = reused(slot, mask_value(mask))) return *r;
    ++reads_run_;
    return note(slot, mask_value(mask), plain_read(slot, mask));
}

std::optional<Bytes> ModemRx::decode_stored(int slot, const MaskId& mask, int rv, std::optional<SoftEntry>& stored) {
    if (auto r = reused(slot, mask_value(mask))) return *r;
    ++reads_run_;
    return note(slot, mask_value(mask), stored_read(slot, mask, rv, stored));
}

std::optional<Bytes> ModemRx::decode_known(int slot, const MaskId& mask, ByteView tmpl, ByteView tmask, bool pair) {
    const int at = pair ? slot + 1 : slot;  // a pair's codeword, where a blind pair read (RV 1's slot) finds it
    if (auto r = reused(at, mask_value(mask))) return *r;
    ++reads_run_;
    return note(at, mask_value(mask), known_read(slot, mask, tmpl, tmask, pair));
}

ModemRx::ModemRx(Heard r, SoftStore* store, std::optional<double> dd_budget, std::shared_ptr<const SlotSoft> soft,
                 bool dd, Clock clock)
    : r_(std::move(r)), store_(store), soft_(soft ? std::move(soft) : soft_bits(r_)), dd_(dd), clock_(std::move(clock)) {
    if (dd_budget) dd_until_ = clock_() + *dd_budget;
    const Mode& md = r_.mode();
    submode_ = std::string(md.name);
    n_cw_ = r_.n_cw();
    n_ctl_slots_ = r_.cpm ? 1 + r_.cpm->dup : 0;
    data_spec_ = &data_code(md);
    ctl_spec_ = &ctl_code(md);
}

const codes::Spec& ModemRx::spec(int slot) const { return slot < n_ctl_slots_ ? *ctl_spec_ : *data_spec_; }

bool ModemRx::dd(const codes::Spec& s) const {
    if (n_ctl_slots_)  // CPM: data slots (GF(M)), on the burst's complex bins
        return dd_ && cpm_dd_default() && r_.cpm && r_.cpm->Z.rows && s.gf;
    return dd_ && r_.ofdm && r_.ofdm->hp.rows && !s.polar;
}

bool ModemRx::late() const { return dd_until_ && clock_() >= *dd_until_; }

// The slot's soft bits on the estimate in use. A refined estimate's are
// made per slot as asked (modem::slot_soft; CPM: its coherent metric, cpm::dd_metric).
const std::vector<double>& ModemRx::slot_soft(int slot) {
    if (!cur_.refined()) return (*soft_)[static_cast<std::size_t>(slot)];
    auto& cache = *cur_.soft;
    auto it = cache.find(slot);
    if (it != cache.end()) return it->second;
    if (cur_.posts) return cache[slot] = cpm::dd_soft(*r_.cpm, cpm::dd_metric(*r_.cpm, *cur_.posts, slot), slot);
    return cache[slot] = modem::slot_soft(*r_.ofdm, *cur_.est, slot);
}

std::optional<Bytes> ModemRx::decode(int slot, const MaskId& mask, int rv, const SoftKey* key) {
    if (!key) return decode_plain(slot, mask);
    std::optional<SoftEntry> stored;
    if (store_) {
        auto it = store_->find(*key);
        if (it != store_->end()) stored = it->second;
    }
    auto out = decode_stored(slot, mask, rv, stored);
    if (!out && store_ && stored) (*store_)[*key] = std::move(*stored);
    return out;
}

void ModemRx::forget(const SoftKey& key) {
    if (store_) store_->erase(key);
}

std::optional<Bytes> ModemRx::plain_read(int slot, const MaskId& mask) {
    if (slot >= n_cw_) return std::nullopt;
    // CPM: its header says which slots are control (their own short
    // codeword); a blind ARQ_DUP pair probe on a data slot is a miss
    if (n_ctl_slots_ && (mask.seq >= SEQ_MOD) != (slot < n_ctl_slots_)) return std::nullopt;
    const std::uint32_t m = mask_value(mask);
    auto it = memo_.find({slot, m});
    if (it != memo_.end()) return it->second;
    const auto& s = spec(slot);
    const Raw& d = decoded(slot, s);
    auto got = codes::check(s, d.cands, d.usable, m);
    if (!got && mask.seq >= SEQ_MOD) got = check_pinned(slot, s, m, ctl_index(mask));  // a control (ctl_mask)
    auto& out = memo_[{slot, m}] = std::move(got);
    tried_.insert(slot);
    if (out) ok_.insert(slot);
    return out;
}

namespace {

// Raw rows' usable candidates as payload bytes, appended.
template <class R>
void add_payloads(const codes::Spec& s, const R& r, std::vector<Bytes>& out) {
    const std::size_t k = static_cast<std::size_t>(s.k), nb = static_cast<std::size_t>(s.payload_bytes);
    for (std::size_t l = 0; l < r.usable.size(); ++l) {
        if (!r.usable[l]) continue;
        Bytes p(nb, 0);
        for (std::size_t i = 0; i < 8 * nb; ++i) p[i / 8] |= static_cast<std::uint8_t>((r.cands[l * k + i] & 1) << (7 - i % 8));
        out.push_back(std::move(p));
    }
}

std::vector<Bytes> unique(std::vector<Bytes> out) {
    std::vector<Bytes> uniq;
    for (auto& p : out)
        if (std::find(uniq.begin(), uniq.end(), p) == uniq.end()) uniq.push_back(std::move(p));
    return uniq;
}

}  // namespace

std::vector<Bytes> ModemRx::raw(int slot) {
    if (slot >= n_cw_) return {};
    const auto& s = spec(slot);
    std::vector<Bytes> out;
    add_payloads(s, decoded(slot, s), out);
    // pinned candidates after the plain ones: LDPC's when it did not converge; polar's always (its list is never
    // empty). A broadcast control is one codeword per slot (CPM: one, twice at most).
    if (out.empty() || s.polar)
        for (int off : offsets(s, n_ctl_slots_ ? 0 : slot)) add_payloads(s, pinned(slot, s, off), out);
    return unique(std::move(out));
}

bool ModemRx::pair_ok(int slot) const {
    if (slot + 1 >= n_cw_) return false;
    return !n_ctl_slots_ || slot + 1 < n_ctl_slots_;
}

// Slots `slot` (RV 0) and `slot + 1` (RV 1) combined and decoded once, mask open; `off` >= 0: the payload past
// it known zero. No DD: a pair is a control, one short decode.
const ModemRx::Raw& ModemRx::paired(int slot, const codes::Spec& s, int off) {
    auto it = pairs_.find({slot, off});
    if (it != pairs_.end()) return it->second;
    auto b0 = combined_buffer(s, slot, 0, slot_soft(slot), nullptr);
    auto b1 = combined_buffer(s, slot + 1, 1, slot_soft(slot + 1), &b0);
    Mat<double> buf(1, b1.size());
    buf.data = std::move(b1);
    const auto known = off < 0 || off == EST ? std::vector<std::int8_t>{} : codes::zero_tail(s, off);
    auto d = codes::decode_buffer_raw(s, off == EST ? with_prior(s, buf.data) : buf, 1, known);
    return pairs_[{slot, off}] = Raw{std::move(d.cands.data), std::move(d.usable.data)};
}

bool ModemRx::converged(int slot, bool pair) {
    if (slot >= n_cw_ || (pair && !pair_ok(slot))) return false;
    const auto& s = spec(slot);
    if (s.polar) return false;
    const Raw& d = pair ? paired(slot, s, -1) : decoded(slot, s);
    return std::any_of(d.usable.begin(), d.usable.end(), [](auto u) { return u != 0; });
}

std::vector<Bytes> ModemRx::raw_pair(int slot) {
    if (!pair_ok(slot)) return {};
    const auto& s = spec(slot);
    std::vector<Bytes> out;
    add_payloads(s, paired(slot, s, -1), out);
    if (out.empty() || s.polar)
        for (int off : offsets(s, slot / 2)) add_payloads(s, paired(slot, s, off), out);
    return unique(std::move(out));
}

std::optional<Bytes> ModemRx::decode_pair(int slot, const MaskId& mask) {
    if (!pair_ok(slot)) return std::nullopt;
    const auto& s = spec(slot);
    const std::uint32_t m = mask_value(mask);
    std::vector<int> offs{-1};
    for (int off : offsets(s, ctl_index(mask))) offs.push_back(off);
    for (int off : offs) {
        const Raw& d = paired(slot, s, off);
        if (auto got = codes::check(s, d.cands, d.usable, m)) return got;
    }
    return std::nullopt;
}

std::optional<Bytes> ModemRx::known_read(int slot, const MaskId& mask, ByteView tmpl, ByteView tmask, bool pair) {
    if (slot >= n_cw_ || (pair && !pair_ok(slot))) return std::nullopt;
    const auto& s = spec(slot);
    // polar only (phy.ModemRx.decode_known)
    if (!s.polar || (n_ctl_slots_ && slot >= n_ctl_slots_)) return std::nullopt;
    const std::uint32_t m = mask_value(mask);
    const auto known = codes::known_bits(s, tmpl, tmask);
    codes::Raw d;
    if (pair) {
        auto b0 = combined_buffer(s, slot, 0, slot_soft(slot), nullptr);
        auto b1 = combined_buffer(s, slot + 1, 1, slot_soft(slot + 1), &b0);
        Mat<double> buf(1, b1.size());
        buf.data = std::move(b1);
        d = codes::decode_buffer_raw(s, buf, 1, known);
    } else {
        d = codes::decode_raw_pinned(s, row_f(slot_soft(slot)), known, slot);
    }
    auto usable = d.usable.data;
    for (std::size_t l = PRIMED_PATHS; l < usable.size(); ++l) usable[l] = 0;
    auto got = codes::check(s, d.cands.data, usable, m);
    if (!got) return std::nullopt;
    // a pair's RV 1 is the same codeword: one more reception, at its own slot's scrambling
    double sc = codes::score(s, slot_soft(slot), *got, m, slot);
    if (pair) sc += codes::score(s, slot_soft(slot + 1), *got, m, slot + 1);
    if (sc < PRIMED_SCORE) {
        log_write("data2g.arq.phy", 20,
                  "primed control in slot " + std::to_string(slot) + ": CRC passed, score " + std::to_string(sc) +
                      " under " + std::to_string(PRIMED_SCORE) + " nats, dropped");
        return std::nullopt;
    }
    return got;
}

std::optional<Bytes> ModemRx::decode_repeat(int slot, const MaskId& mask, const SoftKey& key) {
    if (store_) {  // what was kept in another mode is not this burst's repeat
        auto it = store_->find(key);
        if (it != store_->end() && it->second.submode != submode_) store_->erase(it);
    }
    auto got = decode(slot, mask, 0, &key);
    if (got && !own_score(slot, mask, *got)) {
        reads_.erase(slot);  // not this burst's after all
        return std::nullopt;
    }
    return got;
}

// The sum may decode to an earlier, different burst's codeword: it must score on this burst alone.
bool ModemRx::own_score(int slot, const MaskId& mask, ByteView payload) {
    const double sc = codes::score(spec(slot), slot_soft(slot), payload, mask_value(mask), slot);
    if (sc >= PRIMED_SCORE) return true;
    log_write("data2g.arq.phy", 20,
              "repeated control in slot " + std::to_string(slot) + ": CRC passed, score " + std::to_string(sc) +
                  " under " + std::to_string(PRIMED_SCORE) + " nats, dropped");
    return false;
}

// Control codeword i's payload offsets past which a cut (bytes of the control stream) knows it zero.
std::vector<int> ModemRx::offsets(const codes::Spec& s, int i) const {
    std::vector<int> out;
    if (s.gf) return out;  // nothing to pin (codes.decode_raw_pinned)
    if (!s.polar && pin_est_default()) return {EST};
    const std::span<const int> cuts = s.polar ? std::span<const int>(POLAR_PIN_CUTS) : std::span<const int>(PIN_CUTS);
    for (int c : cuts) {
        const int off = c - i * s.payload_bytes;
        if (off > 0 && off < s.payload_bytes && std::find(out.begin(), out.end(), off) == out.end()) out.push_back(off);
    }
    return out;
}

// The slot decoded alone with its payload past `off` bytes known zero.
const ModemRx::Raw& ModemRx::pinned(int slot, const codes::Spec& s, int off) {
    auto it = pinned_.find({slot, off});
    if (it != pinned_.end()) return it->second;
    const auto& soft = slot_soft(slot);
    codes::Raw d;
    if (off == EST) {
        const auto v = codes::add_prior(s, soft, codes::pad_prior(s, codes::info_llrs(s, soft, slot)), slot);
        Mat<float> m(1, v.size());
        m.data = v;
        const int idx[1] = {slot};
        d = codes::decode_raw(s, m, idx);
    } else {
        d = codes::decode_raw_pinned(s, row_f(soft), codes::zero_tail(s, off), slot);
    }
    return pinned_[{slot, off}] = Raw{std::move(d.cands.data), std::move(d.usable.data)};
}

// Control codeword i that failed alone, decoded again with its padding known,
// the shortest cut first; each try CRC-checked under m like any decode.
std::optional<Bytes> ModemRx::check_pinned(int slot, const codes::Spec& s, std::uint32_t m, int i) {
    for (int off : offsets(s, i)) {
        const Raw& d = pinned(slot, s, off);
        if (auto got = codes::check(s, d.cands, d.usable, m)) {
            learn(slot, s, *got, 0, m);
            return got;
        }
    }
    return std::nullopt;
}

// The slot decoded alone, mask left open, with DD while it fails to
// converge. A converged codeword is what was on air, whoever it was for: DD
// learns from it.
cpm::Pass cpm_pass(const cpm::Received& r, const codes::Spec& s, std::span<const double> soft,
                   const cpm::Posts& earlier, int slot, const std::function<bool()>& late) {
    cpm::Posts posts;
    for (const auto& [k, v] : earlier)
        if (k < slot) posts.emplace(k, v);
    std::vector<double> cur(soft.begin(), soft.end());
    codes::Posterior p;
    for (int i = 0; i <= CPM_DD_ITERS; ++i) {
        p = codes::decode_posterior(s, cur);
        if (p.ok || i == CPM_DD_ITERS || (late && late())) break;
        posts[slot] = std::move(p.post);
        cur = cpm::dd_soft(r, cpm::dd_metric(r, posts, slot), slot);
    }
    cpm::Pass out{codes::descramble(s, p.bits, slot), {static_cast<std::uint8_t>(p.ok)}, {}};
    if (p.ok) {
        Mat<std::uint8_t> b(1, p.bits.size());
        b.data = p.bits;
        out.post = known(s, codes::encode_info(s, b, 0).data);
    }
    return out;
}

cpm::PassFn cpm_pass_fn() {
    return [](const cpm::Received& r, int slot, const cpm::Posts& earlier) {
        return cpm_pass(r, codes::spec(*r.spec), r.soft[static_cast<std::size_t>(slot)], earlier, slot);
    };
}

// ModemRx._cpm_passes: every data slot's pass through `upto`, in slot order,
// each seeing only earlier passes' posteriors: the receiver's (made while the
// burst arrived, r.passes) where it has them.
void ModemRx::cpm_passes(int upto) {
    const auto& s = *data_spec_;
    for (int k = n_ctl_slots_; k <= upto; ++k) {
        if (passed_.count(k)) continue;
        const auto made = r_.cpm->passes.find(k);
        cpm::Pass p = made != r_.cpm->passes.end()
                          ? made->second
                          : cpm_pass(*r_.cpm, s, (*soft_)[static_cast<std::size_t>(k)], pass_post_, k,
                                     [this] { return late(); });
        passed_.insert(k);
        if (!p.post.empty()) {
            pass_post_[k] = p.post;
            post_.emplace(k, std::move(p.post));
        }
        raw_.emplace(k, Raw{std::move(p.cands), std::move(p.usable)});
    }
}

const ModemRx::Raw& ModemRx::decoded(int slot, const codes::Spec& s) {
    if (!raw_.count(slot) && n_ctl_slots_ && dd(s)) cpm_passes(slot);
    auto it = raw_.find(slot);
    if (it != raw_.end()) return it->second;
    const DdState saved = cur_;
    Raw out;
    for (int i = 0; i <= iters(); ++i) {
        if (!dd(s)) {
            const int idx[1] = {slot};
            auto d = codes::decode_raw(s, row_f(slot_soft(slot)), idx);
            out = {std::move(d.cands.data), std::move(d.usable.data)};
            break;
        }
        auto p = codes::decode_posterior(s, slot_soft(slot));
        out = {codes::descramble(s, p.bits, slot), {static_cast<std::uint8_t>(p.ok)}};
        if (p.ok) {
            Mat<std::uint8_t> b(1, p.bits.size());
            b.data = p.bits;
            post_[slot] = known(s, codes::encode_info(s, b, 0).data);
            break;
        }
        if (i == iters() || late()) break;
        if (!refine(slot, s, std::move(p.post))) break;
    }
    if (std::none_of(out.usable.begin(), out.usable.end(), [](auto u) { return u != 0; })) undo(slot, saved);
    return raw_[slot] = std::move(out);
}

std::optional<Bytes> ModemRx::stored_read(int slot, const MaskId& mask, int rv, std::optional<SoftEntry>& stored) {
    if (slot >= n_cw_) return std::nullopt;
    if (n_ctl_slots_ && (mask.seq >= SEQ_MOD) != (slot < n_ctl_slots_)) return std::nullopt;
    const std::uint32_t m = mask_value(mask);
    const auto& s = spec(slot);
    tried_.insert(slot);
    if (stored && stored->submode != submode_) {  // a resend in another mode: what was kept can't combine
        log_write("data2g.arq.phy", 30,
                  format("soft bits stored in %s (slot %d rv %d), resent in %s slot %d rv %d: dropped",
                         stored->submode.c_str(), stored->slot, stored->rv, submode_.c_str(), slot, rv));
        stored.reset();
    }
    const int top = std::max(stored ? stored->top : 0, rv);
    const DdState saved = cur_;
    const std::vector<double>* kept = stored ? &stored->buf : nullptr;
    std::vector<double> buf;
    for (int i = 0; i <= iters(); ++i) {
        DecodePass ps;
        const auto pre = prepared_.find(slot);
        if (i == 0 && pre != prepared_.end() && !cur_.refined() && pre->second.rv == rv && pre->second.mask == m &&
            pre->second.had_stored == stored.has_value()) {
            ps = std::move(pre->second.pass);  // made ahead, on this same estimate and these same bits
            prepared_.erase(pre);
        } else {
            ps = decode_pass(s, dd(s), slot, rv, m, top, slot_soft(slot), kept);
        }
        buf = std::move(ps.buf);
        if (!dd(s)) {
            if (!ps.ok) break;
            ok_.insert(slot);
            return std::move(ps.data);
        }
        if (ps.ok) {
            learn(slot, s, ps.data, rv, m);
            ok_.insert(slot);
            return std::move(ps.data);
        }
        if (i == iters() || late()) break;
        // back to the codeword as sent (codes.plain: LDPC's signs flipped, GF(M)'s values permuted)
        if (!refine(slot, s, codes::plain(s, ps.post, slot, rv))) break;
    }
    prepared_.erase(slot);
    if (cur_.est != saved.est || cur_.posts != saved.posts) {
        undo(slot, saved);
        buf = combined_buffer(s, slot, rv, slot_soft(slot), kept);
    }
    if (kept && mask.seq >= SEQ_MOD) {  // a control combined (ARQ_DUP): its padding known
        for (int off : offsets(s, ctl_index(mask))) {
            Mat<double> b(1, buf.size());
            b.data = buf;
            const std::uint32_t masks[1] = {m};
            const int plain[1] = {codes::PLAIN};
            auto p = off == EST ? codes::decode_buffer(s, with_prior(s, buf), top, masks, plain)[0]
                                : codes::decode_buffer_pinned(s, b, top, codes::zero_tail(s, off), m)[0];
            if (p.ok) {
                learn(slot, s, p.data, rv, m);
                ok_.insert(slot);
                return std::move(p.data);
            }
        }
    }
    stored = SoftEntry{std::move(buf), top, submode_, slot, rv, mask};
    return std::nullopt;
}

// decode_stored's first pass for each request at once: the LDPC decodes are independent, and the session then
// asks for them one at a time. Only on the burst's own estimate (a DD re-estimate changes every slot's soft bits)
// and with every key distinct (two slots sharing one combine into the same stored buffer, in order).
void ModemRx::prepare(std::span<const DecodeRequest> reqs) {
    prepared_.clear();
    if (reqs.size() < 2 || cur_.refined() || pool::threads() < 2) return;
    std::set<SoftKey> keys;
    std::vector<const DecodeRequest*> todo;
    for (const auto& q : reqs) {
        if (q.slot >= n_cw_ || (n_ctl_slots_ && (q.mask.seq >= SEQ_MOD) != (q.slot < n_ctl_slots_))) continue;
        if (!keys.insert(q.key).second) return;
        todo.push_back(&q);
    }
    if (todo.size() < 2) return;
    std::vector<Prepared> out(todo.size());
    try {
        pool::parallel_for(todo.size(), [&](std::size_t t) {
            const DecodeRequest& q = *todo[t];
            const std::vector<double>* kept = nullptr;
            if (store_)
                if (const auto it = store_->find(q.key); it != store_->end() && it->second.submode == submode_) kept = &it->second.buf;
            int top = q.rv;
            if (kept) top = std::max(store_->find(q.key)->second.top, q.rv);
            out[t] = {q.rv, mask_value(q.mask), kept != nullptr,
                      decode_pass(spec(q.slot), dd(spec(q.slot)), q.slot, q.rv, mask_value(q.mask), top, (*soft_)[static_cast<std::size_t>(q.slot)], kept)};
        });
    } catch (...) {
        return;  // the sequential decodes will meet whatever it was themselves
    }
    for (std::size_t t = 0; t < todo.size(); ++t) prepared_[todo[t]->slot] = std::move(out[t]);
}

void ModemRx::learn(int slot, const codes::Spec& s, const Bytes& payload, int rv, std::uint32_t m) {
    if (dd(s)) post_[slot] = known(s, codes::encode(s, payload, rv, m, slot));
}

// DD after a failed decode of `slot`: its posterior joins the other slots'
// as soft pilots, the channel is re-estimated and the soft bits remade.
bool ModemRx::refine(int slot, const codes::Spec& s, std::vector<double> post) {
    ++refines_;
    post_[slot] = std::move(post);
    // The budget holds inside the work, not just between passes: a pass can cost seconds on a slow machine.
    const cancel::Expired expired = [this] { return late(); };
    const cancel::Scope scope(dd_until_ ? &expired : nullptr);
    try {
        remake_estimate(s);
        return true;
    } catch (const cancel::Cancelled&) {
        post_.erase(slot);  // a failed decode's posterior doesn't outlive the attempt
        dd_until_ = clock_();  // spent
        return false;
    }
}

// The blind pass (once), then the estimate re-made from every posterior held.
void ModemRx::remake_estimate(const codes::Spec& s) {
    if (!blind_) {
        // RV 0 slots the link has not asked about yet: parity checks alone
        // say they decoded (a resend at another RV simply fails).
        // split: one decode per slot on the pool, each stopping when it converges. Python decodes them as one
        // batch, which stops together: a burst's failing slots kept every converged one iterating to the limit.
        blind_ = true;
        if (n_ctl_slots_) {  // CPM: every slot's pass (a streaming receiver has made them)
            cpm_passes(n_cw_ - 1);
            auto cache = std::make_shared<std::map<int, std::vector<double>>>();
            cur_ = DdState{nullptr, cache, std::make_shared<const cpm::Posts>(post_)};
            return;
        }
        std::vector<int> todo;
        for (int i = n_ctl_slots_; i < n_cw_; ++i)
            if (!post_.count(i)) todo.push_back(i);
        std::vector<std::vector<double>> got(todo.size());
        pool::parallel_for(todo.size(), [&](std::size_t t) {
            const auto& v = (*soft_)[static_cast<std::size_t>(todo[t])];
            Mat<float> llr(1, v.size());
            std::copy(v.begin(), v.end(), llr[0]);
            const auto info = codes::decode_llrs(s, llr);
            if (info.ok[0]) got[t] = known(s, codes::encode_info(s, info.bits, 0).data);
        });
        for (std::size_t t = 0; t < todo.size(); ++t)
            if (!got[t].empty()) post_[todo[t]] = std::move(got[t]);
    }
    auto cache = std::make_shared<std::map<int, std::vector<double>>>();
    // CPM: the estimate is the posteriors themselves; each slot's metric is local (cpm::dd_metric), made as asked
    cur_ = n_ctl_slots_ ? DdState{nullptr, cache, std::make_shared<const cpm::Posts>(post_)}
                        : DdState{dd_estimate(), cache, nullptr};
}

// A failed decode's posterior can be confidently wrong, so neither it nor
// the estimate made from it outlives the attempt.
void ModemRx::undo(int slot, const DdState& saved) {
    post_.erase(slot);
    cur_ = saved;
}

// phy._dd_estimate: the burst's estimate re-made by equalizer::refine with
// post_'s soft symbols as pilots.
ModemRx::Est ModemRx::dd_estimate() const {
    const auto& R = *r_.ofdm;
    const auto& spec = *R.spec;
    const auto& est = R.est;
    const auto& pts = *constellation::find(spec.constellation);
    const int m = pts.m;
    const std::size_t M = pts.points.size(), cb = static_cast<std::size_t>(spec.coded_bits);
    std::vector<double> llr(static_cast<std::size_t>(n_cw_) * cb, 0.0);
    std::vector<std::uint8_t> have(llr.size(), 0);
    for (const auto& [s, v] : post_) {
        std::copy(v.begin(), v.end(), llr.begin() + static_cast<std::ptrdiff_t>(s * cb));
        std::fill_n(have.begin() + static_cast<std::ptrdiff_t>(s * cb), cb, 1);
    }
    const auto L = codes::spread<double>(llr, n_cw_, m);
    const auto K = codes::spread<std::uint8_t>(have, n_cw_, m);
    const std::size_t nc = R.raw.cols, rows = est.h.rows, n_sym = rows * nc;
    const double g = est.gain, p = est.p_sig;
    Mat<cd> z(rows, nc);
    Mat<double> w(rows, nc);
    std::vector<double> la1(static_cast<std::size_t>(m)), la0(static_cast<std::size_t>(m)), lp(M), prob(M);
    SymCache& cache = sym_cache_;
    if (cache.valid.size() != n_sym) {
        cache.L.assign(n_sym * static_cast<std::size_t>(m), 0.0);
        cache.z.assign(n_sym, 0.0);
        cache.w.assign(n_sym, 0.0);
        cache.valid.assign(n_sym, 0);
    }
    // split: per symbol, independent
    for (std::size_t j = 0; j < n_sym; ++j) {
        if ((j & 255) == 0) cancel::check();
        // no posterior for this symbol's codeword: no soft pilot (z, w stay 0), whatever the math says
        if (!K[j * m]) continue;
        const std::size_t row = j / nc, c = j % nc;
        double* cL = &cache.L[j * static_cast<std::size_t>(m)];
        if (cache.valid[j] && std::equal(cL, cL + m, &L[j * m])) {
            z[row][c] = cache.z[j];
            w[row][c] = cache.w[j];
            continue;
        }
        std::copy(&L[j * m], &L[j * m] + m, cL);
        cache.valid[j] = 1;
        cache.z[j] = 0.0;
        cache.w[j] = 0.0;
        // log P(label) per point, labels MSB first (modulate's order)
        for (int b = 0; b < m; ++b) {
            const double l = std::clamp(L[j * m + b], -30.0, 30.0);
            la1[b] = -std::log1p(std::exp(-std::abs(l))) - std::max(l, 0.0);   // -logaddexp(0, l)
            la0[b] = -std::log1p(std::exp(-std::abs(l))) - std::max(-l, 0.0);  // -logaddexp(0, -l)
        }
        double top = -INFINITY;
        for (std::size_t a = 0; a < M; ++a) {
            double s1 = 0.0, s0 = 0.0;
            for (int b = 0; b < m; ++b) {
                if ((a >> (m - 1 - b)) & 1)
                    s1 += la1[b];
                else
                    s0 += la0[b];
            }
            lp[a] = s1 + s0;
            top = std::max(top, lp[a]);
        }
        double sum = 0.0;
        for (std::size_t a = 0; a < M; ++a) sum += prob[a] = std::exp(lp[a] - top);
        cd x = 0.0;
        double e2 = 0.0;
        for (std::size_t a = 0; a < M; ++a) {
            prob[a] /= sum;
            x += prob[a] * pts.points[a];
            e2 += prob[a] * std::norm(pts.points[a]);
        }
        const double v = e2 - std::norm(x);
        if (!(std::abs(x) > 1e-3)) continue;
        cache.z[j] = modem::data_row(R.raw, row)[c] / (g * x);
        // modem::noise_var's with |h|^2 at its expectation g^2 p, plus the soft symbol's own variance
        const double n0 = est.n0_k.empty() ? est.n0 : est.n0_k[c];
        cache.w[j] = g * g * std::norm(x) / (n0 + est.clip_ratio * (g * g) * p + g * g * p * v);
        z[row][c] = cache.z[j];
        w[row][c] = cache.w[j];
    }
    const auto t = modem::symbol_times(R);
    auto [h, mse] = equalizer::refine(R.hp, t.pilots, z, w, t.rows, R.support, est.p_sig, est.filter_spread_hz, est.n0,
                                      equalizer::bb(config::band(spec.band)), est.slack);
    auto out = std::make_shared<modem::DataEstimate>(est);
    for (std::size_t i = 0; i < h.data.size(); ++i) {
        out->h.data[i] = g * h.data[i];
        out->mse.data[i] = g * g * mse.data[i];
    }
    return out;
}

}  // namespace data2g::arq
