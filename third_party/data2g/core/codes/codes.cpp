#include "codes/codes.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>

#include "tables/cpm.hpp"
#include "tables/format.hpp"

namespace data2g::codes {

namespace {

// MSB-first CRC table, as codes._crc24_table builds it.
template <std::uint32_t Poly, int Bits>
constexpr std::array<std::uint32_t, 256> msb_table() {
    constexpr std::uint32_t top = 1u << (Bits - 1), mask = (1u << Bits) - 1;
    std::array<std::uint32_t, 256> t{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t c = i << (Bits - 8);
        for (int j = 0; j < 8; ++j) c = (c & top) ? ((c << 1) ^ Poly) & mask : (c << 1) & mask;
        t[i] = c;
    }
    return t;
}

constexpr auto CRC16 = msb_table<0x1021, 16>();
constexpr auto CRC24 = msb_table<0xB2B117, 24>();

constexpr std::array<std::uint32_t, 256> crc32_table() {
    std::array<std::uint32_t, 256> t{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t c = i;
        for (int j = 0; j < 8; ++j) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        t[i] = c;
    }
    return t;
}
constexpr auto CRC32 = crc32_table();

}  // namespace

std::uint16_t crc16(std::span<const std::uint8_t> data) {
    std::uint32_t crc = 0xFFFF;
    for (auto b : data) crc = ((crc << 8) & 0xFFFF) ^ CRC16[((crc >> 8) ^ b) & 0xFF];
    return static_cast<std::uint16_t>(crc);
}

std::uint32_t crc24(std::span<const std::uint8_t> data, std::uint32_t crc) {
    for (auto b : data) crc = ((crc << 8) & 0xFFFFFF) ^ CRC24[((crc >> 16) ^ b) & 0xFF];
    return crc;
}

std::uint32_t crc32(std::span<const std::uint8_t> data) {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (auto b : data) crc = CRC32[(crc ^ b) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

std::vector<std::uint8_t> with_crc(std::span<const std::uint8_t> payload, int n_crc, std::uint32_t mask) {
    std::uint32_t c;
    if (n_crc == 16) c = (crc16(payload) ^ mask) & 0xFFFF;
    else if (n_crc == 24) c = (crc24(payload) ^ mask) & 0xFFFFFF;
    else if (n_crc == 32) c = crc32(payload) ^ mask;
    else throw std::invalid_argument("n_crc must be 16, 24 or 32");
    std::vector<std::uint8_t> out(payload.begin(), payload.end());
    for (int shift = n_crc - 8; shift >= 0; shift -= 8) out.push_back(static_cast<std::uint8_t>(c >> shift));
    return out;
}

int scramble_seed(int index) {
    if (index == PLAIN) return 0;
    if (index < 0) throw std::invalid_argument("burst position must be >= 0 or PLAIN");
    return 1 + static_cast<int>((static_cast<std::uint64_t>(index) * 0x9E3779B1u) % 511);
}

std::vector<std::uint8_t> scrambler(int k, int seed) {
    std::vector<std::uint8_t> out(static_cast<std::size_t>(k));
    unsigned reg = static_cast<unsigned>(seed);
    for (auto& o : out) {
        const unsigned bit = ((reg >> 8) ^ (reg >> 4)) & 1;
        o = static_cast<std::uint8_t>(bit);
        reg = ((reg << 1) | bit) & 0x1FF;
    }
    return out;
}

// --- the codec ---------------------------------------------------------------

namespace {

std::vector<Spec> build_specs() {
    std::vector<Spec> v;
    for (std::size_t i = 0; i < config::SUBMODES.size(); ++i) {
        const auto& s = config::SUBMODES[i];
        v.push_back({s.name, s.code == "polar", s.k, s.coded_bits, s.bits_per_cu, s.crc_bits, s.payload_bytes,
                     tables::FORMATS[i].perm, tables::FORMATS[i].info_pos});
    }
    for (auto table : {tables::CPM_SPECS, tables::CPM_CTL})
        for (const auto& s : table) {
            const bool polar = s.code == "polar";
            const int crc = crc_bits(s);
            int bits = 0;
            for (const auto& g : tables::CPM_GRIDS)
                if (g.name == s.grid) bits = g.bits;
            v.push_back({s.name, polar, s.k, s.coded_bits, bits, crc, payload_bytes(s), s.perm,
                         polar ? polar::ga_info_pos(s.k, s.coded_bits) : std::span<const std::uint16_t>{},
                         s.code == "gf" ? &s.gf : nullptr});
        }
    return v;
}

const std::vector<Spec>& specs() {
    static const std::vector<Spec> v = build_specs();
    return v;
}

// Codes and decoders per spec, built on first use. Entries are never
// replaced, so references stay valid; the lock covers construction only.
struct Codecs {
    std::optional<ldpc::Code> code, mother;
    std::optional<gfldpc::Code> gf;
    std::map<int, std::unique_ptr<gfldpc::Decoder>> gf_dec;  // by extent, 0 = the codeword
    std::optional<std::vector<int>> soft_perm;
    std::optional<polar::PolarCode> polar;
    std::map<int, std::unique_ptr<ldpc::Decoder>> ldpc;  // by extent, 0 = the code itself
    std::unique_ptr<polar::SCLDecoder> scl;
    std::optional<std::vector<int>> systematic, info_code, order;  // systematic_positions, info_code_positions, polar_order
};
std::mutex cache_mutex;
std::map<const Spec*, Codecs> cache;

const ldpc::Code& mother_code(const Spec& s) {
    const auto& code = ldpc_code(s);
    std::lock_guard lock(cache_mutex);
    auto& c = cache[&s];
    if (!c.mother) c.mother = code.mother();
    return *c.mother;
}

// numpy's broadcast of a scalar-or-per-row argument.
template <typename T>
T at(std::span<const T> v, std::size_t row, std::size_t rows, T dflt) {
    if (v.empty()) return dflt;
    if (v.size() == 1) return v[0];
    if (v.size() != rows) throw std::invalid_argument("per-row argument: expected 1 or one per row");
    return v[row];
}

std::uint32_t mask_at(std::span<const std::uint32_t> m, std::size_t b, std::size_t rows) { return at(m, b, rows, 0u); }
int index_at(std::span<const int> i, std::size_t b, std::size_t rows) { return at(i, b, rows, static_cast<int>(b)); }

int crc_bytes(const Spec& s) { return s.crc_bits / 8; }
int framed_bytes(const Spec& s) { return s.payload_bytes + crc_bytes(s); }

// The first framed_bytes of info bits, XORed with pn (if given), packed MSB first.
std::vector<std::uint8_t> pack(const Spec& s, const std::uint8_t* bits, const std::uint8_t* pn) {
    std::vector<std::uint8_t> out(static_cast<std::size_t>(framed_bytes(s)), 0);
    for (std::size_t i = 0; i < out.size() * 8; ++i)
        out[i / 8] |= static_cast<std::uint8_t>(((bits[i] ^ (pn ? pn[i] : 0)) & 1) << (7 - i % 8));
    return out;
}

// pack()'s bytes are payload + CRC: the payload if the CRC matches under mask.
bool crc_matches(const Spec& s, const std::vector<std::uint8_t>& data, std::uint32_t mask) {
    const auto payload = std::span(data).first(static_cast<std::size_t>(s.payload_bytes));
    return with_crc(payload, s.crc_bits, mask) == data;
}

std::vector<std::uint8_t> pn_at(const Spec& s, int index) { return scrambler(s.k, scramble_seed(index)); }

// x[:, :cols] as float32: a buffer is in code order already
template <typename T>
Mat<float> cut(const Spec& s, const Mat<T>& x, std::size_t cols) {
    if (x.cols < cols) throw std::invalid_argument(std::string(s.name) + ": too few soft values per row");
    Mat<float> out(x.rows, cols);
    for (std::size_t b = 0; b < x.rows; ++b)
        for (std::size_t i = 0; i < cols; ++i) out[b][i] = static_cast<float>(x[b][i]);
    return out;
}

// The interleaver over soft values (codes._soft_perm): GF(M)'s symbol permutation expanded to q values each.
const std::vector<int>& soft_perm(const Spec& s) {
    std::lock_guard lock(cache_mutex);
    auto& c = cache[&s];
    if (!c.soft_perm) {
        std::vector<int> p;
        const int q = s.gf ? 1 << s.bits_per_cu : 1;
        for (auto v : s.perm)
            for (int a = 0; a < q; ++a) p.push_back(v * q + a);
        c.soft_perm = std::move(p);
    }
    return *c.soft_perm;
}

Mat<float> deinterleave_llr(const Spec& s, const Mat<float>& llr) {
    if (static_cast<int>(llr.cols) != soft_len(s))
        throw std::invalid_argument(std::string(s.name) + ": expected (B, soft_len)");
    const auto& perm = soft_perm(s);
    Mat<float> out(llr.rows, llr.cols);
    for (std::size_t b = 0; b < llr.rows; ++b)
        for (std::size_t i = 0; i < llr.cols; ++i) out[b][perm[i]] = llr[b][i];
    return out;
}

// GF(M): (B, extent) likelihoods in code order -> info bits (B, k) and convergence (codes._decode_code_order).
Info decode_gf(const Spec& s, const gfldpc::Decoder& dec, const Mat<float>& deint) {
    const auto d = dec.decode(deint, GF_ITERS);
    const auto& info = dec.code().info();
    Mat<std::uint8_t> u(d.hard.rows, info.size());
    for (std::size_t b = 0; b < u.rows; ++b)
        for (std::size_t j = 0; j < info.size(); ++j) u[b][j] = d.hard[b][info[j]];
    const auto bits = gfldpc::unpack(u, s.bits_per_cu);
    Info out{Mat<std::uint8_t>(u.rows, static_cast<std::size_t>(s.k)), d.ok};
    for (std::size_t b = 0; b < u.rows; ++b) std::copy_n(bits[b], s.k, out.bits[b]);
    return out;
}

// (B, L * k) polar paths in the code's order -> the info bits' (polar_order is its own inverse).
void reorder_paths(const Spec& s, Mat<std::uint8_t>& paths) {
    const auto& order = polar_order(s);
    const std::size_t k = order.size();
    std::vector<std::uint8_t> tmp(k);
    for (std::size_t r = 0; r < paths.rows; ++r)
        for (std::size_t off = 0; off + k <= paths.cols; off += k) {
            std::uint8_t* p = paths[r] + off;
            for (std::size_t j = 0; j < k; ++j) tmp[j] = p[order[j]];
            std::copy(tmp.begin(), tmp.end(), p);
        }
}

// codes._decode_code_order: LLRs in the decoder's code order.
Info decode_code_order(const Spec& s, const ldpc::Decoder* dec, const Mat<float>& deint, int iters,
                       std::span<const std::uint32_t> masks, std::span<const int> index) {
    if (dec) {
        auto r = dec->decode(deint, iters);
        return {std::move(r.bits), std::move(r.ok)};
    }
    const auto& scl = polar_decoder(s);
    auto r = scl.decode(deint);
    reorder_paths(s, r.paths);
    const std::size_t B = deint.rows, L = static_cast<std::size_t>(scl.list_size()), k = static_cast<std::size_t>(s.k);
    Info out{Mat<std::uint8_t>(B, k), std::vector<std::uint8_t>(B, 0)};
    for (std::size_t b = 0; b < B; ++b) {
        const auto pn = pn_at(s, index_at(index, b, B));
        const std::uint32_t m = mask_at(masks, b, B);
        std::size_t pick = 0;
        for (std::size_t l = 0; l < L; ++l)
            if (crc_matches(s, pack(s, r.paths[b] + l * k, pn.data()), m)) {
                pick = l;
                out.ok[b] = 1;
                break;
            }
        std::copy_n(r.paths[b] + pick * k, k, out.bits[b]);
    }
    return out;
}

}  // namespace

const Spec* spec(std::string_view name) {
    for (const auto& s : specs())
        if (s.name == name) return &s;
    return nullptr;
}

const Spec& spec(const config::Submode& s) { return specs().at(config::row(s)); }

const Spec& spec(const tables::CpmSpec& s) {
    // specs() is SUBMODES, then CPM_SPECS, then CPM_CTL
    const std::less<const tables::CpmSpec*> lt;
    std::size_t at = config::SUBMODES.size();
    for (auto table : {tables::CPM_SPECS, tables::CPM_CTL}) {
        if (!lt(&s, table.data()) && lt(&s, table.data() + table.size()))
            return specs()[at + static_cast<std::size_t>(&s - table.data())];
        at += table.size();
    }
    throw std::invalid_argument(std::string(s.name) + ": not a tables::CPM_SPECS or CPM_CTL row");
}

const gfldpc::Code& gf_code(const Spec& s) {
    if (!s.gf) throw std::invalid_argument(std::string(s.name) + " is not GF(M)");
    std::lock_guard lock(cache_mutex);
    auto& c = cache[&s];
    if (!c.gf) c.gf.emplace(s.bits_per_cu, s.gf->n, s.gf->n0, s.gf->dmax, s.gf->var, s.gf->h);
    return *c.gf;
}

const gfldpc::Decoder& gf_decoder(const Spec& s, int extent) {
    const auto& code = gf_code(s);
    std::lock_guard lock(cache_mutex);
    auto& d = cache[&s].gf_dec[extent];
    if (!d) d = std::make_unique<gfldpc::Decoder>(code.cut(extent ? extent >> s.bits_per_cu : n_sym(s)));
    return *d;
}

const ldpc::Code& ldpc_code(const Spec& s) {
    if (s.polar || s.gf) throw std::invalid_argument(std::string(s.name) + " is not LDPC");
    std::lock_guard lock(cache_mutex);
    auto& c = cache[&s];
    if (!c.code) c.code = ldpc::qc_code(s.k, s.coded_bits);
    return *c.code;
}

const ldpc::Decoder& ldpc_decoder(const Spec& s, int extent) {
    const auto& code = ldpc_code(s);
    std::lock_guard lock(cache_mutex);
    auto& d = cache[&s].ldpc[extent];
    if (!d) d = std::make_unique<ldpc::Decoder>(extent ? code.mother(extent) : code);
    return *d;
}

namespace {

// s's cache entry with its polar code built; cache_mutex held.
Codecs& polar_entry(const Spec& s) {
    if (!s.polar) throw std::invalid_argument(std::string(s.name) + " is not polar");
    auto& c = cache[&s];
    if (!c.polar) c.polar.emplace(s.k, s.coded_bits, s.info_pos);
    return c;
}

}  // namespace

const polar::PolarCode& polar_code(const Spec& s) {
    std::lock_guard lock(cache_mutex);
    return *polar_entry(s).polar;
}

const polar::SCLDecoder& polar_decoder(const Spec& s) {
    std::lock_guard lock(cache_mutex);  // once: the decoder is fetched per decode
    auto& c = polar_entry(s);
    if (!c.scl) c.scl = std::make_unique<polar::SCLDecoder>(*c.polar, POLAR_LIST);
    return *c.scl;
}

int rv_cycle(const Spec& s) { return s.polar ? 1 : 4; }

int buffer_len(const Spec& s) {
    if (s.gf) return s.gf->n << s.bits_per_cu;
    return s.polar ? s.coded_bits : mother_code(s).n;
}

namespace {

// GF(M): the mother code's symbols RV rv carries (codes._rv_symbols).
std::vector<int> rv_symbols(const Spec& s, int rv) {
    const int n = n_sym(s), c = rv_cycle(s);
    std::vector<int> out(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) out[i] = (((rv % c) + c) % c * n + i) % s.gf->n;
    return out;
}

}  // namespace

std::vector<int> rv_positions(const Spec& s, int rv) {
    if (s.gf) {
        const int q = 1 << s.bits_per_cu;
        std::vector<int> out;
        for (int v : rv_symbols(s, rv))
            for (int a = 0; a < q; ++a) out.push_back(v * q + a);
        return out;
    }
    const int n = s.coded_bits, c = rv_cycle(s), L = buffer_len(s);
    std::vector<int> out(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) out[i] = s.polar ? i : (((rv % c) + c) % c * n + i) % L;
    return out;
}

int ir_extent(const Spec& s, int max_rv) {
    return std::min(buffer_len(s), (std::min(max_rv, rv_cycle(s) - 1) + 1) * soft_len(s));
}

std::vector<std::uint8_t> info_bits(const Spec& s, std::span<const std::uint8_t> payload, std::uint32_t crc_mask,
                                    int index) {
    if (static_cast<int>(payload.size()) != s.payload_bytes)
        throw std::invalid_argument(std::string(s.name) + " carries " + std::to_string(s.payload_bytes) + " bytes, got " +
                                    std::to_string(payload.size()));
    const auto framed = with_crc(payload, s.crc_bits, crc_mask);
    auto bits = pn_at(s, index);
    for (std::size_t i = 0; i < framed.size() * 8; ++i) bits[i] ^= (framed[i / 8] >> (7 - i % 8)) & 1;
    return bits;
}

Mat<std::uint8_t> encode_info(const Spec& s, const Mat<std::uint8_t>& bits, int rv) {
    if (s.gf) {
        const auto& c = gf_code(s);
        const auto x = c.encode(gfldpc::pack(bits, s.bits_per_cu, c.k()));
        const auto pos = rv_symbols(s, rv);
        Mat<std::uint8_t> sent(x.rows, pos.size());
        for (std::size_t b = 0; b < x.rows; ++b)
            for (std::size_t i = 0; i < pos.size(); ++i) sent[b][i] = x[b][pos[s.perm[i]]];
        return gfldpc::unpack(sent, s.bits_per_cu);
    }
    Mat<std::uint8_t> coded;
    if (rv && !s.polar) {
        const auto full = mother_code(s).encode(bits);
        const auto pos = rv_positions(s, rv);
        coded = Mat<std::uint8_t>(bits.rows, pos.size());
        for (std::size_t b = 0; b < bits.rows; ++b)
            for (std::size_t j = 0; j < pos.size(); ++j) coded[b][j] = full[b][pos[j]];
    } else if (s.polar) {
        const auto& order = polar_order(s);
        Mat<std::uint8_t> in(bits.rows, bits.cols);
        for (std::size_t b = 0; b < bits.rows; ++b)
            for (std::size_t j = 0; j < order.size(); ++j) in[b][j] = bits[b][static_cast<std::size_t>(order[j])];
        coded = polar_code(s).encode(in);
    } else {
        coded = ldpc_code(s).encode(bits);
    }
    Mat<std::uint8_t> out(coded.rows, coded.cols);
    for (std::size_t b = 0; b < coded.rows; ++b)
        for (std::size_t i = 0; i < coded.cols; ++i) out[b][i] = coded[b][s.perm[i]];
    return out;
}

std::vector<std::uint8_t> encode(const Spec& s, std::span<const std::uint8_t> payload, int rv, std::uint32_t crc_mask,
                                 int index) {
    Mat<std::uint8_t> info(1, static_cast<std::size_t>(s.k));
    info.data = info_bits(s, payload, crc_mask, index);
    return encode_info(s, info, rv).data;
}

std::vector<double> flip(const Spec& s, int index, int rv) {
    Mat<std::uint8_t> info(1, static_cast<std::size_t>(s.k));
    info.data = pn_at(s, index);
    const auto coded = encode_info(s, info, rv);
    std::vector<double> out(coded.data.size());
    for (std::size_t i = 0; i < out.size(); ++i) out[i] = 1.0 - 2.0 * coded.data[i];
    return out;
}

std::vector<double> plain(const Spec& s, std::span<const double> soft, int index, int rv) {
    if (static_cast<int>(soft.size()) != soft_len(s))
        throw std::invalid_argument(std::string(s.name) + ": expected soft_len values");
    std::vector<double> out(soft.size());
    if (!s.gf) {
        const auto fl = flip(s, index, rv);
        for (std::size_t i = 0; i < soft.size(); ++i) out[i] = fl[i] * soft[i];
        return out;
    }
    Mat<std::uint8_t> info(1, static_cast<std::size_t>(s.k));
    info.data = pn_at(s, index);
    const auto sc = gfldpc::pack(encode_info(s, info, rv), s.bits_per_cu, n_sym(s));  // the scrambling's symbols
    const int q = 1 << s.bits_per_cu;
    for (int i = 0; i < n_sym(s); ++i)
        for (int w = 0; w < q; ++w) out[i * q + w] = soft[i * q + (w ^ sc[0][i])];
    return out;
}

void combine(const Spec& s, Mat<double>& buf, const Mat<double>& soft, std::span<const int> rvs) {
    if (static_cast<int>(soft.cols) != soft_len(s))
        throw std::invalid_argument(std::string(s.name) + ": expected (B, soft_len)");
    const std::size_t B = soft.rows, L = static_cast<std::size_t>(buffer_len(s));
    if (buf.rows == 0) buf = Mat<double>(B, L, 0.0);
    if (buf.rows < B || buf.cols != L) throw std::invalid_argument(std::string(s.name) + ": buffer shape");
    std::vector<double> deint(soft.cols);
    const auto& perm = soft_perm(s);
    for (std::size_t b = 0; b < B; ++b) {
        for (std::size_t i = 0; i < soft.cols; ++i) deint[perm[i]] = soft[b][i];
        const auto pos = rv_positions(s, at(rvs, b, B, 0));
        for (std::size_t j = 0; j < pos.size(); ++j) buf[b][pos[j]] += deint[j];  // np.add.at: in order
    }
}

std::vector<Payload> payloads(const Spec& s, const Mat<std::uint8_t>& bits, std::span<const std::uint8_t> converged,
                              std::span<const std::uint32_t> masks, std::span<const int> index) {
    if (static_cast<int>(bits.cols) < 8 * framed_bytes(s) || converged.size() != bits.rows)
        throw std::invalid_argument(std::string(s.name) + ": payloads shape");
    std::vector<Payload> out;
    for (std::size_t b = 0; b < bits.rows; ++b) {
        auto data = pack(s, bits[b], pn_at(s, index_at(index, b, bits.rows)).data());
        const bool ok = converged[b] && crc_matches(s, data, mask_at(masks, b, bits.rows));
        data.resize(static_cast<std::size_t>(s.payload_bytes));
        out.push_back({std::move(data), ok});
    }
    return out;
}

namespace {
std::atomic<int> iters_cap{DEFAULT_ITERS};
}  // namespace

int ldpc_iters() { return iters_cap.load(std::memory_order_relaxed); }

void set_ldpc_iters(int n) {
    if (n < 1) throw std::invalid_argument("LDPC iterations must be >= 1");
    iters_cap.store(n, std::memory_order_relaxed);
}

Info decode_llrs(const Spec& s, const Mat<float>& llr, int iters, std::span<const std::uint32_t> masks,
                 std::span<const int> index) {
    if (s.gf) return decode_gf(s, gf_decoder(s), deinterleave_llr(s, llr));
    return decode_code_order(s, s.polar ? nullptr : &ldpc_decoder(s), deinterleave_llr(s, llr),
                             iters > 0 ? iters : ldpc_iters(), masks, index);
}

std::vector<Payload> decode_many(const Spec& s, const Mat<float>& soft, std::span<const std::uint32_t> masks,
                                 std::span<const int> index) {
    const auto d = decode_llrs(s, soft, 0, masks, index);
    return payloads(s, d.bits, d.ok, masks, index);
}

std::vector<Payload> decode_buffer(const Spec& s, const Mat<double>& buf, int max_rv, std::span<const std::uint32_t> masks,
                                   std::span<const int> index) {
    if (s.polar) {
        const auto d = decode_code_order(s, nullptr, cut(s, buf, static_cast<std::size_t>(s.coded_bits)), ldpc_iters(),
                                         masks, index);
        return payloads(s, d.bits, d.ok, masks, index);
    }
    const int extent = ir_extent(s, max_rv);
    const auto d = s.gf ? decode_gf(s, gf_decoder(s, extent), cut(s, buf, static_cast<std::size_t>(extent)))
                        : decode_code_order(s, &ldpc_decoder(s, extent), cut(s, buf, static_cast<std::size_t>(extent)),
                                            ldpc_iters(), {}, {});
    return payloads(s, d.bits, d.ok, masks, index);
}

Posterior decode_posterior(const Spec& s, std::span<const double> soft, std::span<const double> buf, int max_rv,
                           int rv) {
    if (s.polar || s.gf) throw std::invalid_argument(std::string(s.name) + ": decode_posterior is LDPC only");
    Mat<float> llr;
    const ldpc::Decoder* dec;
    if (buf.empty()) {
        if (static_cast<int>(soft.size()) != s.coded_bits)
            throw std::invalid_argument(std::string(s.name) + ": expected coded_bits soft bits");
        llr = Mat<float>(1, soft.size());
        for (std::size_t i = 0; i < soft.size(); ++i) llr[0][s.perm[i]] = static_cast<float>(soft[i]);
        dec = &ldpc_decoder(s);
    } else {
        const int extent = ir_extent(s, max_rv);
        if (static_cast<int>(buf.size()) < extent) throw std::invalid_argument(std::string(s.name) + ": buffer shape");
        llr = Mat<float>(1, static_cast<std::size_t>(extent));
        std::copy_n(buf.begin(), extent, llr.data.begin());
        dec = &ldpc_decoder(s, extent);
    }
    const auto d = dec->decode(llr, ldpc_iters(), {}, true);
    std::vector<double> code(d.posterior.data.begin(), d.posterior.data.end());
    if (!buf.empty()) {
        const auto pos = rv_positions(s, rv);
        std::vector<double> c(pos.size());
        for (std::size_t j = 0; j < pos.size(); ++j) c[j] = code[static_cast<std::size_t>(pos[j])];
        code = std::move(c);
    }
    std::vector<double> post(static_cast<std::size_t>(s.coded_bits));
    for (std::size_t i = 0; i < post.size(); ++i) post[i] = code[s.perm[i]];
    return {std::vector<std::uint8_t>(d.bits.data.begin(), d.bits.data.end()), d.ok[0] != 0, std::move(post)};
}

Raw decode_raw(const Spec& s, const Mat<float>& soft, std::span<const int> index) {
    const auto deint = deinterleave_llr(s, soft);
    const std::size_t B = soft.rows, k = static_cast<std::size_t>(s.k);
    Raw out;
    if (s.polar) {
        auto r = polar_decoder(s).decode(deint);
        reorder_paths(s, r.paths);
        out.list = POLAR_LIST;
        out.cands = std::move(r.paths);
        out.usable = Mat<std::uint8_t>(B, POLAR_LIST, 1);
    } else {
        auto r = s.gf ? decode_gf(s, gf_decoder(s), deint) : [&] {
            auto d = ldpc_decoder(s).decode(deint, ldpc_iters());
            return Info{std::move(d.bits), std::move(d.ok)};
        }();
        out.list = 1;
        out.cands = std::move(r.bits);
        out.usable = Mat<std::uint8_t>(B, 1);
        out.usable.data = std::move(r.ok);
    }
    for (std::size_t b = 0; b < B; ++b) {
        const auto pn = pn_at(s, index_at(index, b, B));
        for (std::size_t l = 0; l < static_cast<std::size_t>(out.list); ++l)
            for (std::size_t j = 0; j < k; ++j) out.cands[b][l * k + j] ^= pn[j];
    }
    return out;
}

std::optional<std::vector<std::uint8_t>> check(const Spec& s, std::span<const std::uint8_t> cands,
                                               std::span<const std::uint8_t> usable, std::uint32_t crc_mask) {
    const std::size_t k = static_cast<std::size_t>(s.k);
    if (cands.size() != usable.size() * k) throw std::invalid_argument(std::string(s.name) + ": check shape");
    for (std::size_t l = 0; l < usable.size(); ++l)
        if (usable[l]) {
            auto data = pack(s, cands.data() + l * k, nullptr);
            if (crc_matches(s, data, crc_mask)) {
                data.resize(static_cast<std::size_t>(s.payload_bytes));
                return data;
            }
        }
    return std::nullopt;
}

namespace {

// Where each info bit is sent at RV 0 in code order (its IR buffer position too), or -1 (codes._info_code_positions).
const std::vector<int>& info_code_positions(const Spec& s) {
    std::vector<int> out(static_cast<std::size_t>(s.k), -1);
    if (!s.polar && !s.gf) {
        const auto& sent = ldpc_code(s).sent();
        for (std::size_t p = 0; p < sent.size(); ++p)
            if (sent[p] < s.k) out[static_cast<std::size_t>(sent[p])] = static_cast<int>(p);
    }
    std::lock_guard lock(cache_mutex);
    auto& c = cache[&s];
    if (!c.info_code) c.info_code = std::move(out);
    return *c.info_code;
}

}  // namespace

const std::vector<int>& systematic_positions(const Spec& s) {
    const auto& code = info_code_positions(s);
    std::vector<int> where(static_cast<std::size_t>(s.coded_bits));  // code order -> mapping order
    if (!s.gf)  // GF(M): no info bit is sent as a bit, every one -1
        for (int i = 0; i < s.coded_bits; ++i) where[s.perm[i]] = i;
    std::vector<int> out(code.size(), -1);
    for (std::size_t i = 0; i < code.size(); ++i)
        if (code[i] >= 0) out[i] = where[static_cast<std::size_t>(code[i])];
    std::lock_guard lock(cache_mutex);
    auto& c = cache[&s];
    if (!c.systematic) c.systematic = std::move(out);
    return *c.systematic;
}

std::vector<std::int8_t> zero_tail(const Spec& s, int nbytes) {
    std::vector<std::int8_t> out(static_cast<std::size_t>(s.k), -1);
    for (int i = 8 * nbytes; i < 8 * s.payload_bytes; ++i) out[static_cast<std::size_t>(i)] = 0;
    for (int i = 8 * s.payload_bytes + s.crc_bits; i < s.k; ++i) out[static_cast<std::size_t>(i)] = 0;
    return out;
}

std::vector<std::int8_t> known_bits(const Spec& s, std::span<const std::uint8_t> tmpl, std::span<const std::uint8_t> mask) {
    std::vector<std::int8_t> out(static_cast<std::size_t>(s.k), -1);
    const auto bit = [](std::span<const std::uint8_t> v, int i) {
        return static_cast<std::size_t>(i / 8) < v.size() ? (v[static_cast<std::size_t>(i / 8)] >> (7 - i % 8)) & 1 : 0;
    };
    for (int i = 0; i < 8 * s.payload_bytes; ++i)
        if (bit(mask, i)) out[static_cast<std::size_t>(i)] = static_cast<std::int8_t>(bit(tmpl, i));
    for (int i = 8 * s.payload_bytes + s.crc_bits; i < s.k; ++i) out[static_cast<std::size_t>(i)] = 0;
    return out;
}

double score(const Spec& s, std::span<const double> soft, std::span<const std::uint8_t> payload, std::uint32_t crc_mask,
             int index) {
    const auto c = encode(s, payload, 0, crc_mask, index);
    double sum = 0.0;
    for (std::size_t i = 0; i < c.size() && i < soft.size(); ++i) {
        const double x = -(1.0 - 2.0 * c[i]) * soft[i];  // log 2 - logaddexp(0, x), as numpy
        sum += std::log(2.0) - (x > 0.0 ? x + std::log1p(std::exp(-x)) : std::log1p(std::exp(x)));
    }
    return sum;
}

std::vector<float> pin(const Spec& s, std::span<const double> soft, std::span<const std::int8_t> known, int index) {
    std::vector<float> out(soft.begin(), soft.end());
    const auto& pos = systematic_positions(s);
    const auto pn = pn_at(s, index);
    for (std::size_t i = 0; i < known.size() && i < pos.size(); ++i)
        if (known[i] >= 0 && pos[i] >= 0)
            out[static_cast<std::size_t>(pos[i])] = ldpc::CH_CLAMP * (1.0f - 2.0f * static_cast<float>((known[i] ^ pn[i]) & 1));
    return out;
}

std::vector<double> pin_buffer(const Spec& s, std::span<const double> buf, std::span<const std::int8_t> known) {
    std::vector<double> out(buf.begin(), buf.end());
    const auto& pos = info_code_positions(s);
    for (std::size_t i = 0; i < known.size() && i < pos.size(); ++i)
        if (known[i] >= 0 && pos[i] >= 0) out[static_cast<std::size_t>(pos[i])] = ldpc::CH_CLAMP * (1.0 - 2.0 * known[i]);
    return out;
}

std::vector<double> info_llrs(const Spec& s, std::span<const double> soft, int index) {
    const auto& pos = systematic_positions(s);
    const auto pn = pn_at(s, index);
    std::vector<double> out(pos.size(), 0.0);
    for (std::size_t i = 0; i < pos.size(); ++i)
        if (pos[i] >= 0 && static_cast<std::size_t>(pos[i]) < soft.size())
            out[i] = soft[static_cast<std::size_t>(pos[i])] * (1.0 - 2.0 * pn[i]);
    return out;
}

std::vector<double> info_llrs_buffer(const Spec& s, std::span<const double> buf) {
    const auto& pos = info_code_positions(s);
    std::vector<double> out(pos.size(), 0.0);
    for (std::size_t i = 0; i < pos.size(); ++i)
        if (pos[i] >= 0 && static_cast<std::size_t>(pos[i]) < buf.size()) out[i] = buf[static_cast<std::size_t>(pos[i])];
    return out;
}

std::vector<double> pad_prior(const Spec& s, std::span<const double> llr) {
    const int pb = s.payload_bytes;
    const auto at = [&](int b) { return static_cast<std::size_t>(b) < llr.size() ? llr[static_cast<std::size_t>(b)] : 0.0; };
    // S(j), j = 1..pb (S(pb) = 0): one sequential sum from the payload's last
    // bit, as codes.pad_prior's cumsum adds; log 2 - logaddexp(0, -l) as numpy
    // computes it (0 exactly where l is 0)
    std::vector<double> S(static_cast<std::size_t>(pb), 0.0);
    double acc = 0.0;
    for (int b = 8 * pb - 1; b >= 8; --b) {
        const double x = -at(b);
        const double lae = x == 0.0 ? std::log(2.0) : x > 0.0 ? x + std::log1p(std::exp(-x)) : std::log1p(std::exp(x));
        acc += std::log(2.0) - lae;
        if (b % 8 == 0) S[static_cast<std::size_t>(b / 8 - 1)] = acc;
    }
    const double top = *std::max_element(S.begin(), S.end());
    double sum = 0.0;
    for (auto& v : S) sum += (v = std::exp(v - top));
    std::vector<double> out(static_cast<std::size_t>(s.k), 0.0);
    double pad = 0.0;  // P(start <= byte b)
    for (int b = 1; b < pb; ++b) {
        pad += S[static_cast<std::size_t>(b - 1)] / sum;
        const double v = 2.0 * std::atanh(std::min(pad, 1.0 - 1e-7));
        for (int i = 8 * b; i < 8 * b + 8; ++i) out[static_cast<std::size_t>(i)] = v;
    }
    for (int i = 8 * pb + s.crc_bits; i < s.k; ++i) out[static_cast<std::size_t>(i)] = INFINITY;
    return out;
}

std::vector<float> add_prior(const Spec& s, std::span<const double> soft, std::span<const double> prior, int index) {
    std::vector<float> out(soft.begin(), soft.end());
    const auto& pos = systematic_positions(s);
    const auto pn = pn_at(s, index);
    for (std::size_t i = 0; i < prior.size() && i < pos.size(); ++i) {
        if (prior[i] == 0.0 || pos[i] < 0) continue;
        const double sign = 1.0 - 2.0 * pn[i];
        auto& o = out[static_cast<std::size_t>(pos[i])];
        o = std::isinf(prior[i]) ? static_cast<float>(ldpc::CH_CLAMP * sign) : static_cast<float>(o + prior[i] * sign);
    }
    return out;
}

std::vector<double> add_prior_buffer(const Spec& s, std::span<const double> buf, std::span<const double> prior) {
    std::vector<double> out(buf.begin(), buf.end());
    const auto& pos = info_code_positions(s);
    for (std::size_t i = 0; i < prior.size() && i < pos.size(); ++i) {
        if (prior[i] == 0.0 || pos[i] < 0 || static_cast<std::size_t>(pos[i]) >= out.size()) continue;
        auto& o = out[static_cast<std::size_t>(pos[i])];
        o = std::isinf(prior[i]) ? ldpc::CH_CLAMP : o + prior[i];
    }
    return out;
}

const std::vector<int>& polar_order(const Spec& s) {
    const int nb = 8 * s.payload_bytes;
    std::vector<int> out(static_cast<std::size_t>(s.k));
    for (int j = 0; j < s.k; ++j) out[static_cast<std::size_t>(j)] = j < nb ? nb - 1 - j : j;
    std::lock_guard lock(cache_mutex);
    auto& c = cache[&s];
    if (!c.order) c.order = std::move(out);
    return *c.order;
}

namespace {

// The full code, and the decoder of the code with `known` (code-order info indices) frozen; built once per pattern.
struct PinnedPolar {
    std::unique_ptr<polar::PolarCode> full;
    std::unique_ptr<polar::SCLDecoder> dec;
    std::vector<int> rest;  // code-order info indices it decodes
};
std::mutex pinned_mutex;
std::map<std::pair<const Spec*, std::vector<int>>, PinnedPolar> pinned_cache;

const PinnedPolar& pinned_polar(const Spec& s, const std::vector<int>& known) {
    const auto& code = polar_code(s);
    std::lock_guard lock(pinned_mutex);
    auto& p = pinned_cache[{&s, known}];
    if (!p.dec) {
        std::vector<std::uint16_t> info;
        std::vector<std::uint8_t> is_known(static_cast<std::size_t>(s.k), 0);
        for (int j : known) is_known[static_cast<std::size_t>(j)] = 1;
        for (int j = 0; j < s.k; ++j)
            if (!is_known[static_cast<std::size_t>(j)]) {
                p.rest.push_back(j);
                info.push_back(code.info_pos[static_cast<std::size_t>(j)]);
            }
        p.full = std::make_unique<polar::PolarCode>(s.k, s.coded_bits, code.info_pos);
        p.dec = std::make_unique<polar::SCLDecoder>(polar::PolarCode(static_cast<int>(info.size()), s.coded_bits, info),
                                                    POLAR_LIST);
    }
    return p;
}

// codes._pinned_paths: (B, coded_bits) code-order LLRs, sent at `index` -> (B, L * k) info bits, scrambled, in
// the info bits' order, best first.
Mat<std::uint8_t> pinned_paths(const Spec& s, const Mat<float>& deint, std::span<const std::int8_t> known, int index) {
    const auto& order = polar_order(s);
    const auto pn = pn_at(s, index);
    const std::size_t k = static_cast<std::size_t>(s.k);
    std::vector<std::int8_t> kc(k);  // known, in the code's order
    std::vector<std::uint8_t> u(k, 0);
    std::vector<int> kp;
    for (std::size_t j = 0; j < k; ++j) {
        const std::size_t i = static_cast<std::size_t>(order[j]);
        kc[j] = i < known.size() ? known[i] : -1;
        if (kc[j] >= 0) {
            u[j] = static_cast<std::uint8_t>((kc[j] ^ pn[i]) & 1);
            kp.push_back(static_cast<int>(j));
        }
    }
    const auto& p = pinned_polar(s, kp);
    Mat<std::uint8_t> ub(1, k);
    ub.data = u;
    const auto xk = p.full->encode(ub);
    Mat<float> llr(deint.rows, deint.cols);
    for (std::size_t b = 0; b < deint.rows; ++b)
        for (std::size_t i = 0; i < deint.cols; ++i) llr[b][i] = xk[0][i] ? -deint[b][i] : deint[b][i];
    const auto r = p.dec->decode(llr);
    const std::size_t L = static_cast<std::size_t>(p.dec->list_size()), kr = p.rest.size();
    Mat<std::uint8_t> out(deint.rows, L * k);
    for (std::size_t b = 0; b < deint.rows; ++b)
        for (std::size_t l = 0; l < L; ++l) {
            std::uint8_t* o = out[b] + l * k;
            for (int j : kp) o[j] = u[static_cast<std::size_t>(j)];
            for (std::size_t t = 0; t < kr; ++t) o[p.rest[t]] = r.paths[b][l * kr + t];
        }
    reorder_paths(s, out);
    return out;
}

}  // namespace

Raw decode_raw_pinned(const Spec& s, const Mat<float>& soft, std::span<const std::int8_t> known, int index) {
    const std::vector<int> idx(soft.rows, index);
    if (!s.polar) {
        Mat<float> pinned(soft.rows, soft.cols);
        for (std::size_t b = 0; b < soft.rows; ++b) {
            const std::vector<double> row(soft[b], soft[b] + soft.cols);
            const auto v = pin(s, row, known, index);
            std::copy(v.begin(), v.end(), pinned[b]);
        }
        return decode_raw(s, pinned, idx);
    }
    Raw out;
    out.list = POLAR_LIST;
    out.cands = pinned_paths(s, deinterleave_llr(s, soft), known, index);
    out.usable = Mat<std::uint8_t>(soft.rows, POLAR_LIST, 1);
    const auto pn = pn_at(s, index);
    const std::size_t k = static_cast<std::size_t>(s.k);
    for (std::size_t b = 0; b < soft.rows; ++b)
        for (std::size_t l = 0; l < static_cast<std::size_t>(POLAR_LIST); ++l)
            for (std::size_t j = 0; j < k; ++j) out.cands[b][l * k + j] ^= pn[j];
    return out;
}

Raw decode_buffer_raw(const Spec& s, const Mat<double>& buf, int max_rv, std::span<const std::int8_t> known) {
    const std::size_t B = buf.rows;
    Raw out;
    if (s.polar) {
        const auto deint = cut(s, buf, static_cast<std::size_t>(s.coded_bits));
        out.list = POLAR_LIST;
        if (known.empty()) {
            auto r = polar_decoder(s).decode(deint);
            reorder_paths(s, r.paths);
            out.cands = std::move(r.paths);
        } else {
            out.cands = pinned_paths(s, deint, known, PLAIN);
        }
        out.usable = Mat<std::uint8_t>(B, POLAR_LIST, 1);
        return out;
    }
    const int extent = ir_extent(s, max_rv);
    Mat<double> b = buf;
    if (!known.empty())
        for (std::size_t r = 0; r < B; ++r) {
            const auto v = pin_buffer(s, buf.row(r), known);
            std::copy(v.begin(), v.end(), b[r]);
        }
    auto r = s.gf ? decode_gf(s, gf_decoder(s, extent), cut(s, b, static_cast<std::size_t>(extent))) : [&] {
        auto d = ldpc_decoder(s, extent).decode(cut(s, b, static_cast<std::size_t>(extent)), ldpc_iters());
        return Info{std::move(d.bits), std::move(d.ok)};
    }();
    out.list = 1;
    out.cands = std::move(r.bits);
    out.usable = Mat<std::uint8_t>(B, 1);
    out.usable.data = std::move(r.ok);
    return out;
}

std::vector<Payload> decode_buffer_pinned(const Spec& s, const Mat<double>& buf, int max_rv,
                                          std::span<const std::int8_t> known, std::uint32_t crc_mask) {
    const std::uint32_t masks[1] = {crc_mask};
    const int plain[1] = {PLAIN};
    if (!s.polar) {
        Mat<double> pinned(buf.rows, buf.cols);
        for (std::size_t b = 0; b < buf.rows; ++b) {
            const auto v = pin_buffer(s, buf.row(b), known);
            std::copy(v.begin(), v.end(), pinned[b]);
        }
        return decode_buffer(s, pinned, max_rv, masks, plain);
    }
    const auto paths = pinned_paths(s, cut(s, buf, static_cast<std::size_t>(s.coded_bits)), known, PLAIN);
    const std::size_t B = buf.rows, L = static_cast<std::size_t>(POLAR_LIST), k = static_cast<std::size_t>(s.k);
    Mat<std::uint8_t> bits(B, k);
    std::vector<std::uint8_t> ok(B, 0);
    for (std::size_t b = 0; b < B; ++b) {
        std::size_t pick = 0;
        for (std::size_t l = 0; l < L; ++l)
            if (crc_matches(s, pack(s, paths[b] + l * k, nullptr), crc_mask)) {
                pick = l;
                ok[b] = 1;
                break;
            }
        std::copy_n(paths[b] + pick * k, k, bits[b]);
    }
    return payloads(s, bits, ok, masks, plain);
}

std::vector<std::uint8_t> descramble(const Spec& s, std::span<const std::uint8_t> bits, int index) {
    if (static_cast<int>(bits.size()) > s.k) throw std::invalid_argument(std::string(s.name) + ": more than k bits");
    const auto pn = pn_at(s, index);
    std::vector<std::uint8_t> out(bits.size());
    for (std::size_t i = 0; i < bits.size(); ++i) out[i] = bits[i] ^ pn[i];
    return out;
}

std::vector<std::uint8_t> crc_ok(const Spec& s, const Mat<std::uint8_t>& bits, std::span<const std::uint32_t> masks,
                                 std::span<const int> index) {
    if (static_cast<int>(bits.cols) < 8 * framed_bytes(s)) throw std::invalid_argument(std::string(s.name) + ": crc_ok shape");
    std::vector<std::uint8_t> out(bits.rows);
    for (std::size_t b = 0; b < bits.rows; ++b)
        out[b] = crc_matches(s, pack(s, bits[b], pn_at(s, index_at(index, b, bits.rows)).data()),
                             mask_at(masks, b, bits.rows));
    return out;
}

}  // namespace data2g::codes
