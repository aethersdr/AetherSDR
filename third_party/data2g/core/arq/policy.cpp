#include "arq/policy.hpp"

#include <algorithm>
#include <tuple>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <stdexcept>

#include "cpm/cpm.hpp"
#include "dsp/dsp.hpp"

namespace data2g::arq {

namespace {

// Python's round(x, 2): the correctly rounded decimal, ties to even.
double round2(double x) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.2f", x);
    return std::strtod(buf, nullptr);
}

double logit_shift(double q, double b) {
    q = std::clamp(q, 1e-6, 1 - 1e-6);
    return 1 / (1 + std::exp(-(std::log(q / (1 - q)) + b)));
}

double get(const GearShifter::Map& m, std::string_view k) {
    const auto it = m.find(k);
    return it == m.end() ? 0.0 : it->second;
}

int cdiv(std::int64_t a, std::int64_t b) { return static_cast<int>((a + b - 1) / b); }  // a >= 0, b > 0

}  // namespace

const std::array<double, 4>& mode_thresholds(std::string_view submode) {
    for (const auto& t : tables::MODE_THRESHOLDS)
        if (t.name == submode) return t.db;
    throw std::out_of_range("no ladder thresholds for " + std::string(submode));
}

std::string_view fallback(int cap) {
    cap_hz(cap);
    return cap == 0 ? "n10-ack-4f" : "ack-4f";
}

std::string_view connect_mode(int cap, int tries) {
    cap_hz(cap);
    if (tries) return ROBUST_CONNECT;
    return cap == 0 ? "n10-qpsk-r1/3" : "qpsk-r1/5";
}

int ctl_slots(const Mode& m) { return std::min(max_ctl(m), cdiv(CTL_BYTES, ctl_payload_bytes(m))); }

int slots_for(const Mode& m, double seconds, bool data, bool dup) {
    if (m.is_cpm()) seconds = std::min(seconds * CPM_SIZE_SCALE, CPM_MAX_S);
    const int lim = m.is_cpm() ? 64 : config::max_codewords(m.ofdm->sync_band);
    int n = 1;
    while (n < lim && burst_seconds(m, n + 1) <= seconds) ++n;
    n = std::max({n, min_cw(m, data), ctl_slots(m) + data});
    if (m.is_cpm()) n = std::min(n + (dup && data), 1 + dup + tables::CPM.max_data);
    return n;
}

std::pair<std::string_view, int> GearShifter::choose(const StationView& st, int escalation) const {
    const auto rec = st.pending ? st.peer_recommend : st.peer_reply_recommend;
    const auto ok = allowed(st.cap);
    // escalation 1 and 3: the peer's reply mode; 2: ALT_POLL; 4 on, answering
    // the peer's robust poll, or my floor there with no data: control only in
    // ROBUST_CONNECT (policy.py)
    const bool robust_floor = st.esc_floor >= ROBUST_ESCALATION && !st.pending;
    if (escalation >= ROBUST_ESCALATION || (escalation && heard == ROBUST_CONNECT) || robust_floor)
        return {ROBUST_CONNECT, 1};
    if (escalation == 2) return {ALT_POLL, 2};
    if (escalation) {
        const Mode* r = st.peer_reply_recommend ? decode(*st.peer_reply_recommend) : nullptr;
        return {r && std::find(ok.begin(), ok.end(), r) != ok.end() ? r->name : fallback(st.cap), 2};
    }
    if (!rec) return {fallback(st.cap), 2};
    const Mode* m = decode(*rec);
    if (!m || std::find(ok.begin(), ok.end(), m) == ok.end()) return {fallback(st.cap), 2};
    return {m->name, slots_for(*m, SIZE_S.at(st.peer_size_hint), st.pending, st.peer_wants_dup)};
}

int GearShifter::next_capacity(const StationView& st) const {
    const Mode* m = st.peer_recommend ? decode(*st.peer_recommend) : nullptr;
    const auto ok = allowed(st.cap);
    if (!m || std::find(ok.begin(), ok.end(), m) == ok.end()) m = &mode_at(fallback(st.cap));
    const int n = slots_for(*m, SIZE_S.at(st.peer_size_hint));
    return (n - ctl_slots(*m)) * payload_bytes(*m);
}

