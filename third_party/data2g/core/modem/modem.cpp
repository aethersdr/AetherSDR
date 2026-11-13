#include "modem/modem.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>

#include <memory>
#include <numbers>
#include <numeric>
#include <stdexcept>
#include <string>

#include "codes/codes.hpp"
#include "constellation/constellation.hpp"
#include "dsp/dsp.hpp"
#include "waveform/dsp.hpp"
#include "waveform/ofdm.hpp"

namespace data2g::modem {

using namespace config;
using std::int64_t;
using std::size_t;

namespace {

constexpr double PI = std::numbers::pi;
constexpr int HB = HEADER_BACKOFF;

const constellation::Constellation& qpsk() {
    static const auto* c = constellation::find("gray-qam4");
    return *c;
}

const constellation::Constellation& points_of(const Spec& spec) {
    const auto* c = constellation::find(spec.constellation);
    if (!c) throw std::out_of_range("no constellation " + std::string(spec.constellation));
    return *c;
}

std::vector<double> bb_of(std::string_view name) { return equalizer::bb(band(name)); }

double sq_abs(cd x) {
    const double a = std::abs(x);  // np.abs(x) ** 2
    return a * a;
}

// x / pilot, carrier by carrier
std::vector<cd> over_pilot(std::vector<cd> x, const waveform::Band& b) {
    for (size_t k = 0; k < x.size(); ++k) x[k] /= b.pilot[k];
    return x;
}

// np.sum(a * conj(b)) over carriers
cd dot_conj(std::span<const cd> a, std::span<const cd> b) {
    std::vector<cd> p(a.size());
    for (size_t k = 0; k < a.size(); ++k) p[k] = a[k] * std::conj(b[k]);
    return dsp::pairwise_sum(std::span<const cd>(p));
}

double energy(std::span<const cd> a) {
    std::vector<double> p(a.size());
    for (size_t k = 0; k < a.size(); ++k) p[k] = sq_abs(a[k]);
    return dsp::pairwise_sum(std::span<const double>(p));
}

// np.median of a list
double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
}

// Python's floor modulo
int64_t pymod(int64_t a, int64_t b) { return ((a % b) + b) % b; }

int64_t clamp_slice(int64_t i, int64_t n) {
    if (i < 0) i += n;
    return std::clamp<int64_t>(i, 0, n);
}

// The header correlation as a Walsh-Hadamard transform. A valid word is
// v << 6 | crc6(v) for a 10-bit v, and the CRC is affine in v, so coded bit
// j of word(v) is parity(v & mask[j]) ^ flip[j]: the correlation of soft
// bits x with every word's signs is the 10-bit WHT of
// X[u] = sum over j with mask[j] = u of +-x[j]. ~10k operations for all
// 1024 values instead of (valid words) x N multiply-adds.
struct Wht {
    std::vector<std::uint16_t> mask;
    std::vector<double> sign;  // (-1)^flip[j]
};

const Wht& wht(std::string_view band) {
    static const auto tables = [] {
        std::array<Wht, HEADER_CODES.size()> out;
        const auto word = [](int v) { return v << 6 | crc6(v); };
        for (size_t b = 0; b < out.size(); ++b)
            for (const auto col : HEADER_CODES[b].second) {
                const auto parity = [col](int w) { return std::popcount(static_cast<unsigned>(w & col)) & 1; };
                std::uint16_t m = 0;
                for (int i = 0; i < 10; ++i) m |= static_cast<std::uint16_t>(parity(word(1 << i) ^ word(0)) << i);
                out[b].mask.push_back(m);
                out[b].sign.push_back(parity(word(0)) ? -1.0 : 1.0);
            }
        return out;
    }();
    for (size_t b = 0; b < HEADER_CODES.size(); ++b)
        if (HEADER_CODES[b].first == band) return tables[b];
    throw std::out_of_range("no header code for band " + std::string(band));
}

// valid_words' 10-bit values (word >> 6), in order
std::vector<int> valid_values(std::string_view band, const Accept* accept) {
    const int b = cw_bits(band);
    if (b < 5) throw std::logic_error("valid_values: lim covers 1024 >> 5 submode indices");
    std::array<int, 32> lim{};  // submode index -> codewords taken (0: none)
    if (accept) {
        for (const auto& [s, cw] : accept->max_cw)
            if (s->sync_band == band) lim[static_cast<size_t>(s->index)] = cw;
    } else {
        for (const auto& s : SUBMODES)
            if (s.sync_band == band) lim[static_cast<size_t>(s.index)] = max_codewords(band);
    }
    std::vector<int> out;
    for (int v = 0; v < 1024; ++v)
        if ((v & ((1 << b) - 1)) < lim[static_cast<size_t>(v >> b)]) out.push_back(v);
    return out;
}

// header_corr over the valid values `vs`
std::vector<float> corr_of(std::span<const double> soft, std::string_view band, std::span<const int> vs) {
    const auto& t = wht(band);
    if (soft.size() != t.mask.size()) throw std::invalid_argument("header: soft bits and the band's code differ in length");
    // soft.astype(np.float32), summed in double: each sum is the float32
    // correlation's exact value to far under a float32 ulp, in any order
    std::array<double, 1024> X{};
    for (size_t j = 0; j < soft.size(); ++j) X[t.mask[j]] += t.sign[j] * static_cast<float>(soft[j]);
    for (size_t h = 1; h < X.size(); h *= 2)
        for (size_t i = 0; i < X.size(); i += 2 * h)
            for (size_t k = i; k < i + h; ++k) {
                const double a = X[k], b = X[k + h];
                X[k] = a + b;
                X[k + h] = a - b;
            }
    std::vector<float> out(vs.size());
    for (size_t i = 0; i < vs.size(); ++i) out[i] = static_cast<float>(X[static_cast<size_t>(vs[i])]);
    return out;
}

double floor_of(std::string_view band, const Accept* accept) {
    return std::max(header_min_score(band), accept ? accept->min_score : 0.0);
}

std::vector<std::string_view> search_bands(std::span<const std::string_view> bands, const Accept* accept) {
    if (!bands.empty()) return {bands.begin(), bands.end()};
    if (accept) return accept->bands();
    return {SYNC_BANDS.begin(), SYNC_BANDS.end()};
}

// (1 - a) h0 + a h1 per carrier
std::vector<cd> lerp(std::span<const cd> h0, std::span<const cd> h1, double a) {
    std::vector<cd> out(h0.size());
    for (size_t k = 0; k < out.size(); ++k) out[k] = (1 - a) * h0[k] + a * h1[k];
    return out;
}

// LLRs of rows ys against channel rows hs (unit variance), QPSK
std::vector<double> qpsk_llr(const Mat<cd>& ys, const Mat<cd>& hs) {
    const std::vector<double> ones(ys.data.size(), 1.0);
    return constellation::llr(ys.data, hs.data, ones, qpsk());
}

Mat<cd> rows_of(const std::vector<std::vector<cd>>& v) {
    Mat<cd> m(v.size(), v.empty() ? 0 : v[0].size());
    for (size_t i = 0; i < v.size(); ++i) std::copy(v[i].begin(), v[i].end(), m[i]);
    return m;
}

// Python slice x[lo:hi] as a span
template <typename T>
std::span<const T> pyslice(std::span<const T> x, int64_t lo, int64_t hi) {
    const auto n = static_cast<int64_t>(x.size());
    const int64_t a = clamp_slice(lo, n), b = clamp_slice(hi, n);
    return b > a ? x.subspan(static_cast<size_t>(a), static_cast<size_t>(b - a)) : std::span<const T>();
}

}  // namespace

double header_min_score(std::string_view band) {
    for (const auto& [b, v] : HEADER_MIN_SCORE)
        if (b == band) return v;
    throw std::out_of_range("no HEADER_MIN_SCORE for band " + std::string(band));
}

double pilot_noise(std::string_view band) {
    for (const auto& [b, v] : PILOT_NOISE)
        if (b == band) return v;
    throw std::out_of_range("no PILOT_NOISE for band " + std::string(band));
}

