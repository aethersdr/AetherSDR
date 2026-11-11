// data2g/arq/modes.py: every mode the ARQ can use, the frozen OFDM ladder
// (config::SUBMODES) then the CPM data modes (tables::CPM_SPECS), with what
// differs by family.
#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "generated/config.hpp"
#include "tables/tables.hpp"

namespace data2g::arq {

struct Mode {
    std::string_view name, band;  // band: an OFDM band or a CPM grid
    const config::Submode* ofdm = nullptr;
    const tables::CpmSpec* cpm = nullptr;
    bool is_cpm() const { return cpm != nullptr; }
};

std::span<const Mode> modes();  // MODES' order
const Mode* mode(std::string_view name);  // nullptr: no such mode
const Mode& mode_at(std::string_view name);  // throws std::out_of_range

double burst_seconds(const Mode& m, int n_cw, bool dup = false);
int payload_bytes(const Mode& m);
int ctl_payload_bytes(const Mode& m);  // its control codeword's (CPM: the grid's short one)
int max_ctl(const Mode& m);
int min_cw(const Mode& m, bool data);
int rv_cycle(const Mode& m);

// Bandwidth: a cap code (a session's, KISS's) and what each mode occupies.
int cap_hz(int cap);  // CAP_HZ; throws for an unknown cap
std::string bw_name(int cap);  // BW_NAME, VARA's name for it (500, 1200, 2300); throws for an unknown cap
double width_hz(const Mode& m);
std::vector<const Mode*> allowed(int cap);  // within the cap, MODES' order

// The recommendation code (the core word's 6-bit recommend): sync band code
// (frames BANDS_CODE) << 4 | index; CPM modes take the spare band code.
inline constexpr int CPM_CODE = 3;
// CPM_CODE's indices from here carry n10's past 15 (16-23)
inline constexpr int N10_HIGH = 8;
int encode(std::string_view submode);  // throws for an unknown mode
const Mode* decode(int rec);           // nullptr: no such mode

}  // namespace data2g::arq
