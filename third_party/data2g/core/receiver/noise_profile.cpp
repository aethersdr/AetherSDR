#include "receiver/noise_profile.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "dsp/dsp.hpp"
#include "dsp/fft.hpp"

namespace data2g::receiver {

NoiseProfile::NoiseProfile() : win_(BLOCK) {
    for (int n = 0; n < BLOCK; ++n) win_[n] = 0.5 - 0.5 * std::cos(2 * std::numbers::pi * n / (BLOCK - 1));  // np.hanning
}

void NoiseProfile::feed(std::span<const double> x, double t_start) {
    const auto s = static_cast<std::int64_t>(std::nearbyint(t_start * config::FS));
    if (!buf_.empty() && s0_ + static_cast<std::int64_t>(buf_.size()) == s) {
        buf_.insert(buf_.end(), x.begin(), x.end());
    } else {
        buf_.assign(x.begin(), x.end());
        s0_ = s;
    }
    std::size_t off = 0;
    std::vector<dsp::cdouble> z(BLOCK);
    while (buf_.size() - off >= static_cast<std::size_t>(BLOCK)) {
        for (int n = 0; n < BLOCK; ++n) z[n] = buf_[off + n] * win_[n];
        const auto spec = dsp::fft(z, true);
        Block b{s0_, s0_ + BLOCK, {}, 0, {}};
        b.spec.resize(BLOCK / 2 + 1);
        for (std::size_t k = 0; k < b.spec.size(); ++k) b.spec[k] = std::norm(spec[k]);
        recent_.push_back({s0_, s0_ + BLOCK, b.spec});
        if (recent_.size() > RECENT) recent_.pop_front();
        {  // impulses: 10 ms pieces whose peak is over IMPULSE_X x the median piece RMS
            std::vector<double> rms, peak;
            for (int q = 0; q < BLOCK; q += PIECE) {
                double ss = 0.0, pk = 0.0;
                for (int n = 0; n < PIECE; ++n) {
                    const double v = buf_[off + q + n];
                    ss += v * v;
                    pk = std::max(pk, std::fabs(v));
                }
                rms.push_back(std::sqrt(ss / PIECE));
                peak.push_back(pk);
            }
            const double ref = dsp::quantile(rms, 0.5);
            if (ref > 0)
                for (const double pk : peak) b.impulses += pk > IMPULSE_X * ref;
        }
        for (std::size_t i = 0; i < BANDS_HZ.size(); ++i) {
            // rfft bins k (k * FS / BLOCK Hz) with lo <= f < hi
            const int bin_hz = config::FS / BLOCK;
            const int k0 = (BANDS_HZ[i].first + bin_hz - 1) / bin_hz, k1 = (BANDS_HZ[i].second + bin_hz - 1) / bin_hz;
            double sum = 0.0;
            for (int k = k0; k < k1; ++k) sum += std::norm(spec[static_cast<std::size_t>(k)]);
            b.p[i] = sum / (k1 - k0);
        }
        pending_.push_back(std::move(b));
        off += BLOCK;
        s0_ += BLOCK;
    }
    buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(off));
    const std::int64_t now = s + static_cast<std::int64_t>(x.size());
    const auto commit = static_cast<std::int64_t>(std::nearbyint(COMMIT_S * config::FS));
    while (!pending_.empty() && pending_.front().end <= now - commit) {
        Block b = std::move(pending_.front());
        pending_.pop_front();
        const bool marked = std::any_of(busy_.begin(), busy_.end(),
                                        [&](const auto& m) { return m.first < b.end && b.start < m.second; });
        if (!marked) {
            kept_.push_back(b.p);
            kept_impulses_.push_back(b.impulses);
            kept_spec_.push_back(std::move(b.spec));
            if (kept_.size() > WINDOW) kept_.pop_front(), kept_impulses_.pop_front(), kept_spec_.pop_front();
        }
    }
    std::erase_if(busy_, [&](const auto& m) { return m.second <= now - 2 * commit; });
}

NoiseProfile::Spectrum NoiseProfile::spectrum(std::span<const double> blk) const {
    std::vector<dsp::cdouble> z(BLOCK);
    for (int n = 0; n < BLOCK; ++n) z[n] = blk[static_cast<std::size_t>(n)] * win_[n];
    const auto spec = dsp::fft(z, true);
    Spectrum out(BLOCK / 2 + 1);
    for (std::size_t k = 0; k < out.size(); ++k) out[k] = std::norm(spec[k]);
    return out;
}