double pilot_pattern(std::string_view band) {
    for (const auto& [b, v] : PILOT_PATTERN)
        if (b == band) return v;
    throw std::out_of_range("no PILOT_PATTERN for band " + std::string(band));
}

Accept Accept::of(std::span<const std::string_view> names, std::optional<double> max_secs, double min_score) {
    Accept a;
    a.min_score = min_score;
    std::vector<std::string_view> all;
    if (names.empty())
        for (const auto& s : SUBMODES) all.push_back(s.name);
    else
        all.assign(names.begin(), names.end());
    for (auto n : all) {
        const Spec* s = config::submode(n);
        if (!s) throw std::out_of_range("no submode " + std::string(n));
        int cw = max_codewords(s->sync_band);
        if (max_secs) {
            int fixed = preamble_samples(band(s->sync_band)) + header_samples(s->sync_band) + NSYM;
            fixed += FRAME_SAMPLES * copies(s->sync_band);
            cw = std::min<int>(cw, static_cast<int>(std::floor((*max_secs * FS - fixed) / (s->frames_per_cw * FRAME_SAMPLES))));
        }
        if (cw >= 1) a.max_cw.emplace_back(s, cw);
    }
    return a;
}

std::vector<std::string_view> Accept::bands() const {
    std::vector<std::string_view> out;
    for (const auto& [s, cw] : max_cw)
        if (std::find(out.begin(), out.end(), s->sync_band) == out.end()) out.push_back(s->sync_band);
    std::sort(out.begin(), out.end());
    return out;
}

// --- header -----------------------------------------------------------------

int crc6(int v) {
    int reg = PROTOCOL_VERSION & 0x3F;
    for (int i = 9; i >= 0; --i) {
        const int fb = ((reg >> 5) & 1) ^ ((v >> i) & 1);
        reg = ((reg << 1) & 0x3F) ^ (fb ? 0x3 : 0);
    }
    return reg;
}

std::span<const std::uint16_t> header_cols(std::string_view band) {
    for (const auto& [b, cols] : HEADER_CODES)
        if (b == band) return cols;
    throw std::out_of_range("no header code for band " + std::string(band));
}

std::vector<std::uint8_t> codeword(int word, std::string_view band) {
    const auto cols = header_cols(band);
    std::vector<std::uint8_t> out(cols.size());
    for (size_t j = 0; j < cols.size(); ++j)
        out[j] = static_cast<std::uint8_t>(std::popcount(static_cast<unsigned>(word & cols[j])) & 1);
    return out;
}

std::vector<std::uint8_t> header_bits(int submode, int n_cw, std::string_view band) {
    const int b = cw_bits(band);
    if (n_cw < 1 || n_cw > 1 << b)
        throw std::invalid_argument("1.." + std::to_string(1 << b) + " codewords per burst on " + std::string(band));
    if (submode < 0 || submode >= 1 << (10 - b))
        throw std::invalid_argument("submode index " + std::to_string(submode) + " does not fit " + std::string(band) +
                                    "'s header");
    const int v = submode << b | (n_cw - 1);
    return codeword(v << 6 | crc6(v), band);
}

std::vector<int> valid_words(std::string_view band, const Accept* accept) {
    auto out = valid_values(band, accept);
    for (int& v : out) v = v << 6 | crc6(v);
    return out;
}

std::vector<float> header_corr(std::span<const double> soft, std::string_view band, const Accept* accept) {
    return corr_of(soft, band, valid_values(band, accept));
}

Header decode_header(std::span<const double> soft, std::string_view band, const Accept* accept) {
    const auto vs = valid_values(band, accept);
    if (vs.empty()) throw std::invalid_argument("decode_header: no valid words");
    const auto corr = corr_of(soft, band, vs);
    size_t i = 0;  // np.argmax: the first maximum (a NaN, if any, wins)
    for (size_t k = 1; k < corr.size() && !std::isnan(corr[i]); ++k)
        if (corr[k] > corr[i] || std::isnan(corr[k])) i = k;
    std::vector<double> sq(soft.size());
    for (size_t k = 0; k < soft.size(); ++k) sq[k] = soft[k] * soft[k];
    const double norm = std::sqrt(dsp::pairwise_sum(std::span<const double>(sq)) * static_cast<double>(soft.size()));
    Header h;
    h.word = vs[i] << 6 | crc6(vs[i]);
    h.score = static_cast<double>(corr[i]) / (norm + 1e-12);
    const int v = h.word >> 6, b = cw_bits(band);
    h.spec = by_index(band, v >> b);
    if (!h.spec) throw std::logic_error("valid word without a submode");
    h.n_cw = (v & ((1 << b) - 1)) + 1;
    return h;
}

// --- transmit -----------------------------------------------------------------

std::vector<int64_t> ace_cells(const Spec& spec, int n_f) {
    const auto kc = copy_frame(spec.sync_band, n_f);
    const int64_t f0 = LEADIN_SAMPLES + preamble_samples(band(spec.sync_band)) + header_samples(spec.sync_band);
    std::vector<int64_t> out;
    for (int f = 0; f < n_f + kc.has_value(); ++f) {
        if (kc && f == *kc) continue;
        for (int s = 1; s < SYMS_PER_FRAME; ++s) out.push_back(f0 + static_cast<int64_t>(f * SYMS_PER_FRAME + s) * NSYM);
    }
    return out;
}

std::vector<double> burst_waveform(const Mat<cd>& data_in, const Spec& spec) {
    const auto& b = waveform::band(spec.band);
    const auto& sb = waveform::band(spec.sync_band);
    const size_t nc = static_cast<size_t>(b.nc()), snc = static_cast<size_t>(sb.nc());
    if (data_in.cols != nc || data_in.rows % DATA_SYMS_PER_FRAME)
        throw std::invalid_argument("burst_waveform: need (n_f * 5, nc) data symbols");
    int n_f = static_cast<int>(data_in.rows / DATA_SYMS_PER_FRAME);
    const int n_cw = n_f / spec.frames_per_cw;
    const auto hbits = header_bits(spec.index, n_cw, spec.sync_band);
    const auto hdr = constellation::modulate(hbits, qpsk());  // (header_syms * snc)
    const int hs = sb.spec->header_syms;
    const auto kc = copy_frame(spec.sync_band, n_f);
    Mat<cd> data = data_in;
    if (kc) {  // the header copy: a frame of its own (data band = sync band here)
        if (snc != nc) throw std::logic_error("header copy on a band other than its sync band");
        Mat<cd> d2(data.rows + DATA_SYMS_PER_FRAME, nc);
        const size_t at = static_cast<size_t>(*kc) * DATA_SYMS_PER_FRAME;
        std::copy(data.data.begin(), data.data.begin() + static_cast<std::ptrdiff_t>(at * nc), d2.data.begin());
        for (int s = 0; s < DATA_SYMS_PER_FRAME; ++s) {
            const size_t src = static_cast<size_t>(s < hs ? s : s - hs);
            std::copy(hdr.begin() + static_cast<std::ptrdiff_t>(src * snc),
                      hdr.begin() + static_cast<std::ptrdiff_t>((src + 1) * snc), d2[at + static_cast<size_t>(s)]);
        }
        std::copy(data.data.begin() + static_cast<std::ptrdiff_t>(at * nc), data.data.end(),
                  d2.data.begin() + static_cast<std::ptrdiff_t>((at + DATA_SYMS_PER_FRAME) * nc));
        data = std::move(d2);
        ++n_f;
    }
    Mat<cd> syms(static_cast<size_t>(n_f) * SYMS_PER_FRAME + 1, nc);
    for (size_t r = 0; r < syms.rows; ++r) {
        if (r % SYMS_PER_FRAME == 0) {
            std::copy(b.pilot.begin(), b.pilot.end(), syms[r]);
        } else {
            const size_t f = r / SYMS_PER_FRAME, s = r % SYMS_PER_FRAME - 1;
            std::copy(data[f * DATA_SYMS_PER_FRAME + s], data[f * DATA_SYMS_PER_FRAME + s] + nc, syms[r]);
        }
    }
    const auto layout = header_layout(spec.sync_band);
    Mat<cd> hsyms(layout.size(), snc);
    size_t d = 0;
    for (size_t r = 0; r < layout.size(); ++r) {
        if (layout[r]) {
            std::copy(sb.pilot.begin(), sb.pilot.end(), hsyms[r]);
        } else {
            std::copy(hdr.begin() + static_cast<std::ptrdiff_t>(d * snc), hdr.begin() + static_cast<std::ptrdiff_t>((d + 1) * snc),
                      hsyms[r]);
            ++d;
        }
    }
    std::vector<double> out(LEADIN_SAMPLES, 0.0);
    const auto pre = sb.preamble_waveform();
    const auto hw = sb.modulate_symbols(hsyms);
    const auto dw = b.modulate_symbols(syms);
    out.insert(out.end(), pre.begin(), pre.end());
    out.insert(out.end(), hw.begin(), hw.end());
    out.insert(out.end(), dw.begin(), dw.end());
    out.insert(out.end(), LEADOUT_SAMPLES, 0.0);
    return out;
}