void GearShifter::observe(const Measured& m_in, std::string_view submode, double now) {
    // a burst too short or weak for a Doppler estimate reports the 2.0 Hz
    // default (what MPD measures): it takes the peer's last measured spread
    Measured m = m_in;
    if (m.spread_est != UNMEASURED_SPREAD_HZ)
        last_spread = m.spread_est;
    else if (last_spread)
        m.spread_est = *last_spread;
    if (measured) prev = Heard{*measured, measured_band, measured_at};
    measured = m;
    measured_band = std::string(mode_at(submode).band);
    measured_at = now;
    heard = std::string(submode);
    spreads.push_back(m.spread_est);
    snrs.push_back(m.snr_est);
    if (snrs.size() > CPM_FLOOR_HIST) snrs.erase(snrs.begin(), snrs.end() - CPM_FLOOR_HIST);
    if (spreads.size() > static_cast<std::size_t>(tables::OUTCOME_GATE_HIST))
        spreads.erase(spreads.begin(), spreads.end() - tables::OUTCOME_GATE_HIST);
}

void GearShifter::observe_energy(double snr_db, double) {
    energies.push_back(snr_db);
    if (energies.size() > ENERGY_HIST) energies.erase(energies.begin(), energies.end() - ENERGY_HIST);
}

std::optional<std::array<double, 3>> GearShifter::energy_features() const {
    if (energies.empty()) return std::nullopt;
    std::vector<double> lin;
    for (const double e : energies) lin.push_back(std::pow(10.0, e / 10));
    const double mean = dsp::pairwise_sum(lin) / static_cast<double>(lin.size());
    return std::array<double, 3>{10 * std::log10(mean), static_cast<double>(energies.size()) / ENERGY_HIST, 1.0};
}

std::optional<std::pair<std::string, double>> GearShifter::expected_reply() const {
    if (log.empty()) return std::nullopt;
    const auto& e = log.back();
    if (peer_had_data) {
        const Mode& m = mode_at(e.data);
        return std::pair{e.data, burst_seconds(m, slots_for(m, SIZE_S.at(static_cast<std::size_t>(e.hint))))};
    }
    const Mode& m = mode_at(e.reply);
    return std::pair{e.reply, burst_seconds(m, ctl_slots(m))};
}

bool GearShifter::cpm_floor() const {
    if (!cpm_floor_on || snrs.empty() || spreads.empty()) return false;
    const auto median = [](std::vector<double> v) {  // np.median
        std::sort(v.begin(), v.end());
        const std::size_t n = v.size();
        return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
    };
    const std::size_t k = std::min(spreads.size(), CPM_FLOOR_HIST);
    if (median(std::vector<double>(spreads.end() - static_cast<std::ptrdiff_t>(k), spreads.end())) < CPM_FLOOR_SPREAD_HZ)
        return false;
    return median(snrs) < CPM_FLOOR_SNR_DB;
}

int GearShifter::gate() const {
    if (spreads.empty() || !measured) return 0;
    std::vector<double> v = spreads;  // np.median: the middle one, or the mean of the middle two
    std::sort(v.begin(), v.end());
    const std::size_t n = v.size();
    const double med = n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
    const double snr = measured->snr_est;
    if (!tables::OUTCOME_GATE_MEMBERS.empty() && med < tables::OUTCOME_GATE_SPREAD_HZ &&
        snr < tables::OUTCOME_GATE_SNR_DB)
        return 1;
    if (!tables::OUTCOME_FAST_MEMBERS.empty() && med >= tables::OUTCOME_FAST_SPREAD_HZ && snr >= tables::OUTCOME_FAST_SNR_LO_DB &&
        snr < tables::OUTCOME_FAST_SNR_HI_DB)
        return 2;
    return 0;
}

double GearShifter::reply_hold(const StationView& st, std::string_view submode) const {
    std::vector<std::string_view> ms = {log.empty() ? fallback(st.cap) : std::string_view(log.back().reply)};
    if (st.misses) ms.insert(ms.end(), {fallback(st.cap), ALT_POLL});
    if (submode == ROBUST_CONNECT || st.esc_floor >= ROBUST_ESCALATION) ms.push_back(ROBUST_CONNECT);
    double t = 0.0;
    for (auto m : ms) {
        const Mode& md = mode_at(m);
        t = std::max(t, burst_seconds(md, ctl_slots(md)));
    }
    return t + REPLY_HOLD_MARGIN_S;
}

