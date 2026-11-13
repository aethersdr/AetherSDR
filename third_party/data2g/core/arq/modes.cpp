#include "arq/modes.hpp"

#include <array>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>

#include "arq/frames.hpp"
#include "codes/codes.hpp"
#include "cpm/cpm.hpp"
#include "modem/timing.hpp"

namespace data2g::arq {

namespace {

// frames.BANDS_CODE: sync bands, 2 bits.
constexpr std::pair<std::string_view, int> BANDS_CODE[] = {{"w", 0}, {"n10", 1}, {"w48", 2}};

int band_code(std::string_view sync_band) {
    for (const auto& [b, c] : BANDS_CODE)
        if (b == sync_band) return c;
    throw std::out_of_range("no band code for " + std::string(sync_band));
}

}  // namespace

std::span<const Mode> modes() {
    static const std::vector<Mode> all = [] {
        std::vector<Mode> v;
        for (const auto& s : config::SUBMODES) v.push_back({s.name, s.band, &s, nullptr});
        for (const auto& s : tables::CPM_SPECS) v.push_back({s.name, s.grid, nullptr, &s});
        return v;
    }();
    return all;
}

const Mode* mode(std::string_view name) {
    for (const auto& m : modes())
        if (m.name == name) return &m;
    return nullptr;
}

const Mode& mode_at(std::string_view name) {
    const Mode* m = mode(name);
    if (!m) throw std::out_of_range("no mode " + std::string(name));
    return *m;
}

double burst_seconds(const Mode& m, int n_cw, bool dup) {
    return m.is_cpm() ? cpm::burst_seconds(*m.cpm, n_cw, dup) : modem::burst_seconds(*m.ofdm, n_cw);
}

int payload_bytes(const Mode& m) { return m.is_cpm() ? codes::payload_bytes(*m.cpm) : codes::payload_bytes(*m.ofdm); }

int ctl_payload_bytes(const Mode& m) {
    return m.is_cpm() ? codes::payload_bytes(cpm::ctl(cpm::grid_of(*m.cpm))) : codes::payload_bytes(*m.ofdm);
}

int max_ctl(const Mode& m) { return m.is_cpm() ? 1 : MAX_CTL; }

int min_cw(const Mode& m, bool data) { return data && m.is_cpm() ? 2 : 1; }

int rv_cycle(const Mode& m) { return m.is_cpm() ? codes::rv_cycle(*m.cpm) : codes::rv_cycle(*m.ofdm); }

int cap_hz(int cap) {
    switch (cap) {
        case 0: return 500;
        case 1: return 1200;
        case 2: return 2400;
    }
    throw std::out_of_range("no cap " + std::to_string(cap));
}

std::string bw_name(int cap) {
    static const std::array<const char*, 3> names = {"500", "1200", "2300"};
    return names.at(static_cast<std::size_t>(cap));
}

double width_hz(const Mode& m) {
    if (m.is_cpm()) {
        const auto& g = cpm::grid_of(*m.cpm);
        return g.m * g.rate + 2 * g.bp;
    }
    if (m.band == "n4") return 200;
    if (m.band == "n10") return 500;
    if (m.band == "w") return 1200;
    if (m.band == "w48") return 2400;
    throw std::out_of_range("no width for band " + std::string(m.band));
}

std::vector<const Mode*> allowed(int cap) {
    const int hz = cap_hz(cap);
    std::vector<const Mode*> out;
    for (const auto& m : modes())
        if (width_hz(m) <= hz) out.push_back(&m);
    return out;
}

// CPM_SPECS fits under N10_HIGH: modes.py asserts it, and the tables are generated from it

int encode(std::string_view submode) {
    const Mode& m = mode_at(submode);
    if (m.is_cpm()) return CPM_CODE << 4 | static_cast<int>(m.cpm - tables::CPM_SPECS.data());
    if (m.ofdm->sync_band == "n10" && m.ofdm->index >= 16) {
        if (m.ofdm->index >= 16 + 16 - N10_HIGH) throw std::out_of_range(std::string(submode) + ": no recommendation code");
        return CPM_CODE << 4 | (N10_HIGH + m.ofdm->index - 16);
    }
    return band_code(m.ofdm->sync_band) << 4 | m.ofdm->index;
}

const Mode* decode(int rec) {
    if (rec >> 4 == CPM_CODE) {
        const auto i = static_cast<std::size_t>(rec & 15);
        if (i >= static_cast<std::size_t>(N10_HIGH)) {
            for (const auto& s : config::SUBMODES)
                if (s.sync_band == "n10" && s.index == 16 + static_cast<int>(i) - N10_HIGH) return mode(s.name);
            return nullptr;
        }
        return i < tables::CPM_SPECS.size() ? mode(tables::CPM_SPECS[i].name) : nullptr;
    }
    for (const auto& [band, code] : BANDS_CODE)
        if (code == rec >> 4) {
            for (const auto& s : config::SUBMODES)
                if (s.sync_band == band && s.index == (rec & 15)) return mode(s.name);
            return nullptr;
        }
    return nullptr;
}

}  // namespace data2g::arq