namespace {

// modem.ace_projector: each data cell of the clipped burst moved into its
// point's region (the point scaled by the carrier's clip gain).
waveform::Projector ace_projector(const Spec& spec, const Mat<cd>& data, std::vector<cd> dirs) {
    const auto& b = waveform::band(spec.band);
    const size_t nc = static_cast<size_t>(b.nc());
    auto cells = std::make_shared<std::vector<int64_t>>(ace_cells(spec, static_cast<int>(data.rows / DATA_SYMS_PER_FRAME)));
    auto X = std::make_shared<Mat<cd>>(data);
    auto D = std::make_shared<std::vector<cd>>(std::move(dirs));
    return [&b, nc, cells, X, D](std::span<const double> x) {
        const size_t n = cells->size();
        Mat<cd> got(n, nc);
        for (size_t i = 0; i < n; ++i) {
            const double* w = x.data() + (*cells)[i] + NCP;
            for (size_t k = 0; k < nc; ++k) {
                cd acc = 0.0;
                for (size_t t = 0; t < static_cast<size_t>(M); ++t) acc += w[t] * std::conj(b.mod[NCP + t][k]);
                got[i][k] = (2.0 / M) * acc;
            }
        }
        std::vector<double> den(nc, 0.0);
        std::vector<cd> numc(nc, 0.0);
        for (size_t i = 0; i < n; ++i)
            for (size_t k = 0; k < nc; ++k) {
                numc[k] += std::conj((*X)[i][k]) * got[i][k];
                den[k] += sq_abs((*X)[i][k]);
            }
        Mat<cd> want(n, nc);
        for (size_t i = 0; i < n; ++i)
            for (size_t k = 0; k < nc; ++k) want[i][k] = (numc[k].real() / den[k]) * (*X)[i][k];
        const auto proj = constellation::ace_project(got.data, want.data, *D);
        std::vector<double> out(x.begin(), x.end());
        std::vector<cd> delta(nc);
        for (size_t i = 0; i < n; ++i) {
            for (size_t k = 0; k < nc; ++k) delta[k] = proj[i * nc + k] - got[i][k];
            double* o = out.data() + (*cells)[i];
            for (size_t t = 0; t < static_cast<size_t>(NSYM); ++t) {
                cd acc = 0.0;
                for (size_t k = 0; k < nc; ++k) acc += delta[k] * b.mod[t][k];
                o[t] += acc.real();
            }
        }
        return out;
    };
}

}  // namespace

std::vector<double> modulate_bits(std::span<const std::uint8_t> bits, const Spec& spec) {
    const auto& b = waveform::band(spec.band);
    const auto& pts = points_of(spec);
    const size_t nc = static_cast<size_t>(b.nc());
    const auto sym = constellation::modulate(bits, pts);
    if (sym.size() % (DATA_SYMS_PER_FRAME * nc)) throw std::invalid_argument("modulate_bits: not whole frames");
    Mat<cd> data(sym.size() / nc, nc);
    data.data = sym;
    const auto x = burst_waveform(data, spec);
    waveform::Projector project;
    if (!spec.ace.empty()) {
        const int m = pts.m;
        std::vector<cd> dirs(2 * sym.size());
        for (size_t s = 0; s < sym.size(); ++s) {
            size_t idx = 0;
            for (int j = 0; j < m; ++j) idx = idx << 1 | bits[s * static_cast<size_t>(m) + static_cast<size_t>(j)];
            dirs[2 * s] = pts.ace[2 * idx];
            dirs[2 * s + 1] = pts.ace[2 * idx + 1];
        }
        project = ace_projector(spec, data, std::move(dirs));
    }
    return waveform::tx_condition(x, spec.headroom, b.spec->clip_overshoot, LEADIN_SAMPLES, x.size() - LEADOUT_SAMPLES,
                                  waveform::band(spec.sync_band).tx_bandpass(), project, spec.ace);
}

std::vector<double> modulate(std::span<const std::vector<std::uint8_t>> payloads, const Spec& spec,
                             std::span<const int> rvs) {
    if (payloads.empty() || payloads.size() > static_cast<size_t>(max_codewords(spec.sync_band)))
        throw std::invalid_argument("1.." + std::to_string(max_codewords(spec.sync_band)) + " codewords per burst, got " +
                                    std::to_string(payloads.size()));
    if (!rvs.empty() && rvs.size() != payloads.size()) throw std::invalid_argument("one rv per codeword");
    const auto& cs = codes::spec(spec);
    std::vector<std::uint8_t> coded;
    for (size_t i = 0; i < payloads.size(); ++i) {
        const auto c = codes::encode(cs, payloads[i], rvs.empty() ? 0 : rvs[i], 0, static_cast<int>(i));
        coded.insert(coded.end(), c.begin(), c.end());
    }
    const auto bits = codes::spread<std::uint8_t>(coded, static_cast<int>(payloads.size()), spec.bits_per_cu);
    return modulate_bits(bits, spec);
}

// --- receive ------------------------------------------------------------------

double bin_phase_step(std::span<const cd> h) {
    if (h.size() < 2) return std::arg(cd(0.0));
    return std::arg(dot_conj(h.subspan(1), h.first(h.size() - 1)));
}

namespace {

// modem._coherence: |<ref, h>| / (|ref| |h|)
double coherence(std::span<const cd> h, std::span<const cd> ref) {
    cd acc{};
    double eh = 0.0, er = 0.0;
    for (size_t k = 0; k < h.size(); ++k) {
        acc += h[k] * std::conj(ref[k]);
        eh += std::norm(h[k]);
        er += std::norm(ref[k]);
    }
    return std::abs(acc) / (std::sqrt(eh) * std::sqrt(er) + 1e-30);
}

// modem._find_slip. Every offset's pilot by a sliding DFT (one update per
// carrier a sample, not a window's demod): cheap enough for a Pi.
int64_t find_slip(std::span<const cd> z, int64_t p, int n_left, std::span<const cd> prev, const waveform::Band& b) {
    if (coherence(over_pilot(b.demod_window(z, p + NCP, DEMOD_BACKOFF), b), prev) >= SLIP_TRIGGER) return 0;
    const auto zlen = static_cast<int64_t>(z.size());
    const auto at_z = [&](int64_t i) { return i < zlen ? z[static_cast<size_t>(i)] : cd{}; };
    const int64_t lo = std::max<int64_t>(-SLIP_MAX, -p);
    const auto n_off = static_cast<size_t>(SLIP_MAX - lo + 1);
    const size_t nc = static_cast<size_t>(b.nc());
    std::vector<cd> rot(nc), x, h(nc);
    for (size_t k = 0; k < nc; ++k) rot[k] = waveform::phasor(b.bb[k], 1);
    std::vector<double> score(n_off, 0.0);
    std::vector<std::vector<double>> at;
    for (int j = 0; j <= std::min(2, n_left); ++j) {
        const int64_t a = p + j * FRAME_SAMPLES + NCP - DEMOD_BACKOFF + lo;  // offset lo's window
        x = b.demod_window(z, a, 0);
        auto& c = at.emplace_back(n_off);
        for (size_t i = 0; i < n_off; ++i) {
            if (i) {
                const int64_t s = a + static_cast<int64_t>(i) - 1;
                const cd d = (2.0 / M) * (at_z(s + M) - at_z(s));
                for (size_t k = 0; k < nc; ++k) x[k] = (x[k] + d) * rot[k];
            }
            for (size_t k = 0; k < nc; ++k) h[k] = x[k] / b.pilot[k];
            c[i] = coherence(h, prev);
            score[i] += c[i];
        }
    }
    size_t best = n_off;
    for (size_t i = 0; i < n_off; ++i)
        if (std::abs(lo + static_cast<int64_t>(i)) >= SLIP_MIN && (best == n_off || score[i] > score[best])) best = i;
    if (best == n_off) return 0;
    for (const auto& c : at)
        if (c[best] < std::max(2 * c[static_cast<size_t>(-lo)], SLIP_ACCEPT)) return 0;
    return lo + static_cast<int64_t>(best);
}

}  // namespace