void GearShifter::outcome(std::string_view submode, int decoded, int sent, std::optional<bool> usable) {
    if (usable && *usable && sent) {
        data_lost = 0;
        ladder_left = false;
        if (ceiling) {  // climb a step; back at the mode whose losses started it, it's off
            bool off = true;
            for (std::size_t i = 0; i < 4; ++i) {
                (*ceiling)[i] += LADDER_STEP_DB;
                off = off && (*ceiling)[i] >= (*ladder_top)[i];
            }
            if (off) ceiling.reset(), ladder_top.reset(), ladder_left = true;
        }
    } else if (usable && !*usable && !log.empty() && submode == log.back().data) {
        if (++data_lost >= LADDER_AFTER || ladder_left) {  // step down from the mode that failed
            ladder_left = false;
            std::array<double, 4> down = mode_thresholds(submode);
            for (std::size_t i = 0; i < 4; ++i) {
                down[i] -= LADDER_STEP_DB;
                if (ceiling) down[i] = std::min(down[i], (*ceiling)[i]);
            }
            if (!ceiling) ladder_top = mode_thresholds(submode);
            ceiling = down;
        }
    }
    const auto it = predicted.find(submode);
    if (it == predicted.end() || (sent == 0 && !usable)) return;
    const bool ok = usable ? *usable : decoded > 0;
    const auto [pb, p] = it->second;
    const std::string key(submode);
    bias_burst[key] = std::clamp(get(bias_burst, key) + BIAS_STEP * (ok - pb), -BIAS_MAX, BIAS_MAX);
    if (!ok || sent == 0) return;
    bias[key] = std::clamp(get(bias, key) + BIAS_STEP * (static_cast<double>(decoded) / sent - p), -BIAS_MAX, BIAS_MAX);
}

