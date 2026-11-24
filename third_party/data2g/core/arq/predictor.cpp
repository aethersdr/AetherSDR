#include "arq/predictor.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <utility>

#include "cpm/cpm.hpp"
#include "dsp/dsp.hpp"
#include "waveform/ofdm.hpp"

namespace data2g::arq {

namespace {

// predictor.LOGIT_OFFSETS for the installed model.
constexpr std::pair<std::string_view, double> LOGIT_OFFSETS[] = {{"w48-16qam-r1/2", -1.0}, {"n10-256l-r3/4", -0.5}};

const tables::CapacityTable& table(std::string_view constellation) {
    const auto family = const_family(constellation);
    for (const auto& t : tables::CAPACITY)
        if (t.name == family) return t;
    throw std::out_of_range("no capacity table for " + std::string(constellation));
}

// np.interp, one point.
double interp(double x, std::span<const double> xp, std::span<const double> fp) {
    const std::size_t n = xp.size();
    if (x > xp[n - 1]) return fp[n - 1];
    if (x < xp[0]) return fp[0];
    const std::size_t j = static_cast<std::size_t>(std::upper_bound(xp.begin(), xp.end(), x) - xp.begin()) - 1;
    if (j == n - 1 || xp[j] == x) return fp[j];
    const double slope = (fp[j + 1] - fp[j]) / (xp[j + 1] - xp[j]);
    return slope * (x - xp[j]) + fp[j];
}

double sigmoid(double z) { return 1 / (1 + std::exp(-z)); }

}  // namespace

std::string_view const_family(std::string_view name) {
    if (name.starts_with("c64")) return "c64-snr18";
    if (name.starts_with("c256")) return "c256-snr26";
    return name;
}

double capacity(double snr_db, std::string_view constellation) {
    return interp(snr_db, tables::CAPACITY_GRID, table(constellation).mi);
}

double peak_db(std::string_view mode, int cap) {
    if (cap < 0 || cap > 2) throw std::out_of_range("no bandwidth cap " + std::to_string(cap));
    for (const auto& p : tables::MODE_PEAK_DB)
        if (p.mode == mode) return p.db[cap];
    throw std::out_of_range("no peak for mode " + std::string(mode));
}

std::array<double, 2> band_span_hz(std::string_view band) {
    if (const auto* g = cpm::grid(band)) return {g->f0 - g->bp, g->f0 + (g->m - 1) * g->rate + g->bp};
    auto f = waveform::band(band).freqs;
    std::sort(f.begin(), f.end());
    const double half = f.size() > 1 ? (f[1] - f[0]) / 2.0 : 25.0;
    return {static_cast<double>(f.front()) - half, static_cast<double>(f.back()) + half};
}

namespace {
double band_level(const std::array<double, 5>& db, std::string_view band) {
    const auto [lo, hi] = band_span_hz(band);
    double num = 0.0, den = 0.0;
    for (std::size_t i = 0; i < NOISE_BANDS_HZ.size(); ++i) {
        const double w = std::max(0.0, std::min(hi, NOISE_BANDS_HZ[i][1]) - std::max(lo, NOISE_BANDS_HZ[i][0]));
        num += w * std::pow(10.0, db[i] / 10);
        den += w;
    }
    if (den <= 0) return dsp::quantile(std::vector<double>(db.begin(), db.end()), 0.5);
    return 10 * std::log10(num / den);
}
}  // namespace

double noise_shift_db(const std::optional<NoiseLevels>& noise, std::string_view measured_band, std::string_view band,
                      double tail_weight, double deadband_db) {
    if (!noise) return 0.0;
    std::array<double, 5> loud{};
    for (std::size_t i = 0; i < loud.size(); ++i) loud[i] = noise->db[i] + noise->tail_db[i];
    const double med = band_level(noise->db, band) - band_level(noise->db, measured_band);
    const double l = band_level(loud, band) - band_level(loud, measured_band);
    const double shift = std::max(0.0, med) + tail_weight * std::max(0.0, l - std::max(0.0, med));
    return shift >= deadband_db ? shift : 0.0;
}

Measured shifted(const Measured& m, double shift_db) {
    if (shift_db == 0.0) return m;
    Measured out = m;
    out.snr_est -= shift_db;
    for (std::size_t i = 0; i < CONSTS.size(); ++i) {
        const auto& t = table(CONSTS[i]);
        const double snr = interp(m.mi[i], t.mi, tables::CAPACITY_GRID);  // the curve is increasing
        out.mi[i] = interp(snr - shift_db, tables::CAPACITY_GRID, t.mi);
    }
    return out;
}

double effective_mi(std::span<const std::complex<double>> h, std::span<const double> var,
                    std::string_view constellation) {
    if (h.size() != var.size() || h.empty()) throw std::invalid_argument("effective_mi: h and var must match");
    const auto& t = table(constellation);
    std::vector<double> mi(h.size());
    for (std::size_t i = 0; i < h.size(); ++i) {
        const double a = std::abs(h[i]);
        const double snr = 10 * std::log10(std::max(a * a / var[i], 1e-6));
        mi[i] = interp(snr, tables::CAPACITY_GRID, t.mi);
    }
    return dsp::pairwise_sum(mi) / static_cast<double>(mi.size());
}

std::vector<double> outcome_inputs(const Measured& m, std::string_view band, double gap, double seconds,
                                   const Prev* prev, std::span<const std::string_view> bands, bool energy) {
    std::vector<double> x = {m.snr_est, std::log(0.05 + m.spread_est), m.delay_est_ms};
    x.insert(x.end(), m.mi.begin(), m.mi.end());
    x.push_back(m.headroom);
    x.push_back(std::log2(m.frames));
    for (const auto b : bands) x.push_back(b == band ? 1.0 : 0.0);
    const Measured& pm = prev ? prev->m : m;
    const std::string_view pb = prev ? std::string_view(prev->band) : band;
    const double age = prev ? prev->age : 0.0;
    x.push_back(prev ? 1.0 : 0.0);
    x.insert(x.end(), pm.mi.begin(), pm.mi.end());
    x.push_back(pm.snr_est);
    x.push_back(std::log(0.05 + pm.spread_est));
    x.push_back(std::log2(1 + age));
    x.push_back(pb == band ? 1.0 : 0.0);
    x.push_back(std::log2(pm.frames));
    x.push_back(gap);
    x.push_back(std::log2(seconds));
    if (energy) {
        const std::array<double, 3> e = m.energy.value_or(std::array<double, 3>{});
        x.insert(x.end(), e.begin(), e.end());
    }
    return x;
}

namespace {

// One dense layer over a batch: h (B, L.in) row-major -> (B, L.out), tanh if `act`.
// W row by row, each row used for every input while it is in cache. numpy's
// float32 matmul sums in another order: the parity tests allow float32
// rounding (test_native_arq.LOGIT_TOL)
std::vector<float> dense(const tables::MlpLayer& L, const std::vector<float>& h, std::size_t B, bool act) {
    const std::size_t in = static_cast<std::size_t>(L.in), out = static_cast<std::size_t>(L.out);
    std::vector<float> o(B * out, 0.0f);
    for (std::size_t i = 0; i < in; ++i) {
        const float* w = L.W.data() + i * out;
        for (std::size_t b = 0; b < B; ++b) {
            const float hi = h[b * in + i];
            float* ob = o.data() + b * out;
            for (std::size_t j = 0; j < out; ++j) ob[j] += hi * w[j];
        }
    }
    for (std::size_t b = 0; b < B; ++b)
        for (std::size_t j = 0; j < out; ++j) {
            float& v = o[b * out + j];
            v += L.b[j];
            if (act) v = std::tanh(v);
        }
    return o;
}

// The CNP's encoding of one input (with its log2(seconds) column, dropped here).
std::vector<float> cnp_encode(std::span<const double> x) {
    const auto& C = tables::OUTCOME_CNP;
    if (x.size() != C.mean.size() + 1) throw std::invalid_argument("outcome_logits: wrong input size");
    std::vector<float> h;
    h.reserve(C.mean.size());
    for (std::size_t i = 0, k = 0; i < x.size(); ++i)
        if (static_cast<int>(i) != C.secs_col) h.push_back(static_cast<float>((x[i] - C.mean[k]) / C.std[k])), ++k;
    for (const auto& L : C.enc) h = dense(L, h, 1, true);
    return h;
}

// Head logits (P(usable), P(codeword)) per (mode, log2 seconds) query from one encoding.
std::vector<std::array<double, 2>> cnp_heads(const std::vector<float>& z, const std::vector<std::pair<int, double>>& q) {
    const auto& C = tables::OUTCOME_CNP;
    const std::size_t nz = z.size(), nd = static_cast<std::size_t>(C.n_desc), w = nz + nd + 1, B = q.size();
    std::vector<float> h(B * w);
    for (std::size_t b = 0; b < B; ++b) {
        float* r = h.data() + b * w;
        std::copy(z.begin(), z.end(), r);
        const float* d = C.desc.data() + static_cast<std::size_t>(q[b].first) * nd;
        std::copy(d, d + nd, r + nz);
        r[nz + nd] = static_cast<float>(q[b].second);
    }
    for (std::size_t l = 0; l < C.head.size(); ++l) h = dense(C.head[l], h, B, l + 1 < C.head.size());
    std::vector<std::array<double, 2>> out(B);
    for (std::size_t b = 0; b < B; ++b) out[b] = {h[2 * b], h[2 * b + 1]};
    return out;
}

bool is_cnp(int model) { return model == 0 && tables::OUTCOME_IS_CNP; }

}  // namespace

std::vector<std::vector<double>> outcome_logits_batch(const std::vector<std::vector<double>>& xs, int model) {
    const auto& members = model == 1   ? tables::OUTCOME_GATE_MEMBERS
                          : model == 2 ? tables::OUTCOME_FAST_MEMBERS
                                       : tables::OUTCOME_MEMBERS;
    if (is_cnp(model)) {  // every mode at each input's own burst length
        const std::size_t n = tables::OUTCOME_MODES.size();
        std::vector<std::vector<double>> out;
        for (const auto& x : xs) {
            std::vector<std::pair<int, double>> q(n);
            for (std::size_t k = 0; k < n; ++k) q[k] = {static_cast<int>(k), x[static_cast<std::size_t>(tables::OUTCOME_CNP.secs_col)]};
            const auto z = cnp_heads(cnp_encode(x), q);
            std::vector<double> row(2 * n);
            for (std::size_t k = 0; k < n; ++k) row[k] = z[k][0], row[n + k] = z[k][1];
            out.push_back(std::move(row));
        }
        return out;
    }
    if (members.empty()) throw std::invalid_argument("outcome_logits: no such model shipped");
    const std::size_t B = xs.size();
    std::vector<std::vector<double>> psum(B);
    for (const auto& mem : members) {
        const std::size_t n_in = mem.mean.size();
        std::vector<float> h(B * n_in);  // (B, width) row-major
        for (std::size_t b = 0; b < B; ++b) {
            if (xs[b].size() != n_in) throw std::invalid_argument("outcome_logits: wrong input size");
            for (std::size_t i = 0; i < n_in; ++i) h[b * n_in + i] = static_cast<float>((xs[b][i] - mem.mean[i]) / mem.std[i]);
        }
        for (std::size_t l = 0; l < mem.layers.size(); ++l) h = dense(mem.layers[l], h, B, l + 1 < mem.layers.size());
        const std::size_t n_out = h.size() / std::max<std::size_t>(B, 1);
        for (std::size_t b = 0; b < B; ++b) {
            if (psum[b].empty()) psum[b].assign(n_out, 0.0);
            for (std::size_t j = 0; j < n_out; ++j)
                psum[b][j] += sigmoid(std::clamp(static_cast<double>(h[b * n_out + j]), -40.0, 40.0));
        }
    }
    for (auto& row : psum)
        for (auto& v : row) {
            const double p = std::clamp(v / static_cast<double>(members.size()), 1e-9, 1 - 1e-9);
            v = std::log(p / (1 - p));
        }
    return psum;
}

std::vector<double> outcome_logits(std::span<const double> x, int model) {
    return outcome_logits_batch({std::vector<double>(x.begin(), x.end())}, model)[0];
}

int outcome_index(std::string_view submode) {
    const auto& names = tables::OUTCOME_MODES;
    const auto it = std::find(names.begin(), names.end(), submode);
    return it == names.end() ? -1 : static_cast<int>(it - names.begin());
}

bool outcome_knows(std::string_view submode) { return outcome_index(submode) >= 0; }

std::vector<std::vector<Outcome>> predict_outcomes(const std::vector<OutcomeQuery>& qs, std::string_view band,
                                                   double gap, const Prev* prev, int model) {
    const bool energy = model == 1   ? tables::OUTCOME_GATE_ENERGY
                        : model == 2 ? tables::OUTCOME_FAST_ENERGY
                                     : tables::OUTCOME_ENERGY;
    std::vector<std::vector<double>> xs;
    xs.reserve(qs.size());
    for (const auto& q : qs) xs.push_back(outcome_inputs(q.m, band, gap, q.seconds, prev, tables::OUTCOME_BANDS, energy));
    auto zs = outcome_logits_batch(xs, model);
    const std::size_t n = tables::OUTCOME_MODES.size();
    std::vector<std::vector<Outcome>> out(qs.size(), std::vector<Outcome>(n));
    for (std::size_t q = 0; q < qs.size(); ++q) {
        auto& z = zs[q];
        for (const auto& [name, off] : LOGIT_OFFSETS)
            if (const int i = outcome_index(name); i >= 0) z[i] += off;
        for (std::size_t i = 0; i < n; ++i)
            out[q][i] = {sigmoid(std::clamp(z[i], -40.0, 40.0)), sigmoid(std::clamp(z[n + i], -40.0, 40.0))};
    }
    return out;
}

std::vector<Outcome> predict_queries(const Measured& m, std::string_view band, double gap, const Prev* prev, int model,
                                     const std::vector<ModeQuery>& qs) {
    std::vector<Outcome> out(qs.size());
    auto offset = [](int mode) {
        double off = 0.0;
        for (const auto& [name, o] : LOGIT_OFFSETS)
            if (outcome_index(name) == mode) off += o;
        return off;
    };
    if (is_cnp(model)) {
        const auto x = outcome_inputs(m, band, gap, 1.0, prev, tables::OUTCOME_BANDS, tables::OUTCOME_ENERGY);
        std::vector<std::pair<int, double>> q;
        for (const auto& mq : qs) q.push_back({mq.mode, std::log2(mq.seconds)});
        const auto z = cnp_heads(cnp_encode(x), q);
        for (std::size_t i = 0; i < qs.size(); ++i)
            out[i] = {sigmoid(std::clamp(z[i][0] + offset(qs[i].mode), -40.0, 40.0)), sigmoid(std::clamp(z[i][1], -40.0, 40.0))};
        return out;
    }
    std::vector<double> secs;
    for (const auto& mq : qs)
        if (std::find(secs.begin(), secs.end(), mq.seconds) == secs.end()) secs.push_back(mq.seconds);
    std::vector<OutcomeQuery> oq;
    for (const double sec : secs) oq.push_back({m, sec});
    const auto all = predict_outcomes(oq, band, gap, prev, model);
    for (std::size_t i = 0; i < qs.size(); ++i) {
        const auto k = static_cast<std::size_t>(std::find(secs.begin(), secs.end(), qs[i].seconds) - secs.begin());
        out[i] = all[k][static_cast<std::size_t>(qs[i].mode)];
    }
    return out;
}

std::vector<Outcome> predict_outcome(const Measured& m, std::string_view band, double gap, double seconds,
                                     const Prev* prev, int model) {
    return predict_outcomes({{m, seconds}}, band, gap, prev, model)[0];
}

}  // namespace data2g::arq
