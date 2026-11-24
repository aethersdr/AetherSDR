#include "cwid/cwid.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace data2g::cwid {

namespace {

std::string_view morse(char c) {
    static constexpr std::array<std::pair<char, std::string_view>, 37> TABLE = {{
        {'A', ".-"},    {'B', "-..."},  {'C', "-.-."},  {'D', "-.."},   {'E', "."},     {'F', "..-."},
        {'G', "--."},   {'H', "...."},  {'I', ".."},    {'J', ".---"},  {'K', "-.-"},   {'L', ".-.."},
        {'M', "--"},    {'N', "-."},    {'O', "---"},   {'P', ".--."},  {'Q', "--.-"},  {'R', ".-."},
        {'S', "..."},   {'T', "-"},     {'U', "..-"},   {'V', "...-"},  {'W', ".--"},   {'X', "-..-"},
        {'Y', "-.--"},  {'Z', "--.."},  {'0', "-----"}, {'1', ".----"}, {'2', "..---"}, {'3', "...--"},
        {'4', "....-"}, {'5', "....."}, {'6', "-...."}, {'7', "--..."}, {'8', "---.."}, {'9', "----."},
        {'/', "-..-."},
    }};
    for (const auto& [k, p] : TABLE)
        if (k == c) return p;
    return {};
}

char up(char c) { return static_cast<char>(std::toupper(static_cast<unsigned char>(c))); }

}  // namespace

std::string id_text(std::string_view call) {
    std::string s(call.substr(0, call.find('-')));
    std::transform(s.begin(), s.end(), s.begin(), up);
    return "DE " + s;
}

std::vector<std::pair<bool, int>> keying(std::string_view text) {
    std::vector<std::pair<bool, int>> out;
    std::size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
        if (i == text.size()) break;
        if (!out.empty()) out.emplace_back(false, 7);
        bool first = true;
        for (; i < text.size() && !std::isspace(static_cast<unsigned char>(text[i])); ++i) {
            const auto p = morse(up(text[i]));
            if (p.empty()) continue;
            if (!first) out.emplace_back(false, 3);
            first = false;
            for (std::size_t e = 0; e < p.size(); ++e) {
                if (e) out.emplace_back(false, 1);
                out.emplace_back(true, p[e] == '-' ? 3 : 1);
            }
        }
    }
    return out;
}

int unit_samples(double wpm) { return static_cast<int>(std::nearbyint(config::FS * 1.2 / wpm)); }  // Python's round

std::vector<double> audio(std::string_view text, double wpm, double freq) {
    if (!(wpm >= MIN_WPM && wpm <= MAX_WPM)) throw std::invalid_argument("CW ID speed outside 15-40 wpm");
    const int unit = unit_samples(wpm);
    std::vector<double> key;
    for (const auto& [on, d] : keying(text)) key.insert(key.end(), static_cast<std::size_t>(d * unit), on ? 1.0 : 0.0);
    key.insert(key.end(), RAMP, 0.0);
    std::array<double, RAMP> w{};
    double sum = 0.0;
    for (int j = 0; j < RAMP; ++j) sum += w[j] = std::sin(std::numbers::pi * (j + 0.5) / RAMP);
    std::vector<double> out(key.size());
    for (std::size_t n = 0; n < key.size(); ++n) {
        double env = 0.0;
        for (int j = 0; j < RAMP && static_cast<std::size_t>(j) <= n; ++j) env += key[n - j] * (w[j] / sum);
        out[n] = env * std::sin(2 * std::numbers::pi * freq * static_cast<double>(n) / config::FS);
    }
    return out;
}

double seconds(std::string_view text, double wpm) {
    int dots = 0;
    for (const auto& [on, d] : keying(text)) dots += d;
    return static_cast<double>(dots * unit_samples(wpm) + RAMP) / config::FS;
}

CwSense::CwSense(double center) {
    basis_.resize(static_cast<std::size_t>((2 * K + 1) * WIN));
    for (int k = -K; k <= K; ++k)
        for (int n = 0; n < WIN; ++n) {
            const double hann = 0.5 - 0.5 * std::cos(2 * std::numbers::pi * (n + 0.5) / WIN);
            const double f = center + STEP_HZ * k;
            basis_[static_cast<std::size_t>((k + K) * WIN + n)] =
                hann * std::exp(std::complex<double>(0.0, -2 * std::numbers::pi * f * n / config::FS));
        }
    reset();
}

void CwSense::reset() {
    buf_.clear();
    hist_.clear();
    floor_ = 0.0;
    active_ = false;
    n_ = 0;
    hits_.clear();
    tones_.clear();
}

int CwSense::classify(std::span<const double> x) {
    std::array<double, 2 * K + 1> p{};
    for (int i = 0; i < 2 * K + 1; ++i) {
        std::complex<double> z = 0.0;
        for (int n = 0; n < WIN; ++n) z += basis_[static_cast<std::size_t>(i * WIN + n)] * x[n];
        p[i] = std::norm(z);
    }
    int kt = -TONE_K;
    for (int k = -TONE_K + 1; k <= TONE_K; ++k)
        if (p[k + K] > p[kt + K]) kt = k;
    const double tone = p[kt + K];
    double m = 0.0;
    int nm = 0;
    for (int k = -K; k <= K; ++k)
        if (std::abs(k) >= FLOOR_K) m += p[k + K], ++nm;
    m = std::max(m / nm, 1e-30);
    if (floor_ == 0.0) floor_ = m;
    const double thr = RATIO * floor_;
    floor_ *= std::pow(m / floor_, m < floor_ ? FLOOR_DOWN : FLOOR_UP);
    double apart = 0.0;
    for (int k = -K; k <= K; ++k)
        if (std::abs(k - kt) >= APART_K) apart = std::max(apart, p[k + K]);
    // LEAK against the key-down level, the strongest tone of WIN_N windows (cwid.py)
    tones_.push_back(tone);
    if (tones_.size() > WIN_N) tones_.pop_front();
    if (apart > std::max(thr, LEAK * *std::max_element(tones_.begin(), tones_.end()))) return -1;
    return tone > thr ? 1 + kt + TONE_K : 0;
}

bool CwSense::feed(std::span<const double> x) {
    buf_.insert(buf_.end(), x.begin(), x.end());
    std::size_t at = 0;
    for (; at + WIN <= buf_.size(); at += WIN) {
        const int c = classify(std::span(buf_).subspan(at, WIN));
        hist_.push_back(c);
        if (hist_.size() > WIN_N) hist_.pop_front();
        // one tone: all hits but one within a step of each other
        int hits = 0, steady = 0;
        for (int v : hist_) hits += v > 0;
        for (int u = 1; u <= 2 * TONE_K + 1; ++u) {
            int s = 0;
            for (int v : hist_) s += v > 0 && std::abs(v - u) <= 1;
            steady = std::max(steady, s);
        }
        int runs = 0, run = 0, longest = 0;  // keyed: runs of hits
        for (int v : hist_) {
            run = v > 0 ? run + 1 : 0;
            runs += run == 1;
            longest = std::max(longest, run);
        }
        const auto n = [&](int v) { return std::count(hist_.begin(), hist_.end(), v); };
        active_ = hits >= MIN_HITS && steady >= hits - 1 && n(0) >= MIN_QUIET && n(-1) <= MAX_ELSEWHERE &&
                  runs >= MIN_RUNS && longest <= MAX_RUN;
        if (c > 0 && active_) {
            hits_.push_back(n_);
            if (hits_.size() > HITS_FROM) hits_.pop_front();
        }
        n_ += WIN;
    }
    buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(at));
    return active_;
}

}  // namespace data2g::cwid