Frames demod_frames(std::span<const cd> z, int64_t p, int n_f, int shift, double phi_ref,
                    std::span<const double> steps_in, std::string_view band, std::span<const std::int64_t> slips_in) {
    const auto& b = waveform::band(band);
    const size_t nc = static_cast<size_t>(b.nc());
    phi_ref = phi_ref + 2 * PI * RS * shift / FS;  // the shift's own slope
    p += shift;
    const auto zlen = static_cast<int64_t>(z.size());
    Frames out;
    Mat<cd> raw(static_cast<size_t>(n_f + 1) * SYMS_PER_FRAME, nc);
    std::vector<double> steps(static_cast<size_t>(n_f + 1), 0.0), powers;
    std::vector<std::int64_t> slips(static_cast<size_t>(n_f + 1), 0);
    const bool search = steps_in.empty() && slips_in.empty();
    double tau_ema = 0.0;
    int total = 0;
    for (int f = 0; f <= n_f; ++f) {
        const int n_s = f < n_f ? SYMS_PER_FRAME : 1;
        auto& slip = slips[static_cast<size_t>(f)];
        if (!slips_in.empty()) slip = slips_in[static_cast<size_t>(f)];
        else if (search && f > 0 && f < n_f) {
            const auto* prev = raw[static_cast<size_t>((f - 1) * SYMS_PER_FRAME)];
            slip = find_slip(z, p, n_f - f, over_pilot({prev, prev + nc}, b), b);
        }
        p += slip;
        if (p + n_s * NSYM > zlen) throw SyncError("burst truncated");
        for (int s = 0; s < n_s; ++s) {
            const auto w = b.demod_window(z, p + s * NSYM + NCP, DEMOD_BACKOFF);
            std::copy(w.begin(), w.end(), raw[static_cast<size_t>(f * SYMS_PER_FRAME + s)]);
        }
        steps[static_cast<size_t>(f)] = total;
        if (!steps_in.empty()) {
            if (f < n_f) {
                total = static_cast<int>(steps_in[static_cast<size_t>(f + 1)]);
                p += static_cast<int64_t>(steps_in[static_cast<size_t>(f + 1)] - steps_in[static_cast<size_t>(f)]);
            }
            p += FRAME_SAMPLES;
            continue;
        }
        const auto hf = over_pilot({raw[static_cast<size_t>(f * SYMS_PER_FRAME)], raw[static_cast<size_t>(f * SYMS_PER_FRAME)] + nc}, b);
        powers.push_back(energy(hf) / static_cast<double>(nc));
        if (powers.back() > 0.1 * median(powers)) {
            const double d = std::arg(std::exp(cd(0.0, bin_phase_step(hf) - phi_ref)));
            tau_ema += 0.02 * (-d * FS / (2 * PI * RS) - tau_ema);
            if (std::abs(tau_ema) >= 2) {
                const int step = static_cast<int>(std::clamp(std::nearbyint(tau_ema), -2.0, 2.0));
                p += step;
                total += step;
                tau_ema -= step;
            }
        }
        p += FRAME_SAMPLES;
    }
    const auto ph = equalizer::time_shift_phase(steps, bb_of(band));
    for (size_t r = 0; r < raw.rows; ++r)
        for (size_t k = 0; k < nc; ++k) raw[r][k] *= ph[r / SYMS_PER_FRAME][k];
    out.hp = Mat<cd>(static_cast<size_t>(n_f + 1), nc);
    for (size_t f = 0; f <= static_cast<size_t>(n_f); ++f)
        for (size_t k = 0; k < nc; ++k) out.hp[f][k] = raw[f * SYMS_PER_FRAME][k] / b.pilot[k];
    raw.rows = static_cast<size_t>(n_f) * SYMS_PER_FRAME;
    raw.data.resize(raw.rows * nc);
    out.raw = std::move(raw);
    out.steps = std::move(steps);
    out.slips = std::move(slips);
    return out;
}

std::optional<std::vector<double>> copy_llr(std::span<const cd> z, int64_t p, std::string_view band, int n_hdr) {
    const auto& b = waveform::band(band);
    if (p + FRAME_SAMPLES + NSYM > static_cast<int64_t>(z.size())) return std::nullopt;
    std::vector<std::vector<cd>> win;
    for (int s = 0; s <= SYMS_PER_FRAME; ++s) win.push_back(b.demod_window(z, p + s * NSYM + NCP, HB));
    const auto h0 = over_pilot(win[0], b), h1 = over_pilot(win[SYMS_PER_FRAME], b);
    const size_t nc = h0.size();
    Mat<cd> ys(DATA_SYMS_PER_FRAME, nc), hs(DATA_SYMS_PER_FRAME, nc);
    for (int s = 1; s < SYMS_PER_FRAME; ++s) {
        std::copy(win[static_cast<size_t>(s)].begin(), win[static_cast<size_t>(s)].end(), ys[static_cast<size_t>(s - 1)]);
        const auto h = lerp(h0, h1, static_cast<double>(s) / SYMS_PER_FRAME);
        std::copy(h.begin(), h.end(), hs[static_cast<size_t>(s - 1)]);
    }
    const auto llr = qpsk_llr(ys, hs);
    const size_t row = 2 * nc;
    std::vector<double> out(llr.begin(), llr.begin() + static_cast<std::ptrdiff_t>(static_cast<size_t>(n_hdr) * row));
    for (int i = n_hdr; i < DATA_SYMS_PER_FRAME; ++i)
        for (size_t j = 0; j < row; ++j) out[static_cast<size_t>(i - n_hdr) * row + j] += llr[static_cast<size_t>(i) * row + j];
    return out;
}

