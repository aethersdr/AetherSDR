#include "modem/timing.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <cstdint>

namespace data2g::modem {

using namespace config;

bool hosts(std::string_view name) {
    return std::any_of(BANDS.begin(), BANDS.end(), [&](const Band& b) { return b.name != name && b.sync_band == name; });
}

std::vector<bool> header_layout(std::string_view name) {
    std::vector<bool> out;
    for (int i = 0; i < band(name).header_syms; ++i) {
        if (i && i % DATA_SYMS_PER_FRAME == 0) out.push_back(true);
        out.push_back(false);
    }
    if (hosts(name)) out.push_back(true);
    return out;
}

int header_samples(std::string_view name) { return static_cast<int>(header_layout(name).size()) * NSYM; }

bool copies(std::string_view name) {
    return std::find(HEADER_COPY_BANDS.begin(), HEADER_COPY_BANDS.end(), name) != HEADER_COPY_BANDS.end();
}

std::optional<int> copy_frame(std::string_view name, int n_f) {
    if (!copies(name)) return std::nullopt;
    return std::min(HEADER_COPY_AFTER, n_f);
}

int frames_on_air(const Submode& spec, int n_cw) {
    const int n_f = n_cw * spec.frames_per_cw;
    return n_f + copy_frame(spec.sync_band, n_f).has_value();
}

std::int64_t burst_end(std::int64_t p0, const Submode& spec, int n_cw) {
    return p0 + static_cast<std::int64_t>(frames_on_air(spec, n_cw) * SYMS_PER_FRAME + 1) * NSYM;
}

int head_samples(std::string_view name) {
    const int n = preamble_samples(band(name)) + header_samples(name) + NSYM;
    return copies(name) ? n + (HEADER_COPY_AFTER + 1) * FRAME_SAMPLES : n;
}

// head_samples, plus room for best_header's reads up to two repeats late (2 M) and a symbol and a repeat to spare
int receive_head(std::string_view name) { return head_samples(name) + NSYM + 3 * M; }

double burst_seconds(const Submode& spec, int n_cw) {
    const std::int64_t n = LEADIN_SAMPLES + preamble_samples(band(spec.sync_band)) + header_samples(spec.sync_band) +
                   static_cast<std::int64_t>(frames_on_air(spec, n_cw) * SYMS_PER_FRAME + 1) * NSYM + LEADOUT_SAMPLES;
    return static_cast<double>(n) / FS;
}

}  // namespace data2g::modem
