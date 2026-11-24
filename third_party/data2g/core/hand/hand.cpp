#include "hand/hand.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <limits>
#include <numbers>
#include <tuple>

#include "codes/codes.hpp"
#include "cpm/cpm.hpp"
#include "dsp/dsp.hpp"
#include "dsp/fft.hpp"
#include "polar/polar.hpp"
#include "tables/hand.hpp"

namespace data2g::hand {
namespace {

using cdouble = std::complex<double>;
constexpr double FS = config::FS;
constexpr double PI = std::numbers::pi;

constexpr int SYNC_AT[2] = {0, N_SYNC + N_DATA / 2};  // the two Costas blocks' first rows

// Symbol windows per strided block transform: 32 search starts (192 windows)
// or 8 nail offsets (216 windows), ~4 MB of transforms each.  Big enough that
// pocketfft's per-call setup is noise next to the arithmetic (one call per
// block instead of one per window) and that its own threads (dsp::fft_thread_count,
// which splits a batch's rows, never a single row) have something to divide;
// small enough that a block's transforms stay in the better caches. Taking
// the whole block at once measures no faster and costs 9 MB.
constexpr int SEARCH_CHUNK = 32;
constexpr int NAIL_CHUNK = 8;

bool is_sync_row(int r) { return (r >= SYNC_AT[0] && r < SYNC_AT[0] + N_SYNC) || (r >= SYNC_AT[1] && r < SYNC_AT[1] + N_SYNC); }

const std::vector<int>& data_rows() {
    static const std::vector<int> rows = [] {
        std::vector<int> v;
        for (int r = 0; r < N_SYM; ++r)
            if (!is_sync_row(r)) v.push_back(r);
        return v;
    }();
    return rows;
}

// cpm.Grid(f"hand{freq:g}", 8, 6.25, center=freq): only m, rate, T, bits and f0 are read.
const cpm::Grid& grid(int c) {
    static const std::array<cpm::Grid, N_CHANNELS> grids = [] {
        std::array<cpm::Grid, N_CHANNELS> gs{};
        for (int i = 0; i < N_CHANNELS; ++i) {
            auto& g = gs[static_cast<std::size_t>(i)];
            g.name = "hand";
            g.m = M;
            g.rate = RATE;
            g.center = channel_freq(i);
            g.bp = 50.0;
            g.T = static_cast<int>(std::lround(config::FS / RATE));
            g.bits = 3;
            // Python's round(): half to even, as nearbyint under the default rounding mode
            g.f0 = std::nearbyint((g.center - (M - 1) * RATE / 2) / RATE) * RATE;
        }
        return gs;
    }();
    return grids.at(static_cast<std::size_t>(c));
}

const polar::SCLDecoder& decoder() {
    static const polar::SCLDecoder dec(polar::PolarCode(K, E, polar::ga_info_pos(K, E)), codes::POLAR_LIST);
    return dec;
}

struct Cand {
    std::int64_t start;
    double cfo;
};

// A window's decode scratch, reused across its channels, cfos and candidates:
// the mixed window plus a margin of silence on either side, and one batch's
// worth of transforms and energies.
//
// The margin is what lets the transforms read the mixed window in place: every
// symbol window the search and the nailing ask for lies inside z, and one that
// runs off the end of the capture reads zeros, exactly what hand.bins pads a
// short segment with. Without it each window would need its own gather and
// zero-fill (a previous form copied 1.35 MB per offset, 6200 times a round).
struct Scratch {
    std::int64_t left = 0;  // z's samples before y[0]
    std::int64_t n = 0;     // y's samples
    std::vector<cdouble> z;
    std::vector<cdouble> Z;  // one block's transforms
};

// The margins for one window: the lowest and highest symbol window either pass
// reaches. `nominal` is the slot; the search walks +-SEARCH_SAMPLES of it, the
// nailing another half symbol out from any candidate it found.
void prepare(Scratch& s, std::span<const double> y, std::int64_t nominal, int T) {
    s.n = static_cast<std::int64_t>(y.size());
    const std::int64_t lo = nominal - SEARCH_SAMPLES - T / 2;
    const std::int64_t hi = nominal + SEARCH_SAMPLES + T / 2 + static_cast<std::int64_t>(N_SYM) * T;
    s.left = std::max<std::int64_t>(0, -lo);
    s.z.assign(static_cast<std::size_t>(s.left + s.n + std::max<std::int64_t>(0, hi - s.n)), cdouble{0, 0});
}

// hand.mix: the window mixed down for a cfo, at cpm._energies' own factor and
// absolute sample times, into the scratch's middle. The silence margins are
// already zero and stay that way, so this is one pass over y per cfo (7 plus
// 3x3 a channel), not one per (start, cfo) as cpm._energies would do.
// The oscillator is a phasor recurrence, set exactly every RESYNC samples, so
// one sincos per RESYNC rather than per sample. The drift over a stretch is
// ~1e-14, far inside the 1e-6 Python agreement.
void mix(Scratch& s, const cpm::Grid& g, std::span<const double> y, double cfo) {
    constexpr std::int64_t RESYNC = 256;
    const double w = (-2 * PI) * (g.f0 + cfo);
    const cdouble step = std::polar(1.0, w / FS);
    for (std::int64_t i0 = 0; i0 < s.n; i0 += RESYNC) {
        cdouble p = std::polar(1.0, w * (static_cast<double>(i0) / FS));
        const std::int64_t end = std::min(s.n, i0 + RESYNC);
        for (std::int64_t i = i0; i < end; ++i, p *= step)
            s.z[static_cast<std::size_t>(s.left + i)] = y[static_cast<std::size_t>(i)] * p;
    }
}

// Energies of a rectangle of symbol windows out of the mixed window: windows
// at base + a*stride + b*T for a < na, b < nb, in the same row order the
// Python reference walks them. One strided block transform (dsp::fft.hpp) over
// z, then the m bins cpm::measure and cpm::llrs read. `En` is the caller's, so
// two blocks can be scored against each other.
void block_energies(Scratch& s, std::int64_t base, int na, int nb, int stride, int T, int m, Mat<double>& En) {
    const std::size_t rows = static_cast<std::size_t>(na) * static_cast<std::size_t>(nb);
    if (s.Z.size() < rows * static_cast<std::size_t>(T)) s.Z.resize(rows * static_cast<std::size_t>(T));
    dsp::fft_block_into(s.z.data() + s.left + base, s.Z.data(), static_cast<std::size_t>(na), static_cast<std::size_t>(nb),
                        static_cast<std::size_t>(T), stride, T, true, dsp::fft_thread_count());
    if (En.rows != rows || En.cols != static_cast<std::size_t>(m)) En = Mat<double>(rows, static_cast<std::size_t>(m));
    for (std::size_t r = 0; r < rows; ++r)
        for (int k = 0; k < m; ++k) {
            const cdouble& c = s.Z[r * static_cast<std::size_t>(T) + static_cast<std::size_t>(k)];
            En[r][k] = c.real() * c.real() + c.imag() * c.imag();
        }
}

// hand.search: top (start, cfo) candidates by the sync blocks' tone shares --
// a share, so a strong stretch of data can't outscore a sync block in a fade.
// Each cfo's starts (61 with ROUND_FULL's T/8 step, 31 with ROUND_CHEAP's T/4)
// go through the two Costas blocks as one strided block transform apiece; the
// score is the same sum of shares, block by block and tone by tone, that
// Python's sync_shares built.
std::vector<Cand> search(const cpm::Grid& g, Scratch& s, std::span<const double> y, std::int64_t nominal, const Round& round) {
    const int T = g.T;
    const int step = T / round.step_div;
    std::vector<std::tuple<double, std::int64_t, double>> cands;
    const double cstep = g.rate / 4;
    const std::int64_t first = nominal - SEARCH_SAMPLES;
    const int nstarts = static_cast<int>(2 * SEARCH_SAMPLES / step + 1);  // Python's range() count
    Mat<double> En;
    for (int i = 0;; ++i) {
        const double cfo = -SEARCH_HZ + i * cstep;  // np.arange's start + i * step
        if (cfo >= SEARCH_HZ + 1e-9) break;
        mix(s, g, y, cfo);
        std::vector<double> score(static_cast<std::size_t>(nstarts), 0.0);
        for (int r0 : SYNC_AT)
            for (int a0 = 0; a0 < nstarts; a0 += SEARCH_CHUNK) {
                const int na = std::min(SEARCH_CHUNK, nstarts - a0);
                block_energies(s, first + static_cast<std::int64_t>(a0) * step + static_cast<std::int64_t>(r0) * T,
                               na, N_SYNC, step, T, g.m, En);
                const auto Sh = cpm::shares(En);
                for (int a = 0; a < na; ++a) {
                    double part = 0.0;
                    for (int j = 0; j < N_SYNC; ++j)
                        part += Sh[static_cast<std::size_t>(a * N_SYNC + j)][tables::HAND_SYNC[static_cast<std::size_t>(j)]];
                    score[static_cast<std::size_t>(a0 + a)] += part;
                }
            }
        for (int a = 0; a < nstarts; ++a) cands.emplace_back(score[static_cast<std::size_t>(a)], first + a * step, cfo);
    }
    std::sort(cands.begin(), cands.end(), std::greater<>());  // Python's sort(reverse=True) on the tuples
    std::vector<Cand> out;
    for (const auto& [share, start, cfo] : cands) {  // distinct peaks: half a symbol or a quarter tone apart
        const bool distinct = std::all_of(out.begin(), out.end(), [&](const Cand& o) {
            return std::abs(start - o.start) > T / 2 || std::abs(cfo - o.cfo) > g.rate / 4 + 1e-9;
        });
        if (distinct) out.push_back({start, cfo});
        if (static_cast<int>(out.size()) == round.top) break;
    }
    return out;
}

// hand.nail: a candidate -> (start, cfo, data-row energies) nailed in on T/16
// x rate/16 steps.  The search's T/8 step can land a quarter symbol off, and
// that leaks every symbol's energy across two bins: ~4 dB of the SNR
// reading, and the same leak in the soft bits, which is why nailing comes
// before the decode rather than after the CRC.  Metric: the data rows'
// winning shares.  df outer, dt inner: a tie goes to the same cell Python's
// loop order reaches first.  Only the 54 data rows are transformed (the sync
// rows are thrown away by both passes here), as two strided blocks of 27.
std::tuple<std::int64_t, double, Mat<double>> nail(const cpm::Grid& g, Scratch& s, std::span<const double> y,
                                                   std::int64_t start, double cfo, const Round& round) {
    const int T = g.T;
    const int ndt = T / (T / 16) + 1;  // -T/2 .. +T/2 step T/16
    const int half = N_DATA / 2;
    // the CFO steps: cheap nails on the search's own CFO grid
    const std::array<double, 3> all_dfs{-g.rate / 16, 0.0, g.rate / 16};
    const std::span<const double> dfs =
        round.nail_cfo ? std::span<const double>(all_dfs) : std::span<const double>(all_dfs.data() + 1, 1);
    std::int64_t best_start = start;
    double best_cfo = cfo, best_v = -std::numeric_limits<double>::infinity();
    Mat<double> best_En, EA, EB;
    for (double df : dfs) {
        mix(s, g, y, cfo + df);
        for (int d0 = 0; d0 < ndt; d0 += NAIL_CHUNK) {
            const int nd = std::min(NAIL_CHUNK, ndt - d0);
            const std::int64_t base = start - T / 2 + static_cast<std::int64_t>(d0) * (T / 16);
            block_energies(s, base + static_cast<std::int64_t>(N_SYNC) * T, nd, half, T / 16, T, g.m, EA);
            block_energies(s, base + static_cast<std::int64_t>(SYNC_AT[1] + N_SYNC) * T, nd, half, T / 16, T, g.m, EB);
            const auto SA = cpm::shares(EA), SB = cpm::shares(EB);
            for (int d = 0; d < nd; ++d) {
                std::vector<double> top(static_cast<std::size_t>(N_DATA));
                for (int k = 0; k < half; ++k) {
                    const double* ra = SA[static_cast<std::size_t>(d * half + k)];
                    const double* rb = SB[static_cast<std::size_t>(d * half + k)];
                    top[static_cast<std::size_t>(k)] = *std::max_element(ra, ra + g.m);
                    top[static_cast<std::size_t>(half + k)] = *std::max_element(rb, rb + g.m);
                }
                const double v = dsp::pairwise_sum(top);
                if (v > best_v) {
                    best_v = v;
                    best_start = start - T / 2 + static_cast<std::int64_t>(d0 + d) * (T / 16);
                    best_cfo = cfo + df;
                    best_En = Mat<double>(static_cast<std::size_t>(N_DATA), static_cast<std::size_t>(g.m));
                    for (int k = 0; k < half; ++k) {
                        std::copy_n(EA[static_cast<std::size_t>(d * half + k)], g.m, best_En[static_cast<std::size_t>(k)]);
                        std::copy_n(EB[static_cast<std::size_t>(d * half + k)], g.m,
                                    best_En[static_cast<std::size_t>(half + k)]);
                    }
                }
            }
        }
    }
    return {best_start, best_cfo, std::move(best_En)};
}

}  // namespace

double channel_freq(int c) {
    const double off = PITCH / 2 + (c / 2) * PITCH;
    return CENTER + (c % 2 ? off : -off);
}

int channels_within(double width_hz) {
    int n = 0;
    for (int c = 0; c < N_CHANNELS; ++c) n += std::abs(channel_freq(c) - CENTER) + M * RATE / 2 <= width_hz / 2;
    return n;
}

std::vector<std::uint8_t> payload_bits(std::uint64_t p) {
    std::vector<std::uint8_t> b(PAYLOAD);
    for (int i = 0; i < PAYLOAD; ++i) b[static_cast<std::size_t>(i)] = (p >> (PAYLOAD - 1 - i)) & 1;
    return b;
}

std::uint64_t payload_int(std::span<const std::uint8_t> bits) {
    std::uint64_t p = 0;
    for (int i = 0; i < PAYLOAD; ++i) p = (p << 1) | (bits[static_cast<std::size_t>(i)] & 1);
    return p;
}

std::vector<std::uint8_t> crc16(std::span<const std::uint8_t> bits, unsigned init) {
    unsigned c = init;
    for (auto b : bits) {
        const unsigned fb = ((c >> 15) & 1) ^ (b & 1);
        c = ((c << 1) & 0xFFFF) ^ (fb ? 0x1021 : 0);
    }
    std::vector<std::uint8_t> out(16);
    for (int i = 0; i < 16; ++i) out[static_cast<std::size_t>(i)] = (c >> (15 - i)) & 1;
    return out;
}

std::vector<double> modulate(int tag, std::span<const std::uint8_t> payload, int c) {
    const cpm::Grid& g = grid(c);
    Mat<std::uint8_t> u(1, K);
    std::copy(payload.begin(), payload.end(), u[0]);
    const auto crc = crc16(payload, crc_init(tag, c));
    std::copy(crc.begin(), crc.end(), u[0] + PAYLOAD);
    const auto coded = decoder().code().encode(u);
    std::vector<std::uint8_t> sent(E);
    for (int j = 0; j < E; ++j) sent[static_cast<std::size_t>(j)] = coded[0][tables::HAND_INTERLEAVER[static_cast<std::size_t>(j)]];
    const auto data = cpm::to_tones(g, sent);
    std::vector<int> sym(N_SYM);
    for (int r0 : SYNC_AT)
        for (int j = 0; j < N_SYNC; ++j) sym[static_cast<std::size_t>(r0 + j)] = tables::HAND_SYNC[static_cast<std::size_t>(j)];
    const auto& dr = data_rows();
    for (std::size_t i = 0; i < dr.size(); ++i) sym[static_cast<std::size_t>(dr[i])] = data[i];
    return cpm::tones(g, sym);
}

std::vector<double> tx_audio(int tag, std::uint64_t payload, int channel, double cfo) {
    auto x = modulate(tag, payload_bits(payload), channel);
    if (cfo != 0.0) {  // hfchannel.freq_shift, the phase wrapped to one turn first
        const auto z = dsp::hilbert(x);
        for (std::size_t n = 0; n < x.size(); ++n) {
            const double c = cfo * static_cast<double>(n) / config::FS;
            x[n] = (z[n] * std::exp(dsp::cdouble(0.0, 2 * std::numbers::pi * (c - std::floor(c))))).real();
        }
    }
    double peak = 0.0;
    for (const auto& z : dsp::hilbert(x)) peak = std::max(peak, std::abs(z));
    for (double& v : x) v /= peak;
    return x;
}

std::vector<Decoded> decode_window(std::span<const double> y, int tag, std::span<const int> channels, std::int64_t nominal,
                                   Round round) {
    const auto& dec = decoder();
    std::vector<Decoded> out;
    if (channels.empty() || y.empty()) return out;
    Scratch s;
    prepare(s, y, nominal, grid(channels.front()).T);  // every channel's symbol is the same length
    for (int c : channels) {
        const auto& g = grid(c);
        const auto cands = search(g, s, y, nominal, round);
        // hand.receive, one candidate at a time instead of all three and then a
        // batched decode: nailing is 2/3 of the work per channel and a burst on
        // the first candidate makes the other two nails dead weight. Same LLRs
        // either way (the decoder's rows are independent).
        for (const auto& cd : cands) {
            const auto [start, cfo, En] = nail(g, s, y, cd.start, cd.cfo, round);
            const auto l = cpm::llrs(g, En);
            Mat<float> llr(1, E);
            for (int j = 0; j < E; ++j)
                llr[0][tables::HAND_INTERLEAVER[static_cast<std::size_t>(j)]] = static_cast<float>(l[static_cast<std::size_t>(j)]);
            const auto res = dec.decode(llr);
            bool done = false;
            for (int li = 0; li < dec.list_size() && !done; ++li) {
                const std::span<const std::uint8_t> u(res.paths[0] + static_cast<std::size_t>(li) * K, K);
                const auto crc = crc16(u.first(PAYLOAD), crc_init(tag, c));
                if (!std::equal(crc.begin(), crc.end(), u.begin() + PAYLOAD)) continue;
                out.push_back({c, payload_int(u), cpm::measure(g, En, N_DATA).snr_est,
                               1000.0 * static_cast<double>(start - nominal) / config::FS});
                done = true;
            }
            if (done) break;  // one Hand per channel: the other candidates are the same burst
        }
    }
    return out;
}

}  // namespace data2g::hand