HeaderRead read_header(std::span<const cd> z, int64_t start, std::string_view band, const Accept* accept) {
    const auto& b = waveform::band(band);
    const size_t nc = static_cast<size_t>(b.nc());
    const int R = b.spec->preamble_repeats, ref = std::min(R, REF_REPEATS);
    const int64_t u0 = start + PREAMBLE_CP + static_cast<int64_t>(R - ref) * M;
    Mat<cd> h_reps(static_cast<size_t>(R), nc);
    for (int r = 0; r < R; ++r) {
        const auto w = over_pilot(b.demod_window(z, start + PREAMBLE_CP + static_cast<int64_t>(r) * M, HB), b);
        std::copy(w.begin(), w.end(), h_reps[static_cast<size_t>(r)]);
    }
    HeaderRead out;
    out.h_pre.assign(nc, 0.0);
    for (int r = R - ref; r < R; ++r)
        for (size_t k = 0; k < nc; ++k) out.h_pre[k] += h_reps[static_cast<size_t>(r)][k];
    for (auto& v : out.h_pre) v /= static_cast<double>(ref);
    const int64_t h0 = start + b.preamble_samples();
    const int64_t p0 = h0 + header_samples(band);
    out.h_first = over_pilot(b.demod_window(z, p0 + NCP, HB), b);
    const auto layout = header_layout(band);
    const size_t L = layout.size();
    std::vector<double> t_sym(L);
    out.y_all = Mat<cd>(L, nc);
    for (size_t i = 0; i < L; ++i) {
        const int64_t s = h0 + static_cast<int64_t>(i) * NSYM + NCP;
        t_sym[i] = static_cast<double>(s) + M / 2.0;
        const auto w = b.demod_window(z, s, HB);
        std::copy(w.begin(), w.end(), out.y_all[i]);
    }
    // pilots: the preamble's reference, the header's own pilots, the first frame pilot unless hosting
    std::vector<double> t_p{static_cast<double>(u0) + ref * M / 2.0};
    std::vector<std::vector<cd>> hp_rows{out.h_pre};
    for (size_t i = 0; i < L; ++i)
        if (layout[i]) {
            t_p.push_back(t_sym[i]);
            hp_rows.push_back(over_pilot({out.y_all[i], out.y_all[i] + nc}, b));
        }
    if (!hosts(band)) {
        t_p.push_back(static_cast<double>(p0 + NCP) + M / 2.0);
        hp_rows.push_back(out.h_first);
    }
    const auto h_p = equalizer::freq_smooth(rows_of(hp_rows), {HB, HB + NCP}, bb_of(band)).hs;
    const int64_t np_ = static_cast<int64_t>(t_p.size());
    std::vector<size_t> data_rows;
    for (size_t i = 0; i < L; ++i)
        if (!layout[i]) data_rows.push_back(i);
    out.y = Mat<cd>(data_rows.size(), nc);
    Mat<cd> hs(data_rows.size(), nc);
    for (size_t d = 0; d < data_rows.size(); ++d) {
        const double t = t_sym[data_rows[d]];
        const int64_t ss = std::lower_bound(t_p.begin(), t_p.end(), t) - t_p.begin();  // searchsorted, left
        const auto j = static_cast<size_t>(std::clamp<int64_t>(ss - 1, 0, np_ - 2));
        const double a = (t - t_p[j]) / (t_p[j + 1] - t_p[j]);
        std::copy(out.y_all[data_rows[d]], out.y_all[data_rows[d]] + nc, out.y[d]);
        const auto h = lerp(h_p.row(j), h_p.row(j + 1), a);
        std::copy(h.begin(), h.end(), hs[d]);
    }
    const auto soft = qpsk_llr(out.y, hs);
    const double floor = floor_of(band, accept);
    out.hdr = decode_header(soft, band, accept);
    if (copies(band)) {
        // the second copy, at each frame it can be in: a decode counts only if
        // the burst it describes carries its copy there
        std::optional<Header> best;
        for (int kc = 1; kc <= HEADER_COPY_AFTER; ++kc) {
            const auto extra = copy_llr(z, p0 + static_cast<int64_t>(kc) * FRAME_SAMPLES, band, static_cast<int>(out.y.rows));
            if (!extra) {
                out.pending_copy = true;
                continue;
            }
            std::vector<double> sum(soft);
            for (size_t i = 0; i < sum.size(); ++i) sum[i] += (*extra)[i];
            const auto h2 = decode_header(sum, band, accept);
            if (copy_frame(band, h2.n_cw * h2.spec->frames_per_cw) == kc && h2.score >= floor &&
                (!best || h2.score > best->score))
                best = h2;
        }
        if (best) {
            out.hdr = *best;
            out.pending_copy = false;
        }
    }
    out.valid = out.hdr.score >= floor;
    out.p0 = p0;
    out.start = start;
    out.band = b.spec->name;
    out.n0_pre = equalizer::preamble_noise(h_reps);
    out.n0_pre_k = equalizer::preamble_noise_k(h_reps);
    return out;
}

BestHeader best_header(std::span<const cd> z0, std::span<const std::string_view> bands, bool complete,
                       const Accept* accept, std::span<const BandStat> stats, bool final) {
    struct Good {
        double rank;
        HeaderRead hd;
        waveform::Acquisition acq;
        std::shared_ptr<const std::vector<cd>> z;
    };
    std::vector<Good> good;
    int detected = 0;
    bool waiting = false;
    const auto len = static_cast<int64_t>(z0.size());
    for (const auto name : search_bands(bands, accept)) {
        const Mat<double>* S = nullptr;
        for (const auto& st : stats)
            if (st.band == name) S = st.S;
        waveform::Acquisition acq_b;
        try {
            acq_b = waveform::acquire(z0, waveform::band(name), {}, ACQUIRE_REACH_HZ, {}, S);
        } catch (const SyncError&) {
            continue;
        }
        ++detected;
        const int64_t hdr_end = preamble_samples(band(name)) + header_samples(name) + NSYM;
        std::vector<std::pair<int64_t, double>> hyps{{acq_b.preamble_start, acq_b.freq_offset}};
        hyps.insert(hyps.end(), acq_b.alternatives.begin(), acq_b.alternatives.end());
        for (size_t h = 0; h < hyps.size(); ++h) {
            const auto [start, f] = hyps[h];
            if (!complete && start + 2 * M > len - hdr_end) {
                waiting = waiting || !final;
                continue;
            }
            auto zb = std::make_shared<const std::vector<cd>>(waveform::freq_correct(z0, f));
            for (int k : {0, -1, 1, -2, 2}) {
                const int64_t s = start + static_cast<int64_t>(k) * M;
                if (!(0 <= s && s <= len - hdr_end)) continue;
                auto r = read_header(*zb, s, name, accept);
                if (!complete && !final && r.pending_copy && r.hdr.score < COPY_COMMIT_SCORE) {
                    waiting = true;
                    continue;
                }
                if (r.valid && (!complete || burst_end(r.p0, *r.hdr.spec, r.hdr.n_cw) <= len)) {
                    const double rank = r.hdr.score - (h || k ? ALT_PENALTY : 0.0);
                    auto acq = acq_b;
                    acq.preamble_start = start;
                    acq.freq_offset = f;
                    good.push_back({rank, std::move(r), std::move(acq), zb});
                }
            }
        }
    }
    if (good.empty()) throw SyncError(detected ? "header decode failed" : "no preamble found");
    size_t best = 0;  // max(): the first of equal ranks
    for (size_t i = 1; i < good.size(); ++i)
        if (good[i].rank > good[best].rank) best = i;
    if (waiting && good[best].rank < STREAM_COMMIT_SCORE) throw SyncError("a header is still arriving");
    return {std::move(good[best].hd), std::move(good[best].acq), good[best].z};
}

Lock find_burst(std::span<const double> x, std::span<const std::string_view> bands, const Accept* accept,
                std::span<const BandStat> stats) {
    const auto z0 = waveform::to_baseband(x);
    const auto b = best_header(z0, bands, false, accept, stats);
    Lock l;
    l.spec = b.hd.hdr.spec;
    l.n_cw = b.hd.hdr.n_cw;
    l.start = b.hd.start;
    l.score = b.hd.hdr.score;
    l.band = b.hd.band;
    l.end = burst_end(b.hd.p0, *l.spec, l.n_cw);
    l.p0 = b.hd.p0;
    l.cfo = b.acq.freq_offset;
    return l;
}

