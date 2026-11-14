#include "arq/nod.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace data2g::arq::nod {

const cpm::Grid& grid() {
    static const cpm::Grid& g = *cpm::grid("c16r25");
    return g;
}

namespace {
std::uint64_t splitmix64(std::uint64_t& state) {
    std::uint64_t z = (state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}
}  // namespace

Family family(const FamilyKey& key) {
    std::uint64_t state = 0x5167ULL << 48 | (static_cast<std::uint64_t>(key[0]) & 0xFFFFFFFFULL) << 16 |
                          static_cast<std::uint64_t>(key[1] & 1) << 8 | static_cast<std::uint64_t>(key[2] & 7) << 4 |
                          static_cast<std::uint64_t>(key[3] & 7);
    Family f(M);
    for (int i = 0; i < M * L; i += 16) {
        const std::uint64_t z = splitmix64(state);
        for (int j = 0; j < 16; ++j) f[(i + j) / L][(i + j) % L] = static_cast<int>((z >> (4 * j)) & 15);
    }
    return f;
}

std::vector<double> waveform(std::span<const int, L> seq, int cap) {
    const cpm::Grid& g = grid();
    return cpm::bandpass(g, cpm::tones(g, seq, cpm::tx_filter(g, cap).glide), cap);
}

double thr(std::int64_t n_pos) { return THR + TAIL * std::log(std::max(1.0, static_cast<double>(n_pos) / THR_POS)); }

std::optional<Detection> detect(std::span<const double> x, std::int64_t lo, std::int64_t hi, double cfo,
                                const Family& fam) {
    const cpm::Grid& g = grid();
    const std::int64_t T = g.T;
    const std::int64_t n_pos = std::max<std::int64_t>(0, (hi - lo) / T) + 1;
    std::array<double, M> best, coh;
    std::array<std::int64_t, M> where{};
    best.fill(-INFINITY);
    coh.fill(-INFINITY);
    for (std::int64_t p = 0; p < T; p += T / 4) {
        const Mat<std::complex<double>> Z = cpm::bins(g, x, lo + p, static_cast<int>(n_pos + L), cfo);
        Mat<double> E(Z.rows, Z.cols);
        for (std::size_t i = 0; i < Z.data.size(); ++i) E.data[i] = std::norm(Z.data[i]);
        const Mat<double> TL = cpm::tone_llr(g, E);
        for (std::int64_t m = 0; m < n_pos; ++m) {
            const std::int64_t s0 = lo + p + m * T;
            if (s0 > hi) continue;
            for (int k = 0; k < M; ++k) {
                double sq = 0.0;
                for (int l = 0; l < L; ++l) sq += TL[static_cast<std::size_t>(m + l)][fam[k][l]];
                if (sq > best[k]) best[k] = sq, where[k] = s0;
            }
        }
    }
    std::array<int, M> order;
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return best[a] > best[b]; });
    for (int j = 0; j < TOPK; ++j) {
        const int k = order[j];
        const Mat<std::complex<double>> Z = cpm::bins(g, x, where[k], L, cfo);
        Mat<double> E(Z.rows, Z.cols);
        for (std::size_t i = 0; i < Z.data.size(); ++i) E.data[i] = std::norm(Z.data[i]);
        const auto ll = cpm::coh_run_llr(g, Z, fam[k], cpm::noise_n0(E));
        coh[k] = std::accumulate(ll.begin(), ll.end(), 0.0);
    }
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return coh[a] > coh[b]; });
    const double b = coh[order[0]], r = coh[order[1]];
    const double t = thr(n_pos);
    if (b < t || r >= t || order[0] >= N_MEANINGS) return std::nullopt;
    return Detection{order[0], where[order[0]], b};
}

Meaning meaning(int i) { return {i < 10, REPLY_STEPS[static_cast<std::size_t>((i % 10) / 2)], (i % 2) != 0}; }

std::optional<int> index(bool all, int step, bool dup) {
    const auto it = std::find(REPLY_STEPS.begin(), REPLY_STEPS.end(), step);
    if (it == REPLY_STEPS.end()) return std::nullopt;
    return (all ? 0 : 10) + static_cast<int>(it - REPLY_STEPS.begin()) * 2 + (dup ? 1 : 0);
}

std::string describe(int i) {
    const Meaning m = meaning(i);
    return std::string("('") + (m.all ? "all" : "unchanged") + "', " + std::to_string(m.step) + ", " +
           std::to_string(m.dup ? 1 : 0) + ")";
}

}  // namespace data2g::arq::nod