GearRecommendation GearShifter::recommend(const StationView& st) {
    if (!measured) {
        const int fb = encode(fallback(st.cap));
        return {fb, 1, fb};
    }
    std::vector<const Mode*> cands;
    for (const Mode* s : allowed(st.cap))
        if (outcome_knows(s->name) && (!s->is_cpm() || use_cpm)) cands.push_back(s);  // a mode added since: not until a model knows it
    std::optional<Prev> pv;
    // older history is used as PREV_MAX_S old, not dropped (policy.py)
    if (prev) pv = Prev{prev->m, prev->band, std::min(measured_at - prev->at, PREV_MAX_S)};

    // the noise rule: each candidate predicted as if its SNR were lower by
    // noise_shift_db (its band's noise in the profile against the measured band's)
    std::map<std::string, double, std::less<>> shift;
    if (noise_rule)
        for (const Mode* s : cands)
            if (!shift.count(s->band)) shift[std::string(s->band)] = noise_shift_db(measured->noise, measured_band, s->band, *noise_rule);
    // by the burst's rounded length, the noise rule's shift and the mode
    std::map<std::tuple<double, double, int>, Outcome> memo;
    const int gated = gate();
    Measured me = *measured;  // with the energy inputs (a model with them reads them)
    me.energy = energy_features();
    auto key_of = [&](const Mode& s, int n_cw) {
        const int i = outcome_index(s.name);
        if (i < 0) throw std::out_of_range("outcome model lacks " + std::string(s.name));
        const auto sh = shift.find(s.band);
        return std::tuple<double, double, int>{round2(burst_seconds(s, n_cw)), sh == shift.end() ? 0.0 : sh->second, i};
    };
    auto predicted_at = [&](const Mode& s, int n_cw) -> const Outcome& {
        const auto key = key_of(s, n_cw);
        auto it = memo.find(key);
        if (it == memo.end()) {
            const auto [sec, sh, i] = key;
            it = memo.emplace(key, predict_queries(shifted(me, sh), measured_band, gap_s, pv ? &*pv : nullptr, gated,
                                                   {{sec, i}})[0]).first;
        }
        return it->second;
    };
    auto q_burst = [&](const Mode& s, int n_cw) { return logit_shift(predicted_at(s, n_cw).burst, get(bias_burst, s.name)); };
    auto q_cw = [&](const Mode& s, int n_cw) { return logit_shift(predicted_at(s, n_cw).cw, get(bias, s.name)); };

    std::vector<const Mode*> data_cands = cands;
    if (ceiling && !cands.empty()) {
        data_cands.clear();
        for (const Mode* s : cands) {
            const auto& t = mode_thresholds(s->name);
            bool ok = true;
            for (std::size_t i = 0; i < 4; ++i) ok = ok && t[i] <= (*ceiling)[i];
            if (ok) data_cands.push_back(s);
        }
        if (data_cands.empty()) {  // none that robust: the lowest worst-case threshold
            auto worst = [](const Mode* s) {
                const auto& t = mode_thresholds(s->name);
                return *std::max_element(t.begin(), t.end());
            };
            data_cands = {*std::min_element(cands.begin(), cands.end(),
                                            [&](const Mode* a, const Mode* b) { return worst(a) < worst(b); })};
        }
    }
    if (cpm_floor()) {  // data in CPM modes only (policy.py CPM_FLOOR)
        std::vector<const Mode*> c;
        for (const Mode* s : data_cands)
            if (s->is_cpm()) c.push_back(s);
        if (c.empty())
            for (const Mode* s : cands)
                if (s->is_cpm()) c.push_back(s);
        if (!c.empty()) data_cands = std::move(c);
    }
    {  // every prediction the loops below ask for, in one batch (the weights read once);
       // predicted_at computes any other lazily (the chat size cut, the final pick)
        std::vector<std::tuple<double, double, int>> keys;
        auto want = [&](const Mode& s, int n_cw) {
            const auto key = key_of(s, n_cw);
            if (!memo.count(key) && std::find(keys.begin(), keys.end(), key) == keys.end()) keys.push_back(key);
        };
        for (const Mode* s : cands) want(*s, 1);
        for (const Mode* s : data_cands)
            for (const double sz : SIZE_S) want(*s, slots_for(*s, sz));
        std::map<double, std::vector<std::size_t>> by_shift;  // one batch per noise-rule shift
        for (std::size_t k = 0; k < keys.size(); ++k) by_shift[std::get<1>(keys[k])].push_back(k);
        for (const auto& [sh, ks] : by_shift) {
            std::vector<ModeQuery> qs;
            for (const std::size_t k : ks) qs.push_back({std::get<0>(keys[k]), std::get<2>(keys[k])});
            const auto got = predict_queries(shifted(me, sh), measured_band, gap_s, pv ? &*pv : nullptr, gated, qs);
            for (std::size_t j = 0; j < ks.size(); ++j) memo.emplace(keys[ks[j]], got[j]);
        }
    }

    // my reply: the cheapest in expectation, a lost one costing a timeout
    const Mode* reply = nullptr;
    double reply_c = std::numeric_limits<double>::infinity(), reply_p = 0.0;
    for (const Mode* s : cands) {
        const double t = burst_seconds(*s, 1);
        const double ok = q_burst(*s, 1);
        const double c = t + (1 - ok) * (TIMEOUT_S + t) / std::max(ok, 1e-3);
        if (c < reply_c) reply = s, reply_c = c, reply_p = ok;
    }

    const Mode* cur = log.empty() ? nullptr : &mode_at(log.back().data);
    const std::int64_t held = cur ? st.held * payload_bytes(*cur) : 0;
    const bool chat = st.chat;
    const std::int64_t queued = std::max<std::int64_t>(CHAT_LINE_BYTES, st.peer_queued);
    const Mode* best = nullptr;
    int best_hint = 0;
    double best_v = chat ? -std::numeric_limits<double>::infinity() : -1.0;
    for (const Mode* s : data_cands) {
        const int pb = payload_bytes(*s), c = ctl_slots(*s);
        for (int hint = 0; hint < static_cast<int>(SIZE_S.size()); ++hint) {
            int n = slots_for(*s, SIZE_S[hint]);
            int k = 0;
            if (chat) {
                k = cdiv(queued, pb);
                if (n < k + c && hint < static_cast<int>(SIZE_S.size()) - 1) continue;
                n = std::min(n, k + c);
            }
            const double pn = q_cw(*s, n);
            const double ok_ctl = q_burst(*s, n);
            if (ok_ctl * pn < min_success) continue;
            const bool dup = s->is_cpm() && ok_ctl < DUP_BELOW;
            const double tb = burst_seconds(*s, n + dup, dup);
            double t = tb + 2 * TURN_S + reply_c + (1 - ok_ctl) * TIMEOUT_S;
            // lost, the burst leaves (LINK_LOST_S - tb) to recover in: each
            // try a poll and its reply, both at about my reply's P
            const double tries = std::max(0.0, std::floor((LINK_LOST_S - tb - TIMEOUT_S) / T_RECOVER_S));
            t += (1 - ok_ctl) * std::pow(1 - reply_p * reply_p, tries) * LOST_LINK_COST_S;
            double v;
            if (chat) {
                // one burst: retried until all of it arrives; past one, each
                // codeword is resent until it does (policy.py)
                const int bursts = cdiv(k, std::max(n - c, 1));
                const double ok = std::max(ok_ctl * (bursts == 1 ? std::pow(pn, static_cast<double>(n - c)) : pn), 1e-3);
                v = -t * bursts / ok + 1e-6 * (n - c) * pb;
            } else {
                v = (ok_ctl * pn * (n - c) * pb - static_cast<double>(s != cur ? held : 0)) / t;
            }
            if (v > best_v) best = s, best_hint = hint, best_v = v;
            if (chat) break;
        }
    }
    std::string_view best_name;
    if (!best) {
        best_name = fallback(st.cap), best_hint = 1;
    } else {
        best_name = best->name;
        const int nb = slots_for(*best, SIZE_S[best_hint]);
        predicted = {{std::string(best->name), {q_burst(*best, nb), q_cw(*best, nb)}}};
        want_dup = predicted.begin()->second.first < DUP_BELOW;
        if (reply) predicted[std::string(reply->name)] = {q_burst(*reply, 1), q_cw(*reply, 1)};
    }
    const std::string_view reply_name = reply ? reply->name : fallback(st.cap);
    log.push_back({std::string(best_name), best_hint, std::string(reply_name)});
    peer_had_data = st.last_rx_data;
    return {encode(best_name), best_hint, encode(reply_name)};
}