std::vector<double> pilot_coherence(std::span<const double> x, const Lock& lock, int n_max, bool latest, double* pattern) {
    const Spec& spec = *lock.spec;
    const auto& b = waveform::band(spec.band);
    const auto len = static_cast<int64_t>(x.size());
    const int total = frames_on_air(spec, lock.n_cw) + 1;
    int avail = 0;
    for (int f = 0; f < total; ++f) avail += lock.p0 + static_cast<int64_t>(f) * FRAME_SAMPLES + NCP + M <= len;
    const int f1 = latest ? avail : std::min(avail, n_max + 1);
    const int f0 = std::max(0, f1 - n_max - 1);
    if (pattern) *pattern = 0.0;
    if (f1 - f0 < 2) return {};
    const int64_t lo = std::max<int64_t>(0, lock.p0 + static_cast<int64_t>(f0) * FRAME_SAMPLES - 2 * NSYM);
    const int64_t hi = lock.p0 + static_cast<int64_t>(f1 - 1) * FRAME_SAMPLES + 3 * NSYM;
    const auto z = waveform::freq_correct(waveform::to_baseband(pyslice(x, lo, hi)), lock.cfo);
    std::vector<std::vector<cd>> y;
    for (int f = f0; f < f1; ++f)
        y.push_back(b.demod_window(z, lock.p0 - lo + static_cast<int64_t>(f) * FRAME_SAMPLES + NCP, DEMOD_BACKOFF));
    std::vector<double> out;
    for (size_t f = 1; f < y.size(); ++f)
        out.push_back(std::abs(dot_conj(y[f], y[f - 1])) / (std::sqrt(energy(y[f]) * energy(y[f - 1])) + 1e-30));
    if (pattern) {  // adjacent carriers' products, pilot divided out, summed over the frames; unit weight each
        const std::size_t nc = b.pilot.size();
        std::vector<cd> a(nc - 1, cd{0, 0});
        for (const auto& v : y)
            for (std::size_t k = 0; k + 1 < nc; ++k)
                a[k] += (v[k + 1] * std::conj(b.pilot[k + 1])) * std::conj(v[k] * std::conj(b.pilot[k]));
        cd s{0, 0};
        for (const cd& t : a) s += t / (std::abs(t) + 1e-30);
        *pattern = std::abs(s) / static_cast<double>(a.size());
    }
    return out;
}

std::vector<double> cfo_aliases(cd d, double centre) {
    const double alias = static_cast<double>(FS) / FRAME_SAMPLES;
    const double frac = std::arg(d) / (2 * PI) * alias;
    const double k0 = std::nearbyint((centre - frac) / alias);
    std::vector<double> out;
    for (double k : {k0 - 1, k0, k0 + 1})
        if (std::abs(frac + k * alias - centre) <= waveform::STEP_HZ / 2 + 0.5) out.push_back(frac + k * alias);
    return out;
}

std::optional<Lock> find_copy(std::span<const double> x, std::string_view band, const Accept* accept,
                              const Mat<cd>* C_in, std::optional<double> level, double* peak) {
    if (!C_in && level) throw std::invalid_argument("find_copy: level without C");
    std::vector<const cd*> rows;
    if (C_in)
        for (size_t i = 0; i < C_in->rows; ++i) rows.push_back((*C_in)[i]);
    return find_copy(x, band, accept, rows, C_in ? C_in->cols : 0, level, peak);
}

std::optional<Lock> find_copy(std::span<const double> x, std::string_view band, const Accept* accept,
                              std::span<const cd* const> C_rows, size_t cols, std::optional<double> level,
                              double* peak) {
    const auto& b = waveform::band(band);
    const auto z0 = waveform::to_baseband(x);
    const auto freqs = waveform::cfo_grid();
    Mat<cd> C_own;
    std::vector<const cd*> own_rows;
    if (C_rows.empty()) {
        if (level) throw std::invalid_argument("find_copy: level without C");
        C_own = waveform::repeat_corrs(z0, waveform::unit_template(b), freqs);
        level = waveform::noise_level(C_own);
        for (size_t i = 0; i < C_own.rows; ++i) own_rows.push_back(C_own[i]);
        C_rows = own_rows;
        cols = C_own.cols;
    } else if (!level) {
        throw std::invalid_argument("find_copy: C without level");
    }
    const size_t F = C_rows.size();
    const int64_t n = static_cast<int64_t>(cols) - static_cast<int64_t>(COPY_PAIRS) * FRAME_SAMPLES;
    const int64_t m = n >= 0 ? n / FRAME_SAMPLES : -((-n + FRAME_SAMPLES - 1) / FRAME_SAMPLES);
    if (m < 1) return std::nullopt;
    const size_t FR = FRAME_SAMPLES, mm = static_cast<size_t>(m);
    // fold[f, ph] = sum over frames i (in order) of D[f, i * FR + ph],
    // D = sum_j d[:, j FR : j FR + m FR], d[:, t] = C[:, FR + t] conj(C[:, t])
    Mat<cd> fold(F, FR);
    std::vector<cd> drow(mm * FR);
    for (size_t f = 0; f < F; ++f) {
        const cd* c = C_rows[f];
        // c[FR + u] * conj(c[u]) with the products spelled out: std::complex's operator* carries
        // numpy-unlike NaN handling that stops the loop vectorizing (same values for finite input)
        for (size_t t = 0; t < mm * FR; ++t) {
            double sr = 0.0, si = 0.0;
            for (size_t j = 0; j < static_cast<size_t>(COPY_PAIRS); ++j) {
                const size_t u = j * FR + t;
                const double ar = c[FR + u].real(), ai = c[FR + u].imag(), br = c[u].real(), bi = c[u].imag();
                sr += ar * br + ai * bi;
                si += ai * br - ar * bi;
            }
            drow[t] = {sr, si};
        }
        for (size_t i = 0; i < mm; ++i)
            for (size_t ph = 0; ph < FR; ++ph) fold[f][ph] = i ? fold[f][ph] + drow[i * FR + ph] : drow[ph];
    }
    std::vector<double> mag(FR, -equalizer::INF);
    for (size_t f = 0; f < F; ++f)
        for (size_t ph = 0; ph < FR; ++ph) mag[ph] = std::max(mag[ph], std::abs(fold[f][ph]));
    const double pk = *std::max_element(mag.begin(), mag.end()) / (*level * std::sqrt(static_cast<double>(m * COPY_PAIRS)));
    if (peak) *peak = pk;
    if (pk < COPY_DETECT) return std::nullopt;
    std::vector<int> order(FR);
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](int a, int c) { return -mag[static_cast<size_t>(a)] < -mag[static_cast<size_t>(c)]; });
    std::vector<int> grids;
    for (int ph : order) {
        if (std::all_of(grids.begin(), grids.end(), [&](int g) {
                return std::min(std::abs(ph - g), FRAME_SAMPLES - std::abs(ph - g)) >= NCP;
            })) {
            grids.push_back(ph);
            if (static_cast<int>(grids.size()) == COPY_GRIDS) break;
        }
    }
    const auto& sb = config::band(band);
    const double floor = floor_of(band, accept);
    const int64_t len = static_cast<int64_t>(z0.size());
    std::vector<Lock> found;
    for (int ph : grids) {
        size_t i = 0;
        for (size_t f = 1; f < F; ++f)
            if (std::abs(fold[f][static_cast<size_t>(ph)]) > std::abs(fold[i][static_cast<size_t>(ph)])) i = f;
        for (double f : cfo_aliases(fold[i][static_cast<size_t>(ph)], freqs[i])) {
            const auto z = waveform::freq_correct(z0, f);
            for (int early : COPY_EARLIER) {
                for (int64_t pc = pymod(ph - early - NCP, FRAME_SAMPLES); pc < len - FRAME_SAMPLES; pc += FRAME_SAMPLES) {
                    const auto llr = copy_llr(z, pc, band, sb.header_syms);
                    if (!llr) continue;
                    const auto h = decode_header(*llr, band, accept);
                    const auto kc = copy_frame(band, h.n_cw * h.spec->frames_per_cw);
                    const int64_t p0 = pc - static_cast<int64_t>(kc.value_or(0)) * FRAME_SAMPLES;
                    const int64_t start = p0 - header_samples(band) - preamble_samples(sb);
                    if (kc && h.score >= floor && start >= 0) {
                        Lock l;
                        l.spec = h.spec;
                        l.n_cw = h.n_cw;
                        l.start = start;
                        l.score = h.score;
                        l.band = sb.name;
                        l.end = burst_end(p0, *h.spec, h.n_cw);
                        l.p0 = p0;
                        l.cfo = f;
                        l.copy = CopyRef{h.word, pc};
                        found.push_back(l);
                    }
                }
            }
        }
    }
    std::stable_sort(found.begin(), found.end(), [](const Lock& a, const Lock& c) { return -a.score < -c.score; });
    for (const auto& l : found) {
        double pat = 0.0;
        const auto c = pilot_coherence(x, l, 10000, false, &pat);
        if (c.size() >= 2 && dsp::pairwise_sum(std::span<const double>(c)) / static_cast<double>(c.size()) >= COPY_COHERENCE &&
            pat >= COPY_PATTERN)
            return l;
    }
    return std::nullopt;
}

