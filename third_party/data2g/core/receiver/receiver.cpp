#include "receiver/receiver.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <string>

#include "dsp/dsp.hpp"
#include "waveform/dsp.hpp"

namespace data2g::receiver {

using std::int64_t;

namespace {

const cpm::Grid& grid_named(std::string_view name) {
    const auto* g = cpm::grid(name);
    if (!g) throw std::out_of_range("no CPM grid " + std::string(name));
    return *g;
}

int64_t grid_span(const cpm::Grid& g) {
    const auto lay = cpm::layout(g, cpm::stream_symbols(g, 0, false));
    return static_cast<int64_t>(lay.hdr_rows[0].back() + 2) * g.T;
}

double mean(std::span<const double> v) { return dsp::pairwise_sum(v) / static_cast<double>(v.size()); }

}  // namespace

int64_t search_span(std::span<const std::string_view> bands, std::span<const std::string_view> cpm_grids) {
    int64_t n = 0;
    for (const auto b : bands) n = std::max<int64_t>(n, modem::head_samples(b));
    for (const auto g : cpm_grids) n = std::max(n, grid_span(grid_named(g)));
    return n;
}

std::optional<Rx> receive_any(std::span<const double> y, int64_t lead,
                              std::optional<std::vector<std::string_view>> cpm_grids) {
    std::vector<std::string_view> grids;
    if (cpm_grids) grids = *cpm_grids;
    else
        for (const auto& g : tables::CPM_GRIDS) grids.push_back(g.name);
    std::vector<std::string_view> all_bands;
    for (const auto& b : config::BANDS) all_bands.push_back(b.name);
    const int64_t head = lead + search_span(all_bands, grids);
    const auto len = static_cast<int64_t>(y.size());
    std::optional<modem::Received> r;
    try {
        r = modem::receive(y, {}, nullptr, std::min(head, len));
        if (r->score >= Receiver::SUSPECT_SCORE) return Rx(std::move(*r));
    } catch (const modem::SyncError&) {
        r.reset();
    }
    for (const auto g : grids) {
        const auto& gr = grid_named(g);
        auto lock = cpm::find(gr, y.first(static_cast<std::size_t>(std::clamp<int64_t>(head, 0, len))), std::nullopt,
                              150.0, true, 0, std::nullopt, true);
        if (lock && lock->wait)  // its first header copy faded: both, as Receiver waits for
            lock = lock->header_end <= len ? cpm::late_lock(gr, y, *lock) : std::nullopt;
        if (lock) return Rx(cpm::receive(y, *lock));  // a CPM lock beats a suspect OFDM header
    }
    for (const auto g : grids) {  // the front missed: the mid block and both header copies (as Receiver's find_cpm)
        const auto& gr = grid_named(g);
        const int64_t n = std::clamp<int64_t>(lead + cpm::hdr_span(gr, 2) + gr.T, 0, len);
        const auto lock = cpm::find_mid(gr, y.first(static_cast<std::size_t>(n)));
        if (lock && lock->start <= lead) return Rx(cpm::receive(y, *lock));
    }
    if (!r) {
        // no preamble: the header copy of a burst whose head faded (as Receiver's find_copy)
        std::optional<modem::Lock> best;
        for (const auto b : config::HEADER_COPY_BANDS) {
            const auto c = modem::find_copy(y, b);
            if (c && c->start <= lead && c->end <= len && (!best || c->score > best->score)) best = c;
        }
        if (best) {
            try {
                return Rx(modem::receive(y, {}, nullptr, std::nullopt, &*best));
            } catch (const modem::SyncError&) {
            }
        }
    }
    if (r) return Rx(std::move(*r));
    return std::nullopt;
}

int64_t Pending::start() const { return is_cpm() ? cpm().start : ofdm().start; }
int64_t Pending::end() const { return is_cpm() ? cpm().end : ofdm().end; }
double Pending::score() const { return is_cpm() ? cpm().score : ofdm().score; }
int Pending::n_cw() const { return is_cpm() ? 1 + cpm().dup + cpm().n_data : ofdm().n_cw; }

void Pending::shift(int64_t d) {
    if (is_cpm()) {
        auto& l = std::get<1>(lock);
        l.start += d;
        l.end += d;
    } else {
        auto& l = std::get<0>(lock);
        l.start += d;
        l.end += d;
        l.p0 += d;
        if (l.copy) l.copy->pc += d;
    }
}

BurstEvent decode(DecodeRequest req, const modem::Accept& accept) {
    BurstEvent ev{req.header, std::nullopt, {}};
    Pending p = req.header;
    p.shift(-req.at);
    const auto t0 = std::chrono::steady_clock::now();
    if (p.is_cpm()) {
        ev.rx = req.stream ? req.stream->finish() : cpm::receive(req.seg, p.cpm());
    } else {
        try {
            if (p.copy()) {
                ev.rx = modem::receive(req.seg, {}, &accept, std::nullopt, &p.ofdm());
            } else {
                const std::string_view band[] = {p.ofdm().band};
                ev.rx = modem::receive(req.seg, band, &accept, std::min<int64_t>(req.head, static_cast<int64_t>(req.seg.size())));
            }
        } catch (const modem::SyncError&) {
            // the burst is lost: rx stays empty
        }
    }
    ev.receive_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (req.audio_lo == 0 && req.audio_hi == static_cast<int64_t>(req.seg.size())) ev.audio = std::move(req.seg);
    else ev.audio.assign(req.seg.begin() + req.audio_lo, req.seg.begin() + req.audio_hi);
    return ev;
}

void run(StreamFeed& f) {
    if (f.stream) f.stream->feed(f.audio);
}

Receiver::Receiver(modem::Accept accept, std::vector<std::string_view> cpm_grids, bool blank)
    : accept_(std::move(accept)), bands_(accept_.bands()) {
    for (const auto g : cpm_grids) grids_.push_back(&grid_named(g));
    // the least audio worth searching (a whole preamble and header), and
    // what a trim keeps (so one still arriving survives it)
    min_search_ = search_span(bands_, cpm_grids);
    keep_ = min_search_ + config::FS;
    // what a trim with nothing found holds: a CPM burst's start until its
    // mid block and second header copy are in (find_mid)
    hold_ = keep_;
    for (const auto* g : grids_) hold_ = std::max(hold_, cpm::hdr_span(*g, 2) + config::FS);
    detectors_.reserve(bands_.size());
    for (const auto b : bands_) detectors_.push_back({b, waveform::StreamDetector(waveform::band(b))});
    if (blank) blanker_.emplace();
    reset();
}

void Receiver::reset() {
    buf_.clear();
    off_ = 0;
    pending_.reset();
    stream_.reset();
    pilots_ok_ = confirmed_ = false;
    dark_ = 0;
    looked_ = false;
    powers_.clear();
    ps_ = 0.0;
    pn_ = 0;
    fresh_ = HOP;  // search at once
    last_start_ = -1;
    decided_.clear();
    decided_mid_.clear();
    cpm_wait_.clear();
    expected_.reset();
    for (auto& d : detectors_) d.d.reset();
}

void Receiver::expect(std::vector<std::string_view> grids, std::int64_t lo, std::int64_t hi, double cfo) {
    expected_.reset();
    if (grids.empty()) return;
    Expected e{{}, lo, hi, cfo};
    for (auto g : grids) e.grids.push_back(cpm::grid(g));
    expected_ = std::move(e);
}

std::optional<cpm::Lock> Receiver::primed() {
    Expected& e = *expected_;
    const cpm::Grid& g = *e.grids.front();
    if (off_ + len() < e.hi + cpm::hdr_span(g, 2) + g.T) return std::nullopt;
    const std::int64_t lo = e.lo, hi = e.hi;
    const double cfo = e.cfo;
    e.grids.erase(e.grids.begin());
    if (e.grids.empty()) expected_.reset();
    if (hi < off_) return std::nullopt;  // trimmed away (an expect() later than its window)
    return cpm::prime_lock(g, buf_, std::max<std::int64_t>(0, lo - off_), hi - off_, cfo);  // (receiver.py logs it)
}

void ReplySlot::heard(std::optional<double> gap_s, std::optional<double> c) {
    if (gap_s && *gap_s >= 0.0 && *gap_s < MAX_GAP_S) {
        gaps_.push_back(*gap_s);
        if (gaps_.size() > GAPS) gaps_.pop_front();
    }
    if (c) cfo = c;
}

std::optional<std::pair<double, double>> ReplySlot::window(double tx_end) const {
    if (gaps_.empty() || !cfo) return std::nullopt;
    std::vector<double> v(gaps_.begin(), gaps_.end());
    std::sort(v.begin(), v.end());
    const std::size_t n = v.size();
    const double g = n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;  // np.median
    return std::make_pair(tx_end + g - TOL_S, tx_end + g + TOL_S);
}

std::optional<std::pair<double, double>> ReplySlot::nod_window(double tx_end) const {
    const auto w = window(tx_end);
    if (!w) return std::nullopt;
    const double g = w->first + TOL_S - tx_end;
    const auto [mn, mx] = std::minmax_element(gaps_.begin(), gaps_.end());
    return std::make_pair(tx_end + std::max(*mn - TOL_S, g - NOD_HALF_S), tx_end + std::min(*mx + TOL_S, g + NOD_HALF_S));
}

bool Receiver::on_air() const {
    if (powers_.size() < 50) return false;
    const double floor = dsp::quantile(std::vector<double>(powers_.begin(), powers_.end()), 0.05);
    const double last[] = {powers_[powers_.size() - 3], powers_[powers_.size() - 2], powers_.back()};
    return mean(last) > floor * std::pow(10.0, ON_AIR_DB / 10);
}

int64_t Receiver::decided(std::string_view key) const {
    const auto it = decided_.find(key);
    return it == decided_.end() ? 0 : it->second;
}

void Receiver::check_pilots() {
    const Pending& p = *pending_;
    if (p.is_cpm()) return;
    modem::Lock l = p.ofdm();
    l.p0 -= off_;  // only p0, as Python's dict(p, p0=...)
    double pat = 0.0;
    const auto c = modem::pilot_coherence(buf_, l, modem::PILOT_PAIRS, true, &pat);
    if (static_cast<int>(c.size()) >= modem::PILOT_PAIRS) {
        const bool ok = mean(c) > modem::pilot_noise(l.spec->band) && pat > modem::pilot_pattern(l.spec->band);
        dark_ = ok ? 0 : dark_ + 1;
        // a copy lock is one header copy: confirmed (no further search) at
        // the single-copy commit score
        confirmed_ = ok && p.score() >= (p.copy() ? modem::COPY_COMMIT_SCORE : SUSPECT_SCORE);
        pilots_ok_ = ok;
    }
}

// Python's buf = buf[-n:] if n < len(buf), off advanced by len(buf) - n.
void Receiver::trim(int64_t n) {
    const int64_t L = len();
    if (n < L) {
        off_ += L - n;
        const int64_t a = n > 0 ? L - n : n == 0 ? 0 : std::min(-n, L);  // buf[-n:]
        buf_.erase(buf_.begin(), buf_.begin() + a);
    }
    for (auto& d : detectors_) d.d.trim(off_);
}

Receiver::Stats Receiver::stats(int64_t w0) {
    Stats out;
    out.mats.reserve(detectors_.size());
    for (auto& [band, d] : detectors_) {
        if (d.fed < off_) d.restart_at(off_);  // its audio was trimmed away: start over
        auto from = static_cast<std::size_t>(d.fed - off_);
        // A confirmed burst isn't searched, so its audio goes unfed until a search is wanted again (a late
        // flip to unconfirmed): the catch-up is the burst's whole length in one step, and the trim just below
        // throws all but the last keep_ away. Start that far back instead (a span more, for the rows' overlap).
        const auto want = static_cast<std::size_t>(std::max<int64_t>(0, len() - keep_ - 2 * d.span));
        if (from < want) {
            d.skip_to(d.fed + static_cast<int64_t>(want - from));
            from = want;
        }
        d.feed(waveform::to_baseband(std::span<const double>(buf_).subspan(from), d.fed));
        // no search looks further back than keep_ (the statistic grew with
        // a long burst in the buffer)
        d.trim(off_ + len() - keep_ - d.span);
        const int64_t n = len() - w0 - d.span + 1;
        if (n <= 0) continue;
        const int64_t lo = off_ + w0;
        Mat<double> S = d.stat(lo, lo + n);
        const auto masked = static_cast<std::size_t>(std::clamp<int64_t>(decided(band) - lo, 0, n));
        for (std::size_t r = 0; r < S.rows; ++r) std::fill(S[r], S[r] + masked, -1.0);
        out.mats.push_back(std::move(S));
        out.v.push_back({band, &out.mats.back()});
    }
    return out;
}

void Receiver::searched() {
    const int64_t end = off_ + len();
    for (const auto& det : detectors_) {
        const auto& b = config::band(det.band);
        decided_[det.band] = std::max(decided(det.band),
                                      end - modem::head_samples(det.band) - config::preamble_samples(b) - REVISIT);
    }
    for (const auto* g : grids_) {
        decided_[g->name] = std::max(decided(g->name), end - grid_span(*g) - g->T);
        // a mid block is decided once its second header copy is in
        decided_mid_[g->name] = std::max(
            decided_mid(*g), end - (static_cast<int64_t>(g->mid_block.size()) + g->hdr_len) * g->T - g->T);
    }
}

std::optional<modem::Lock> Receiver::find_copy() const {
    std::optional<modem::Lock> best;
    int64_t best_k = 0;
    for (const auto band : config::HEADER_COPY_BANDS) {
        const auto it = std::find_if(detectors_.begin(), detectors_.end(), [&](const Detector& d) { return d.band == band; });
        if (it == detectors_.end()) continue;
        const auto& d = it->d;
        const auto level = d.level();
        if (!level) continue;
        const auto c = d.outputs_from(off_);  // from the stream index both the buffer and C cover
        const int64_t a = c.start;
        const auto x = std::span<const double>(buf_).subspan(static_cast<std::size_t>(a - off_));
        const auto cols = static_cast<std::size_t>(std::max<int64_t>(
            0, std::min(static_cast<int64_t>(c.cols), static_cast<int64_t>(x.size()) - config::M + 1)));
        const auto lock = modem::find_copy(x, band, &accept_, c.rows, cols, level);
        if (lock && (!best || lock->score > best->score)) {
            best = lock;
            best_k = a - off_;
        }
    }
    if (best) {
        Pending p{*best};
        p.shift(best_k);
        return p.ofdm();
    }
    return best;
}

std::optional<cpm::Lock> Receiver::find_cpm() {
    // a waiting candidate's lock (both header copies) once its second copy is in
    std::erase_if(cpm_wait_, [&](const CpmWait& w) { return w.first < off_; });  // trimmed with a burst handled
    for (std::size_t i = 0; i < cpm_wait_.size();) {
        if (cpm_wait_[i].cand.header_end > off_ + len()) {
            ++i;
            continue;
        }
        const CpmWait w = cpm_wait_[i];
        cpm_wait_.erase(cpm_wait_.begin() + static_cast<std::ptrdiff_t>(i));
        cpm::Lock c = w.cand;
        c.start -= off_;
        c.header_end -= off_;
        c.end -= off_;
        if (auto lock = cpm::late_lock(*w.g, buf_, c)) return lock;
    }
    for (const auto* g : grids_) {
        auto lock = cpm::find(*g, buf_, std::nullopt, 150.0, true, std::max<int64_t>(0, decided(g->name) - off_),
                              len() - grid_span(*g) + 2 * g->T, true);
        if (!lock) continue;
        if (!lock->wait) return lock;
        lock->start += off_;
        lock->header_end += off_;
        lock->end += off_;
        // the next search may meet the same front again before it is decided
        const bool known = std::any_of(cpm_wait_.begin(), cpm_wait_.end(), [&](const CpmWait& w) {
            return w.g == g && std::abs(w.cand.start - lock->start) < cpm::hdr_span(*g, 1);
        });
        if (!known) cpm_wait_.push_back({g, *lock, lock->header_end - cpm::hdr_span(*g, 2)});
    }
    for (const auto* g : grids_) {  // the front missed: the mid block and both header copies
        const auto lock = cpm::find_mid(*g, buf_, std::max<int64_t>(0, decided_mid(*g) - off_),
                                        len() - (static_cast<int64_t>(g->mid_block.size()) + g->hdr_len - 1) * g->T);
        if (lock) return lock;
    }
    return std::nullopt;
}

int64_t Receiver::decided_mid(const cpm::Grid& g) const {
    const auto it = decided_mid_.find(g.name);
    return it == decided_mid_.end() ? 0 : it->second;
}

bool Receiver::supersede(std::vector<Item>& out, bool whole) {
    fresh_ %= HOP;  // searches on the stream's HOP grid (receiver.py's fresh)
    const Pending p = *pending_;
    if (p.is_cpm()) return false;  // scores aren't comparable across families
    std::optional<Pending> q;
    // under a suspect header, any CPM lock replaces it (a false n4 header
    // claiming 52 s hid three FSK bursts: recordings/20261024-190009)
    if (!p.copy() && p.score() < SUSPECT_SCORE) {
        if (auto c = find_cpm()) {  // (else the OFDM search below marks both searched)
            searched();
            q = Pending{*c};
            q->shift(off_);
        }
    }
    if (!q) {
        const auto band = p.ofdm().band;
        const int64_t hdr_end = p.start() + config::preamble_samples(config::band(band)) + modem::header_samples(band) - off_;
        const int64_t w0 = whole ? hdr_end : std::max(hdr_end, len() - keep_);
        if (len() - w0 < keep_ / 2) return false;
        try {
            const auto st = stats(w0);
            auto l = modem::find_burst(std::span<const double>(buf_).subspan(static_cast<std::size_t>(w0)), bands_, &accept_, st.v);
            q = Pending{l};
            q->shift(w0 + off_);
        } catch (const modem::SyncError&) {
        }
        searched();
        if (p.copy() && (!q || q->score() < p.score() + SUPERSEDE_MARGIN)) {
            // a copy lock can be a copy read off the wrong frame, taken before
            // the burst's own copy arrived: the true one, later, replaces it
            if (auto c = find_copy()) {
                q = Pending{*c};
                q->shift(off_);
            }
        }
        if (!q || q->score() < p.score() + SUPERSEDE_MARGIN || q->start() == p.start()) return false;
    }
    if (!whole) {  // completing: the caller has already handled p
        const auto a = static_cast<std::size_t>(std::max<int64_t>(0, p.start() - off_));
        const auto b = static_cast<std::size_t>(std::clamp<int64_t>(q->start() - off_, 0, len()));
        out.push_back(BurstEvent{p, std::nullopt, std::vector<double>(buf_.begin() + a, buf_.begin() + std::max(a, b))});
    }
    pending_ = q;
    pilots_ok_ = q->is_cpm() || q->copy() || q->score() >= SUSPECT_SCORE;
    confirmed_ = false;
    dark_ = 0;
    looked_ = false;
    out.push_back(HeaderEvent{*pending_, off_ + len()});
    return true;
}

bool Receiver::release(std::vector<Item>& out) {
    const Pending& p = *pending_;
    if (release_s <= 0 || p.is_cpm() || p.score() >= (p.copy() ? modem::COPY_COMMIT_SCORE : SUSPECT_SCORE) ||
        static_cast<double>(dark_) * HOP < release_s * config::FS)
        return false;
    const auto a = static_cast<std::size_t>(std::clamp<int64_t>(p.start() - off_, 0, len()));
    out.push_back(BurstEvent{p, std::nullopt, std::vector<double>(buf_.begin() + a, buf_.end())});
    drop();
    // as a suspect burst's end: keep the last search window and every CPM front waiting
    int64_t keep = keep_;
    for (const auto& w : cpm_wait_) keep = std::max(keep, len() - (w.first - off_));
    trim(keep);
    return true;
}

// A CPM early lock (front + header copy 1), once its mid block and second
// header copy are in: under cpm::CONFIRM_LLR there the burst is not there,
// and it is let go as release lets an OFDM one (receiver.Receiver._unconfirmed).
bool Receiver::unconfirmed(std::vector<Item>& out) {
    const Pending& p = *pending_;
    if (!p.is_cpm() || looked_) return false;
    const cpm::Lock& l = p.cpm();
    const cpm::Grid& g = *cpm::grid(l.spec->grid);
    if (l.copies >= tables::CPM.hdr_copies) {  // (a late, mid or primed lock)
        looked_ = true;
        return false;
    }
    if (off_ + len() < cpm::confirm_end(g, l) + g.T) return false;
    looked_ = true;
    cpm::Lock here = l;
    here.start -= off_;
    if (cpm::confirm(g, buf_, here) >= cpm::CONFIRM_LLR) return false;
    const auto a = static_cast<std::size_t>(std::clamp<int64_t>(p.start() - off_, 0, len()));
    out.push_back(BurstEvent{p, std::nullopt, std::vector<double>(buf_.begin() + a, buf_.end())});
    drop();
    int64_t keep = keep_;
    for (const auto& w : cpm_wait_) keep = std::max(keep, len() - (w.first - off_));
    trim(keep);
    return true;
}

std::vector<Event> Receiver::feed(std::span<const double> x) {
    std::vector<Event> out;
    for (auto& it : feed_deferred(x)) {
        if (auto* r = std::get_if<DecodeRequest>(&it)) out.emplace_back(decode(std::move(*r), accept_));
        else if (auto* h = std::get_if<HeaderEvent>(&it)) out.emplace_back(std::move(*h));
        else if (auto* f = std::get_if<StreamFeed>(&it)) run(*f);
        else out.emplace_back(std::move(std::get<BurstEvent>(it)));
    }
    return out;
}

std::vector<Receiver::Item> Receiver::feed_deferred(std::span<const double> x_in) {
    std::vector<double> blanked;
    std::span<const double> x = x_in;
    if (blanker_) {
        blanked = (*blanker_)(x_in);
        x = blanked;
    }
    // 0.1 s block powers for the noise floor (feeds may be any size)
    {
        std::vector<double> sq(x.size());
        for (std::size_t i = 0; i < x.size(); ++i) sq[i] = x[i] * x[i];
        std::span<const double> rest = sq;
        while (!rest.empty()) {
            const auto take = std::min<std::size_t>(rest.size(), static_cast<std::size_t>(config::FS / 10 - pn_));
            ps_ += dsp::pairwise_sum(rest.first(take));
            pn_ += static_cast<int>(take);
            rest = rest.subspan(take);
            if (pn_ == config::FS / 10) {
                powers_.push_back(ps_ / pn_);
                if (powers_.size() > FLOOR_BLOCKS) powers_.pop_front();
                ps_ = 0.0;
                pn_ = 0;
            }
        }
    }
    buf_.insert(buf_.end(), x.begin(), x.end());
    fresh_ += static_cast<int64_t>(x.size());
    std::vector<Item> out;
    if (stream_) out.push_back(StreamFeed{stream_, std::vector<double>(x.begin(), x.end())});
    while (true) {
        if (!pending_) {
            // a burst's end is checked on every call; new preambles only every HOP samples
            if (len() < min_search_ || fresh_ < HOP) break;
            fresh_ %= HOP;
            // digital silence has no noise to normalize by, and filter
            // ringing then reads as preambles
            {
                const auto tail = std::span<const double>(buf_).last(static_cast<std::size_t>(min_search_));
                std::vector<double> sq(tail.size());
                for (std::size_t i = 0; i < tail.size(); ++i) sq[i] = tail[i] * tail[i];
                if (std::sqrt(mean(sq)) < SILENCE_RMS) {
                    trim(keep_);
                    for (auto& d : detectors_) d.d.restart_at(off_ + len());  // no noise level in silence: skip it
                    break;
                }
            }
            std::optional<Pending> p;
            try {
                const auto st = stats();
                p = Pending{modem::find_burst(buf_, bands_, &accept_, st.v)};
                if (p->score() < SUSPECT_SCORE)
                    if (auto c = find_cpm()) p = Pending{*c};  // a strong CPM burst read as a weak OFDM header
            } catch (const modem::SyncError&) {
                if (auto c = find_cpm()) p = Pending{*c};
            }
            searched();
            if (!p)
                if (auto c = find_copy()) p = Pending{*c};  // the preamble faded: the frame pilots and header copy
            if (!p && expected_)
                if (auto c = primed()) p = Pending{*c};
            if (!p) {
                // (a waiting candidate's start is within it; and the expected window, while queued)
                trim(expected_ ? std::max(hold_, off_ + len() - expected_->lo) : hold_);
                break;
            }
            expected_.reset();
            if (p->start() + off_ <= last_start_) break;  // the suspect burst just handled, still in the kept window
            p->shift(off_);
            pending_ = p;
            // BUSY at once on a clear header (or a copy lock: its pilots
            // passed already); a suspect one waits for its pilots
            pilots_ok_ = p->is_cpm() || p->copy() || p->score() >= SUSPECT_SCORE;
            confirmed_ = false;
            dark_ = 0;
            looked_ = false;
            out.push_back(HeaderEvent{*p, off_ + len()});
            if (stream_on_ && p->is_cpm()) {  // the buffer as decode() will read it (origin off_, untrimmed till then)
                Pending q = *p;
                q.shift(-off_);
                stream_ = std::make_shared<cpm::Stream>(buf_, q.cpm(), stream_pass_);
                out.push_back(StreamFeed{stream_, {}});
            }
        }
        const Pending p = *pending_;
        if (off_ + len() < p.end() + MARGIN) {
            if (fresh_ >= HOP) {
                check_pilots();
                // the later-header search is for false locks: a confirmed burst skips it
                if (confirmed_) fresh_ %= HOP;
                else if (supersede(out)) continue;
                else if (release(out) || unconfirmed(out)) continue;
            }
            break;
        }
        DecodeRequest req;
        req.header = p;
        if (p.is_cpm()) {
            req.seg = buf_;
            req.at = off_;
            req.audio_lo = std::max<int64_t>(0, p.start() - off_);
            req.audio_hi = std::max(req.audio_lo, std::min(p.end() - off_, len()));
            req.stream = std::move(stream_);
            out.push_back(std::move(req));
            last_start_ = p.start();
            // a CPM burst has no lead-out silence: with the sender's clock fast,
            // the next back-to-back burst's front begins before this one's
            // computed end (receiver.Receiver.feed)
            trim(len() - (p.end() - CPM_TAIL - off_));
            pending_.reset();
            continue;
        }
        const int64_t s0 = std::max<int64_t>(0, p.start() - MARGIN - off_);
        const int64_t s1 = std::clamp<int64_t>(p.end() + MARGIN - off_, s0, len());
        req.seg.assign(buf_.begin() + s0, buf_.begin() + s1);
        req.at = off_ + s0;
        req.head = p.start() - off_ - s0 + modem::receive_head(p.ofdm().band);
        req.audio_hi = s1 - s0;
        out.push_back(std::move(req));
        last_start_ = p.start();
        // a better header inside this burst's span (it was a false lock, and
        // the real one began before it ended) is next, not trimmed away
        if (!confirmed_ && supersede(out, true)) {
            trim(len() - std::max<int64_t>(0, pending_->start() - MARGIN - off_));
            continue;
        }
        // a suspect burst may have claimed an end past a real preamble that
        // began inside it: keep the last search window (a confident one
        // keeps nothing of itself), and every CPM front waiting inside it
        int64_t cut = p.end() - off_;
        if (p.score() < SUSPECT_SCORE) {
            cut = std::min(cut, len() - keep_);
            for (const auto& w : cpm_wait_) cut = std::min(cut, w.first - off_);
        }
        trim(len() - std::max<int64_t>(0, cut));
        pending_.reset();
    }
    return out;
}

}  // namespace data2g::receiver