StationView view(const Station& st) {
    StationView v;
    v.cap = st.cap;
    v.pending = st.tx.pending();
    v.peer_recommend = st.peer_recommend;
    v.peer_reply_recommend = st.peer_reply_recommend;
    v.peer_size_hint = st.peer_size_hint;
    v.peer_wants_dup = st.peer_wants_dup;
    v.chat = st.chat || st.peer_chat;
    v.peer_queued = st.peer_queued;
    v.held = static_cast<std::int64_t>(st.rx.buf.size());
    v.misses = st.misses;
    v.esc_floor = st.esc_floor;
    v.last_rx_data = st.last_rx_data;
    return v;
}

std::pair<std::string, int> GearPolicy::choose(Station& st, int escalation) {
    const auto [m, n] = shifter.choose(view(st), escalation);
    return {std::string(m), n};
}
int GearPolicy::payload_bytes(const std::string& m) { return arq::payload_bytes(mode_at(m)); }
int GearPolicy::ctl_payload_bytes(const std::string& m) { return arq::ctl_payload_bytes(mode_at(m)); }
int GearPolicy::max_ctl(const std::string& m) { return arq::max_ctl(mode_at(m)); }
int GearPolicy::rv_cycle(const std::string& m) { return arq::rv_cycle(mode_at(m)); }
std::optional<Recommendation> GearPolicy::recommend(Station& st) {
    const auto r = shifter.recommend(view(st));
    return Recommendation{r.data, r.hint, r.reply};
}
std::string GearPolicy::mode_name(int rec) {
    const Mode* m = decode(rec);
    return m ? std::string(m->name) : "?" + std::to_string(rec);
}
std::optional<double> GearPolicy::snr_est() {
    return shifter.measured ? std::optional(shifter.measured->snr_est) : std::nullopt;
}
std::string GearPolicy::measured_extra() {
    if (!shifter.measured) return {};
    const Measured& m = *shifter.measured;
    char buf[128];
    std::snprintf(buf, sizeof buf, ", spread %.2f Hz, delay %.1f ms, mi %.2f/%.2f/%.2f/%.2f", m.spread_est,
                  m.delay_est_ms, m.mi[0], m.mi[1], m.mi[2], m.mi[3]);
    std::string out = buf;
    if (!shifter.energies.empty()) {
        std::snprintf(buf, sizeof buf, ", energy %.1f dB", shifter.energies.back());
        out += buf;
    }
    return out;
}
double GearPolicy::airtime(const std::string& m, int n_cw, bool dup) { return burst_seconds(mode_at(m), n_cw, dup); }
std::string GearPolicy::connect_mode(int cap, int tries) { return std::string(arq::connect_mode(cap, tries)); }

}  // namespace data2g::arq