HeaderRead copy_header(std::span<const cd> z, const Lock& lock) {
    const auto& b = waveform::band(lock.band);
    auto hd = read_header(z, lock.start, lock.band);
    hd.hdr = Header{lock.copy->word, lock.spec, lock.n_cw, lock.score};
    hd.valid = true;
    hd.h_pre = over_pilot(b.demod_window(z, lock.copy->pc + NCP, HB), b);
    return hd;
}

ClipConsts clip_consts(const Spec& spec) {
    const auto& c = CLIP_SUBMODE[config::row(spec)];
    return {{{1, c.gain_1f}}, c.gain, c.ratio};
}

ClipConsts clip_consts(std::string_view name) {
    for (size_t i = 0; i < BANDS.size(); ++i)
        if (BANDS[i].name == name) return {{{1, CLIP_BAND[i].gain_1f}}, CLIP_BAND[i].gain, CLIP_BAND[i].ratio};
    throw std::out_of_range("no band " + std::string(name));
}

DataEstimate data_channel(const Mat<cd>& h_pilot, equalizer::Support support, std::string_view band, double n0_pre,
                          const ClipConsts* clip, std::span<const double> n0_pre_k, int n_frames) {
    DataEstimate est;
    static_cast<equalizer::Estimate&>(est) = equalizer::estimate(h_pilot, support, bb_of(band), n0_pre, n0_pre_k);
    const ClipConsts own = clip ? ClipConsts{} : clip_consts(band);
    const ClipConsts& c = clip ? *clip : own;
    est.clip_ratio = c.ratio;
    const double g = c.gain_for(n_frames < 0 ? static_cast<int>(h_pilot.rows) - 1 : n_frames);
    for (auto& v : est.h.data) v *= g;
    for (auto& v : est.mse.data) v *= g * g;
    est.gain = g;
    est.band = config::band(band).name;
    return est;
}

double resolve_alias(double fine, double coarse) {
    return fine + std::nearbyint((coarse - fine) * equalizer::FRAME_S) / equalizer::FRAME_S;
}

Received receive(std::span<const double> x, std::span<const std::string_view> bands, const Accept* accept,
                 std::optional<int64_t> head, const Lock* copy) {
    const auto z0 = waveform::to_baseband(x);
    const auto len = static_cast<int64_t>(z0.size());
    HeaderRead hd;
    waveform::Acquisition acq;
    std::vector<cd> z;
    if (copy) {
        if (copy->end > len) throw SyncError("burst runs past the buffer");
        z = waveform::freq_correct(z0, copy->cfo);
        hd = copy_header(z, *copy);
        acq = {copy->start, copy->cfo, 0.0, {}};
    } else if (!head) {
        auto b = best_header(z0, bands, true, accept);
        hd = std::move(b.hd);
        acq = std::move(b.acq);
        z = *b.z;
    } else {
        auto b = best_header(pyslice(std::span<const cd>(z0), 0, *head), bands, false, accept, {}, true);
        hd = std::move(b.hd);
        acq = std::move(b.acq);
        z = waveform::freq_correct(z0, acq.freq_offset);
        if (burst_end(hd.p0, *hd.hdr.spec, hd.hdr.n_cw) > len) throw SyncError("burst runs past the buffer");
    }
    const std::string_view sband = hd.band;  // the sync band; frames are on spec.band's carriers
    const auto& b = waveform::band(sband);
    const size_t nc = static_cast<size_t>(b.nc());
    const Spec& spec = *hd.hdr.spec;
    const int n_cw = hd.hdr.n_cw;
    const std::string_view db = spec.band;
    const int n_f = n_cw * spec.frames_per_cw;
    const auto kc = copy_frame(sband, n_f);
    const int n_air = n_f + kc.has_value();
    const double phi_ref = bin_phase_step(hd.h_pre);

    // Residual CFO's alias from the known header symbols (modem.py's notes)
    const auto layout = header_layout(sband);
    const auto hsym = constellation::modulate(codeword(hd.hdr.word, sband), qpsk());
    Mat<cd> ref_syms(layout.size(), nc);
    std::vector<std::vector<cd>> hrows;
    for (size_t r = 0, d = 0; r < layout.size(); ++r) {
        if (layout[r]) {
            std::copy(b.pilot.begin(), b.pilot.end(), ref_syms[r]);
        } else {
            std::copy(hsym.begin() + static_cast<std::ptrdiff_t>(d * nc), hsym.begin() + static_cast<std::ptrdiff_t>((d + 1) * nc),
                      ref_syms[r]);
            hrows.emplace_back(ref_syms[r], ref_syms[r] + nc);
            ++d;
        }
    }
    auto times_conj = [&](std::span<const cd> y, std::span<const cd> r) {
        std::vector<cd> o(y.size());
        for (size_t k = 0; k < o.size(); ++k) o[k] = y[k] * std::conj(r[k]);
        return o;
    };
    std::vector<std::vector<std::vector<cd>>> runs(1);
    for (size_t r = 0; r < layout.size(); ++r) runs[0].push_back(times_conj(hd.y_all.row(r), ref_syms.row(r)));
    if (db == sband) runs[0].push_back(hd.h_first);
    if (kc) {
        const int64_t pc = hd.p0 + static_cast<int64_t>(*kc) * FRAME_SAMPLES;
        std::vector<std::vector<cd>> ref2{b.pilot};
        for (int s = 0; s < DATA_SYMS_PER_FRAME; ++s)
            ref2.push_back(hrows[static_cast<size_t>(s) < hrows.size() ? static_cast<size_t>(s) : static_cast<size_t>(s) - hrows.size()]);
        ref2.push_back(b.pilot);
        std::vector<std::vector<cd>> run;
        for (int s = 0; s <= SYMS_PER_FRAME; ++s)
            run.push_back(times_conj(b.demod_window(z, pc + s * NSYM + NCP, HB), ref2[static_cast<size_t>(s)]));
        runs.push_back(std::move(run));
    }
    cd dsum = 0.0;
    for (const auto& run : runs)
        for (size_t i = 0; i + 1 < run.size(); ++i) dsum += dot_conj(run[i + 1], run[i]);
    const double coarse = std::arg(dsum) / (2 * PI * NSYM / FS);
    // slips are searched for once, here (residual CFO left: the search is blind to a common phase), and replayed
    auto first = demod_frames(z, hd.p0, n_air, 0, phi_ref, {}, db);
    const double fine = equalizer::residual_cfo(first.hp);
    const auto slips = std::move(first.slips);
    const double cfo_res = resolve_alias(fine, coarse);
    z = waveform::freq_correct(z, cfo_res);
    const auto bbd = bb_of(db);
    // the window and the reported delay on the support as before the noise
    // gate (the gear shifter was trained on it); the estimate on the gated one
    const auto hp0 = demod_frames(z, hd.p0, n_air, 0, phi_ref, {}, db, slips).hp;
    auto seen = equalizer::delay_support(hp0, bbd);
    auto support = equalizer::delay_support(hp0, bbd, -15.0, hd.n0_pre);
    const int shift = equalizer::window_shift(seen);
    auto fr = demod_frames(z, hd.p0, n_air, shift, phi_ref, {}, db, slips);
    support = {support.first - shift, support.second - shift};
    seen = {seen.first - shift, seen.second - shift};
    const auto clip = clip_consts(spec);
    Received out;
    out.est = data_channel(fr.hp, support, db, hd.n0_pre, &clip, db == sband ? std::span<const double>(hd.n0_pre_k)
                                                                              : std::span<const double>(), n_f);
    if (kc) {  // the copy frame's pilot stays in hp; its symbols leave the data
        auto drop = [](auto& m, size_t from, size_t count) {
            m.data.erase(m.data.begin() + static_cast<std::ptrdiff_t>(from * m.cols),
                         m.data.begin() + static_cast<std::ptrdiff_t>((from + count) * m.cols));
            m.rows -= count;
        };
        drop(fr.raw, static_cast<size_t>(*kc) * SYMS_PER_FRAME, SYMS_PER_FRAME);
        drop(out.est.h, static_cast<size_t>(*kc) * DATA_SYMS_PER_FRAME, DATA_SYMS_PER_FRAME);
        drop(out.est.mse, static_cast<size_t>(*kc) * DATA_SYMS_PER_FRAME, DATA_SYMS_PER_FRAME);
        out.est.n_f -= 1;
    }
    out.spec = &spec;
    out.n_cw = n_cw;
    out.raw = std::move(fr.raw);
    out.acq = std::move(acq);
    out.band = db;
    out.hp = std::move(fr.hp);
    out.kc = kc;
    out.cfo = out.acq.freq_offset + cfo_res;
    out.p0 = hd.p0;
    out.shift = shift;
    out.steps = std::move(fr.steps);
    out.slips = slips;
    out.phi_ref = phi_ref;
    out.support = support;
    out.seen_support = seen;
    out.preamble_start = hd.start;
    out.score = hd.hdr.score;
    return out;
}

