// The CPM modes' grids, specs and parameters (data2g/cpm.py), compiled in:
// generated/cpm.cpp. Other areas read them through cpm::.
#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace data2g::tables {

// Grid row i owns CPM_CTL[i]. header_tones: 1024 rows of hdr_len, row v =
// (mode index + 2 x dup) << 6 | n_data (the header word before its CRC-6),
// as numpy's PCG64 drew them. tx: cpm.TX_FILTERS, by bandwidth cap code
// (0: 500 Hz, 1: 1200, 2: 2400).
struct CpmTxFilter {
    double bp, glide;
    int passes;
};

struct CpmGrid {
    std::string_view name;
    int m;
    double rate, center, bp, clip_db;
    int T, bits;
    // sync_threshold, mid_threshold: detect's floors (front, mid block); lock_llr1/2:
    // find's and late_lock's tone_llr score floors, wait_llr: a waiting front's; mid_llr: find_mid's.
    double f0, sync_threshold, mid_threshold, lock_llr1, lock_llr2, wait_llr, mid_llr;
    int hdr_len;
    std::span<const std::uint8_t> preamble, mid_block, header_tones;
    CpmTxFilter tx[3];
};

// A GF(M) data code's mother code (codes.gf_code, data2g.gfldpc.GFCode): n
// symbols, the codeword its first n0, rows of dmax slots (var -1: padding).
struct GfTable {
    int n = 0, n0 = 0, dmax = 0;
    std::span<const std::int16_t> var;
    std::span<const std::uint8_t> h;
};

struct CpmSpec {
    std::string_view name, grid, code;  // code: "gf" (data) | "polar" (control)
    int index, k, coded_bits, n_sym;
    std::span<const std::uint16_t> perm;  // interleaver (codes.interleaver; GF(M): of symbols)
    GfTable gf;  // code "gf" only
};

struct CpmParams {
    double preamble_s, block_s, spacing_s, hdr_s;
    int hdr_copies, max_data;
    double peak_ratio, ramp_s;
    int data_n, ctl_k, ctl_n;
    double hdr_margin1, hdr_margin2, llr_eps;  // cpm.HDR_MARGIN (one copy, both), LLR_EPS
    int refine_symbols;
};

extern const std::span<const CpmGrid> CPM_GRIDS;
extern const std::span<const CpmSpec> CPM_SPECS, CPM_CTL;
extern const CpmParams CPM;

}  // namespace data2g::tables
