// data2g/codes.py: the codec over ldpc:: and polar:: (which know nothing of
// submodes): CRC framing, the PN9 scrambler, interleavers, burst order
// (spread/despread), encode, IR combining and the batched decodes.
//
// Keyed by codes::Spec (Python's codes.CodeSpec): a submode, a CPM mode or a
// CPM control codeword, with its interleaver and info set from the generated
// tables. Callers: modem (encode, spread, decode_many), arq::phy (IR,
// decode_raw + check, decode_posterior), arq::modes (the rules on raw rows).
// Parity with Python: tests/test_native_codes.py through native.codes
// (bindings/bind_codes.cpp).
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "config/config.hpp"
#include "gfldpc/gfldpc.hpp"
#include "ldpc/ldpc.hpp"
#include "polar/polar.hpp"
#include "tables/cpm.hpp"
#include "util/mat.hpp"

namespace data2g::codes {

inline constexpr int PLAIN = -1;  // burst position meaning "not scrambled"

std::uint16_t crc16(std::span<const std::uint8_t> data);  // CRC-16/CCITT-FALSE (binascii.crc_hqx, 0xFFFF)
std::uint32_t crc24(std::span<const std::uint8_t> data, std::uint32_t crc = 0xFFFFFF);  // CRC-24C
std::uint32_t crc32(std::span<const std::uint8_t> data);  // zlib's

// payload + its n_crc-bit CRC (16, 24 or 32) XORed with mask, big-endian.
std::vector<std::uint8_t> with_crc(std::span<const std::uint8_t> payload, int n_crc, std::uint32_t mask = 0);

int scramble_seed(int index);
std::vector<std::uint8_t> scrambler(int k, int seed = 0x1FF);  // PN9, k bits

// The codec's rules (codes.crc_bits, payload_bytes, rv_cycle) on a raw
// table row with .code and .k (config::Submode, tables::CpmSpec): Spec is
// built with them, and callers holding a row (arq::modes) ask them here.
// Inline on the row: the ARQ asks per decision.
template <typename Row>
int crc_bits(const Row& r) {
    if (r.code == "polar") return 24;
    return r.k >= 512 ? 32 : 16;
}

template <typename Row>
int payload_bytes(const Row& r) {
    return (r.k - crc_bits(r)) / 8;
}

template <typename Row>
int rv_cycle(const Row& r) {
    return r.code == "polar" ? 1 : 4;
}

// --- the codec (codes.encode / decode / combine) ------------------------------
//
// Batches are Mat rows. Per-row arguments (masks: CRC masks, index: burst
// positions, rvs) broadcast like numpy: empty means the default (mask 0,
// index = row number), one value means every row, else one per row.
// Everything is reentrant: one call never shares mutable state with another.

// A coded format: a submode, a CPM mode or a CPM control codeword
// (Python's SubmodeSpec / CpmSpec where data2g.codes reads them).
// GF(M) (gf set: the CPM modes' data, gfldpc::): soft values are q = M
// log-likelihoods per code symbol, symbol-major (soft_len a codeword); the
// interleaver (perm) and RVs move whole symbols; scrambling permutes each
// symbol's likelihoods (plain) where it flips a bit's sign.
struct Spec {
    std::string_view name;
    bool polar;  // else LDPC, binary or (gf) GF(M)
    int k, coded_bits, bits_per_cu, crc_bits, payload_bytes;
    std::span<const std::uint16_t> perm, info_pos;  // interleaver; polar info set
    const tables::GfTable* gf = nullptr;  // GF(M): its mother code
};

inline int n_sym(const Spec& s) { return s.coded_bits / s.bits_per_cu; }  // GF(M): code symbols a codeword
// Soft values a codeword: coded bits, or GF(M) q per symbol (codes.soft_len).
inline int soft_len(const Spec& s) { return s.gf ? n_sym(s) << s.bits_per_cu : s.coded_bits; }

const Spec* spec(std::string_view name);  // nullptr if none
const Spec& spec(const config::Submode& s);  // s an element of config::SUBMODES
const Spec& spec(const tables::CpmSpec& s);  // s an element of tables::CPM_SPECS or CPM_CTL

const ldpc::Code& ldpc_code(const Spec& s);  // LDPC specs
const polar::PolarCode& polar_code(const Spec& s);  // polar specs, on their frozen info set
// codes._decoder (extent 0) or codes._ext_decoder(spec, extent).
const ldpc::Decoder& ldpc_decoder(const Spec& s, int extent = 0);
const polar::SCLDecoder& polar_decoder(const Spec& s);  // polar specs, list POLAR_LIST
const gfldpc::Code& gf_code(const Spec& s);  // GF(M) specs: the mother code
// GF(M): the decoder of the mother code cut to `extent` soft values (0: the codeword).
const gfldpc::Decoder& gf_decoder(const Spec& s, int extent = 0);
inline constexpr int GF_ITERS = 50;  // codes.GF_ITERS

inline constexpr int POLAR_LIST = 8;
// The LDPC decoder's iteration cap, process-wide: the app's "decoder effort" (--decoder-effort). The
// layered sum-product decoder at 20 matched the old flooding one at 40; 25 is about 0.1 dB better.
inline constexpr int DEFAULT_ITERS = 25;
int ldpc_iters();
void set_ldpc_iters(int n);  // n >= 1; takes effect from the next decode

int rv_cycle(const Spec& s);
int buffer_len(const Spec& s);
std::vector<int> rv_positions(const Spec& s, int rv);
// Buffer positions an LDPC decode reads with RVs up to max_rv received: the
// mother code is cut there (unreceived parity would only slow it).
int ir_extent(const Spec& s, int max_rv);

// payload (exactly payload_bytes) -> k info bits: payload, masked CRC, zero fill, scrambled.
std::vector<std::uint8_t> info_bits(const Spec& s, std::span<const std::uint8_t> payload, std::uint32_t crc_mask = 0,
                                    int index = 0);
// (B, k) info bits -> (B, coded_bits), interleaved; rv > 0 on LDPC: mother-code parity.
Mat<std::uint8_t> encode_info(const Spec& s, const Mat<std::uint8_t>& bits, int rv = 0);
std::vector<std::uint8_t> encode(const Spec& s, std::span<const std::uint8_t> payload, int rv = 0,
                                 std::uint32_t crc_mask = 0, int index = 0);
// +-1 per coded bit: soft bits sent at (index, rv) times this are the unscrambled codeword's.
std::vector<double> flip(const Spec& s, int index, int rv = 0);
// soft_len soft values sent at (index, rv) -> the unscrambled codeword's: flip's signs, or GF(M) each
// symbol's likelihoods permuted (codes.plain).
std::vector<double> plain(const Spec& s, std::span<const double> soft, int index, int rv = 0);

// (n_cw, n) row-major -> burst order, every codeword's m-bit symbols dealt round-robin.
template <typename T>
std::vector<T> spread(std::span<const T> coded, int n_cw, int m) {
    const std::size_t n = coded.size() / n_cw, syms = n / m;
    std::vector<T> out(coded.size());
    for (int c = 0; c < n_cw; ++c)
        for (std::size_t j = 0; j < syms; ++j)
            for (int b = 0; b < m; ++b) out[(j * n_cw + c) * m + b] = coded[c * n + j * m + b];
    return out;
}

template <typename T>
std::vector<T> despread(std::span<const T> x, int n_cw, int m) {
    const std::size_t n = x.size() / n_cw, syms = n / m;
    std::vector<T> out(x.size());
    for (int c = 0; c < n_cw; ++c)
        for (std::size_t j = 0; j < syms; ++j)
            for (int b = 0; b < m; ++b) out[c * n + j * m + b] = x[(j * n_cw + c) * m + b];
    return out;
}

// Adds (B, coded_bits) soft bits in mapping order, sent at RVs `rvs`, into
// the (B, buffer_len) buffer; an empty buf (rows 0) starts as zeros.
void combine(const Spec& s, Mat<double>& buf, const Mat<double>& soft, std::span<const int> rvs);

struct Payload {
    std::vector<std::uint8_t> data;
    bool ok;  // CRC matches and the decoder converged
};

// Decoded (scrambled) info bits -> payloads (codes.payloads).
std::vector<Payload> payloads(const Spec& s, const Mat<std::uint8_t>& bits, std::span<const std::uint8_t> converged,
                              std::span<const std::uint32_t> masks = {}, std::span<const int> index = {});

struct Info {
    Mat<std::uint8_t> bits;     // (B, k), still scrambled
    std::vector<std::uint8_t> ok;  // LDPC: H satisfied; polar: a list path's CRC checked
};

// (B, coded_bits) LLRs in mapping order, decoded as one batch (LDPC: the
// batch stops together, so a row's result depends on its batch mates).
Info decode_llrs(const Spec& s, const Mat<float>& llr, int iters = 0,  // 0: ldpc_iters()
                 std::span<const std::uint32_t> masks = {},
                 std::span<const int> index = {});
std::vector<Payload> decode_many(const Spec& s, const Mat<float>& soft, std::span<const std::uint32_t> masks = {},
                                 std::span<const int> index = {});
// (B, buffer_len) combined buffers, RVs up to max_rv received.
std::vector<Payload> decode_buffer(const Spec& s, const Mat<double>& buf, int max_rv = 0,
                                   std::span<const std::uint32_t> masks = {}, std::span<const int> index = {});

struct Posterior {
    std::vector<std::uint8_t> bits;  // (k,) info bits as decoded (still scrambled unless from a buffer)
    bool ok;                         // converged
    std::vector<double> post;        // a-posteriori LLRs of the slot's coded bits, mapping order
};
// One LDPC decode as decode_raw (buf empty: a slot's coded_bits soft bits
// alone) or decode_buffer (buf: one combined buffer row, RVs up to max_rv,
// the slot sent at rv) would, with the posterior DD (arq::ModemRx) needs
// from the same pass.
Posterior decode_posterior(const Spec& s, std::span<const double> soft, std::span<const double> buf = {},
                           int max_rv = 0, int rv = 0);

struct Raw {
    int list;                  // candidates per row: 1 (LDPC) or POLAR_LIST
    Mat<std::uint8_t> cands;   // (B, list * k), descrambled, best first
    Mat<std::uint8_t> usable;  // (B, list)
};
// Decode once with the CRC mask open; then check() each mask in question.
Raw decode_raw(const Spec& s, const Mat<float>& soft, std::span<const int> index = {});
// One row of decode_raw -> the first usable candidate's payload whose CRC passes, or empty.
std::optional<std::vector<std::uint8_t>> check(const Spec& s, std::span<const std::uint8_t> cands,
                                               std::span<const std::uint8_t> usable, std::uint32_t crc_mask);

// Known info bits (codes.py's "source-aware decoding"): a sent info bit the
// receiver already knows is pinned at the decoder's input. Where each info
// bit is sent at RV 0, in mapping order, -1 if it isn't (LDPC's punctured
// first 2Z columns and filler; polar: every one).
const std::vector<int>& systematic_positions(const Spec& s);
// (k,) known info bits, -1 unknown: payload bytes from nbytes on are zero, and the zero fill past the CRC.
std::vector<std::int8_t> zero_tail(const Spec& s, int nbytes);
// (k,) known info bits, -1 unknown: the payload bits `mask` sets are `tmpl`'s (MSB first; each zero-extended
// to payload_bytes), and the zero fill past the CRC; never the CRC (codes.known_bits, docs/arq.md §3a).
std::vector<std::int8_t> known_bits(const Spec& s, std::span<const std::uint8_t> tmpl, std::span<const std::uint8_t> mask);
// Log-likelihood ratio (nats) of payload's RV 0 codeword sent at `index` against random bits, from its soft
// bits in mapping order (codes.score).
double score(const Spec& s, std::span<const double> soft, std::span<const std::uint8_t> payload, std::uint32_t crc_mask,
             int index);
// An RV 0 codeword's soft bits (sent at burst position `index`) with every
// sent info bit `known` gives (0/1, -1 unknown; unscrambled) pinned to
// ldpc::CH_CLAMP. Unchanged for polar.
std::vector<float> pin(const Spec& s, std::span<const double> soft, std::span<const std::int8_t> known, int index);
// pin() for one combined IR buffer row (code order, unscrambled: decode_buffer's input).
std::vector<double> pin_buffer(const Spec& s, std::span<const double> buf, std::span<const std::int8_t> known);
// (k,) the channel's LLR of each info bit as unscrambled, 0 where not sent:
// an RV 0 codeword's soft bits (mapping order) sent at burst position `index`
// (codes.info_llrs), or one combined IR buffer row (codes.info_llrs_buffer).
std::vector<double> info_llrs(const Spec& s, std::span<const double> soft, int index);
std::vector<double> info_llrs_buffer(const Spec& s, std::span<const double> buf);
// A control's padding estimated from info_llrs -> (k,) prior LLRs toward 0
// per info bit, +inf where known zero (codes.pad_prior).
std::vector<double> pad_prior(const Spec& s, std::span<const double> llr);
// An RV 0 codeword's soft bits (sent at `index`) with each sent info bit's
// prior added, set to the clamp where it is inf (codes.add_prior); one
// combined IR buffer row the same (codes.add_prior_buffer).
std::vector<float> add_prior(const Spec& s, std::span<const double> soft, std::span<const double> prior, int index);
std::vector<double> add_prior_buffer(const Spec& s, std::span<const double> buf, std::span<const double> prior);
// The order info bits enter a polar code (codes.polar_order): the payload's bits reversed, then CRC and fill.
const std::vector<int>& polar_order(const Spec& s);
// decode_raw with the info bits `known` gives known (0/1, -1 unknown; unscrambled), every row sent at burst
// position `index`: LDPC pinned (pin), polar frozen in a code of the rest (codes.decode_raw_pinned).
Raw decode_raw_pinned(const Spec& s, const Mat<float>& soft, std::span<const std::int8_t> known, int index);
// decode_raw of (B, buffer_len) combined buffers, RVs up to max_rv received: candidates already plain (the
// buffer is unscrambled); `known` (empty: none) as decode_buffer_pinned's (codes.decode_buffer_raw).
Raw decode_buffer_raw(const Spec& s, const Mat<double>& buf, int max_rv = 0, std::span<const std::int8_t> known = {});
// decode_buffer (index PLAIN) with the info bits `known` gives known (codes.decode_buffer_pinned).
std::vector<Payload> decode_buffer_pinned(const Spec& s, const Mat<double>& buf, int max_rv,
                                          std::span<const std::int8_t> known, std::uint32_t crc_mask = 0);

std::vector<std::uint8_t> descramble(const Spec& s, std::span<const std::uint8_t> bits, int index);
// (B, >= payload + CRC bits) scrambled info bits -> (B,) CRC matches.
std::vector<std::uint8_t> crc_ok(const Spec& s, const Mat<std::uint8_t>& bits,
                                 std::span<const std::uint32_t> masks = {}, std::span<const int> index = {});

}  // namespace data2g::codes