Mat<double> noise_var(const Mat<cd>& h, const DataEstimate& est) {
    Mat<double> v(h.rows, h.cols);
    for (size_t r = 0; r < h.rows; ++r)
        for (size_t k = 0; k < h.cols; ++k)
            v[r][k] = (est.n0_k.empty() ? est.n0 : est.n0_k[k]) + est.clip_ratio * sq_abs(h[r][k]);
    return v;
}

std::vector<double> soft_bits(const Mat<cd>& raw, const Mat<cd>& h, const Mat<double>& var, const Spec& spec) {
    const size_t nc = raw.cols, n_f = raw.rows / SYMS_PER_FRAME;
    if (h.rows != n_f * DATA_SYMS_PER_FRAME || var.rows != h.rows || h.cols != nc || var.cols != nc)
        throw std::invalid_argument("soft_bits: raw, h, var shapes disagree");
    std::vector<cd> y;
    y.reserve(h.data.size());
    for (size_t f = 0; f < n_f; ++f)
        for (int s = 1; s < SYMS_PER_FRAME; ++s) y.insert(y.end(), raw[f * SYMS_PER_FRAME + static_cast<size_t>(s)],
                                                         raw[f * SYMS_PER_FRAME + static_cast<size_t>(s)] + nc);
    return constellation::llr(y, h.data, var.data, points_of(spec));
}

Mat<double> burst_soft(const Received& r, const DataEstimate& est) {
    auto var = noise_var(est.h, est);
    for (size_t i = 0; i < var.data.size(); ++i) var.data[i] += est.mse.data[i];
    Mat<double> out;
    out.data = codes::despread<double>(soft_bits(r.raw, est.h, var, *r.spec), r.n_cw, r.spec->bits_per_cu);
    out.rows = static_cast<size_t>(r.n_cw);
    out.cols = out.data.size() / out.rows;
    return out;
}

std::vector<double> slot_soft(const Received& r, const DataEstimate& est, int slot) {
    const size_t nc = r.raw.cols, total = est.h.data.size(), n = static_cast<size_t>(r.n_cw);
    std::vector<cd> y, h;
    std::vector<double> var;
    for (size_t j = static_cast<size_t>(slot); j < total; j += n) {
        const size_t row = j / nc, c = j % nc;
        const cd hj = est.h.data[j];
        y.push_back(data_row(r.raw, row)[c]);
        h.push_back(hj);
        var.push_back((est.n0_k.empty() ? est.n0 : est.n0_k[c]) + est.clip_ratio * std::norm(hj) + est.mse.data[j]);
    }
    return constellation::llr(y, h, var, points_of(*r.spec));
}

double snr_db(const equalizer::Estimate& est, std::string_view b) {
    return 10 * std::log10(est.p_sig / est.n0 * band(b).nc * RS / SNR_REF_BW_HZ);
}

SymbolTimes symbol_times(const Received& r) {
    const size_t S = DATA_SYMS_PER_FRAME, n_f = r.raw.rows / SYMS_PER_FRAME;
    SymbolTimes out{Mat<double>(n_f, S), std::vector<double>(r.hp.rows)};
    for (size_t f = 0; f < n_f; ++f) {
        const size_t air = f + (r.kc && static_cast<int>(f) >= *r.kc ? 1 : 0);
        for (size_t s = 0; s < S; ++s)
            out.rows[f][s] = static_cast<double>((air * SYMS_PER_FRAME + s + 1) * NSYM) / FS;
    }
    for (size_t i = 0; i < out.pilots.size(); ++i)
        out.pilots[i] = static_cast<double>(i * SYMS_PER_FRAME * NSYM) / FS;
    return out;
}

std::vector<cd> equalized(const Received& r) {
    const auto& h = r.est.h;
    const size_t nc = r.raw.cols, n_f = r.raw.rows / SYMS_PER_FRAME;
    std::vector<cd> out;
    out.reserve(h.data.size());
    for (size_t f = 0; f < n_f; ++f)
        for (size_t s = 1; s < SYMS_PER_FRAME; ++s)
            for (size_t k = 0; k < nc; ++k) {
                const cd hk = h[f * DATA_SYMS_PER_FRAME + s - 1][k];
                out.push_back(std::norm(hk) > 0 ? r.raw[f * SYMS_PER_FRAME + s][k] / hk : cd{});
            }
    return out;
}

std::vector<cd> preview_frame(std::span<const double> x, double cfo, const Spec& spec, int n_f) {
    const auto fr = demod_frames(waveform::freq_correct(waveform::to_baseband(x), cfo), 0, 1, 0, 0.0, {}, spec.band);
    const double g = clip_consts(spec).gain_for(n_f);
    const size_t nc = fr.raw.cols;
    std::vector<cd> out;
    out.reserve(DATA_SYMS_PER_FRAME * nc);
    for (int s = 1; s < SYMS_PER_FRAME; ++s) {
        const double t = static_cast<double>(s) / SYMS_PER_FRAME;
        for (size_t k = 0; k < nc; ++k) {
            const cd h = g * ((1 - t) * fr.hp[0][k] + t * fr.hp[1][k]);
            out.push_back(std::norm(h) > 0 ? fr.raw[static_cast<size_t>(s)][k] / h : cd{});
        }
    }
    return out;
}

Burst decode_received(const Received& r) {
    const Spec& spec = *r.spec;
    Burst out;
    out.soft = burst_soft(r);
    Mat<float> sf(out.soft.rows, out.soft.cols);
    std::copy(out.soft.data.begin(), out.soft.data.end(), sf.data.begin());
    for (auto& p : codes::decode_many(codes::spec(spec), sf)) {
        out.payloads.push_back(std::move(p.data));
        out.crc_ok.push_back(p.ok);
    }
    out.spec = &spec;
    out.freq_offset = r.cfo;
    out.preamble_start = r.preamble_start;
    out.snr_db = snr_db(r.est, spec.band);
    return out;
}

Burst demodulate(std::span<const double> x, std::span<const std::string_view> bands, const Accept* accept) {
    return decode_received(receive(x, bands, accept));
}

}  // namespace data2g::modem