std::vector<NoiseProfile::Spectrum> NoiseProfile::span_spectra(double start, double end) const {
    const auto a = static_cast<std::int64_t>(std::nearbyint(start * config::FS));
    const auto b = static_cast<std::int64_t>(std::nearbyint(end * config::FS));
    std::vector<Spectrum> out;
    for (const auto& r : recent_)
        if (r.start >= a && r.end <= b) out.push_back(r.spec);
    return out;
}

std::vector<std::size_t> NoiseProfile::band_bins(double lo, double hi) {
    // rfft bins k (f = k * FS / BLOCK Hz) with lo <= f < hi, as np.fft.rfftfreq
    const double df = static_cast<double>(config::FS) / BLOCK;
    std::vector<std::size_t> band;
    for (std::size_t k = 0; k <= static_cast<std::size_t>(BLOCK / 2); ++k) {
        const double f = static_cast<double>(k) * df;
        if (f >= lo && f < hi) band.push_back(k);
    }
    return band;
}

double NoiseProfile::band_mean(const Spectrum& s, const std::vector<std::size_t>& band) {
    std::vector<double> v;
    v.reserve(band.size());
    for (const auto k : band) v.push_back(s[k]);
    return dsp::pairwise_sum(v) / static_cast<double>(v.size());
}

std::optional<double> NoiseProfile::span_snr_db(const std::vector<Spectrum>& spectra, double lo, double hi,
                                                const std::vector<Spectrum>* floor_spectra) const {
    const std::size_t n_ref = floor_spectra ? floor_spectra->size() : kept_spec_.size();
    if ((!floor_spectra && n_ref < MIN_BLOCKS) || n_ref == 0 || spectra.empty()) return std::nullopt;
    const auto band = band_bins(lo, hi);
    std::vector<double> ref, pw;
    if (floor_spectra)
        for (const auto& s : *floor_spectra) ref.push_back(band_mean(s, band));
    else
        for (const auto& s : kept_spec_) ref.push_back(band_mean(s, band));
    for (const auto& s : spectra) pw.push_back(band_mean(s, band));
    const double floor = dsp::quantile(ref, 0.5);
    const double power = dsp::pairwise_sum(pw) / static_cast<double>(pw.size());
    if (floor <= 0) return std::nullopt;
    return 10 * std::log10(std::max(power / floor - 1, 1e-3) * (hi - lo) / config::SNR_REF_BW_HZ);
}

std::optional<double> NoiseProfile::span_z(const std::vector<Spectrum>& spectra, double lo, double hi) const {
    if (kept_spec_.size() < MIN_BLOCKS || spectra.empty()) return std::nullopt;
    const auto band = band_bins(lo, hi);
    std::vector<double> ref, dev, pw;
    for (const auto& s : kept_spec_) ref.push_back(band_mean(s, band));
    const double floor = dsp::quantile(ref, 0.5);
    for (const auto r : ref) dev.push_back(std::abs(r - floor));
    const double spread = 1.4826 * dsp::quantile(dev, 0.5);
    if (spread <= 0) return std::nullopt;
    for (const auto& s : spectra) pw.push_back(band_mean(s, band));
    const double power = dsp::pairwise_sum(pw) / static_cast<double>(pw.size());
    return (power - floor) / spread * std::sqrt(static_cast<double>(pw.size()));
}

void NoiseProfile::mark(double start, double end) {
    busy_.emplace_back(static_cast<std::int64_t>(std::nearbyint(start * config::FS)),
                       static_cast<std::int64_t>(std::nearbyint(end * config::FS)));
}

std::optional<NoiseSnapshot> NoiseProfile::snapshot() const {
    if (kept_.size() < MIN_BLOCKS) return std::nullopt;
    NoiseSnapshot out;
    out.blocks = static_cast<int>(kept_.size());
    double imp = 0.0;
    for (const int n : kept_impulses_) imp += n;
    out.impulses_per_min = 60.0 * imp / (static_cast<double>(kept_.size()) * BLOCK / config::FS);
    for (std::size_t i = 0; i < BANDS_HZ.size(); ++i) {
        std::vector<double> v;
        v.reserve(kept_.size());
        for (const auto& p : kept_) v.push_back(p[i]);
        const double med = std::max(dsp::quantile(v, 0.5), 1e-30);  // digital silence
        out.db[i] = 10 * std::log10(med);
        out.tail_db[i] = 10 * std::log10(dsp::quantile(v, 0.9) / med);
    }
    return out;
}

}  // namespace data2g::receiver
