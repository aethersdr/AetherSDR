// data2g/arq/frames.py: the 32-bit core control word, extension TLVs,
// bitmaps, callsigns, stream records and compression (docs/arq.md §4, §5,
// §7, §9a).
//
// Malformed input fails as Python does: what raises ValueError there throws
// std::invalid_argument here, an IndexError std::out_of_range.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace data2g::arq {

using Bytes = std::vector<std::uint8_t>;
using ByteView = std::span<const std::uint8_t>;

inline constexpr int SEQ_BITS = 7;
inline constexpr int SEQ_MOD = 1 << SEQ_BITS;
inline constexpr int BITMAP_BITS = 64;
inline constexpr int WINDOW = SEQ_MOD / 2 - 1;
inline constexpr int BURST_MOD = 8;
inline constexpr int MAX_CTL = 4;  // control codewords a burst may have: the core's 2-bit n_ctl

// frame types (core word)
inline constexpr int ARQ = 0, SESSION = 1, PROBE = 2, ARQ_DUP = 3;
// extension types
inline constexpr int T_PAD = 0, T_NEW = 1, T_ABANDON = 2, T_RV = 3, T_BITMAP = 4, T_RESYNC = 5, T_REPORT = 6,
                     T_SURVEY = 7, T_SOUND = 8, T_BUFFER = 9,
                     T_SESS = 10,  // a session frame's body (arq/session.hpp)
                     T_REPLY = 11, T_CHAT = 12, T_CQ = 13, T_DUPCTL = 14,
                     T_COMP = 15,
                     T_ID = 16,  // packed callsign + session key: an ID frame (docs/arq.md §7a), mask 0
                     T_DRAINED = 17,  // empty: this burst's new codewords end the sender's queued data
                     // a data codeword in the control's padding (docs/arq.md §5a): seq | comp << 7, the
                     // sender's abandon epoch, its payload with trailing zeros cut
                     T_INLINE = 18;
// An extension type's short name ("ext N" for one not listed): monitor/'s
// dumps. A new T_* gets its name here too. (Native only.)
std::string ext_name(int t);
inline constexpr int CHAT_LINE_BYTES = 200;
inline constexpr int HIST = 4096;
inline constexpr int MAX_INFLATE = 1 << 16;
// session control subtypes
inline constexpr int CONNECT = 1, CONNECT_ACK = 2, CONNECT_NAK = 3, DISC = 4, DISC_ACK = 5;

// Python's % and //: the result takes the divisor's sign.
inline std::int64_t pmod(std::int64_t a, std::int64_t m) {
    const std::int64_t r = a % m;
    return r < 0 ? r + m : r;
}
// -(-a // b) for a >= 0, b > 0
inline std::int64_t ceil_div(std::int64_t a, std::int64_t b) { return (a + b - 1) / b; }

// b[i], or std::out_of_range as Python's IndexError.
std::uint8_t at(ByteView b, std::size_t i);

struct Core {
    int ftype = ARQ;     // 2 bits
    int n_ctl = 1;       // 1-MAX_CTL control codewords
    int burst_seq = 0;   // 3 bits
    int acted_on = 0;    // 3 bits
    int cum = 0;         // 7 bits
    bool reply_lost = false;
    int k = 0;           // 6 bits
    int recommend = 0;   // 6 bits
    int size_hint = 1;   // 2 bits

    Bytes pack() const;
    static Core unpack(ByteView b);  // the first 4 bytes (fewer: as many as there are)
    bool operator==(const Core&) const = default;
};

using Ext = std::map<int, Bytes>;  // type -> value, packed in type order

struct Control {
    Core core;
    Ext ext;

    // -> control codeword payloads; sets core.n_ctl to fit.
    std::vector<Bytes> pack(int payload_bytes);
    static Control unpack(const std::vector<Bytes>& payloads);
};

Bytes pack_bitmap(const std::set<int>& received, int cum);
std::set<int> unpack_bitmap(ByteView b, int cum);
Bytes pack_rv(const std::vector<int>& rvs);
std::vector<int> unpack_rv(ByteView b, int k);
Bytes pack_flags(const std::vector<bool>& flags);
std::vector<bool> unpack_flags(ByteView b, int n);

// Raw deflate (level 9, window 15, memLevel 9) primed with ZDICT + hist.
Bytes deflate(ByteView hist, ByteView data);
// The longest prefix of data past pb bytes whose deflate fits pb bytes.
std::optional<std::pair<std::int64_t, Bytes>> deflate_fit(ByteView hist, ByteView data, int pb);
// A compressed codeword (zero padded) -> its stream bytes.
Bytes inflate(ByteView hist, ByteView payload);

inline constexpr int CALL_CHARS = 10;
Bytes pack_call(const std::string& call);
std::string unpack_call(ByteView b);

// --- session frame bodies (docs/arq.md §7, §7a) ---
// A T_SESS body is its subtype byte (CONNECT ... DISC_ACK) and the fields
// below; DISC and DISC_ACK are the subtype alone. T_CQ and T_ID bodies have
// no subtype. pack() throws std::invalid_argument for a call that won't
// pack or a field over a byte; unpack() for a call that won't unpack.

// subtype, version, caller and callee (pack_call, 8 B each), nonce (2 B),
// bandwidth cap code, t_turn in tenths of a second
struct Connect {
    static constexpr std::size_t BYTES = 22;
    int version = 0;
    std::string caller, callee;
    std::int64_t nonce = 0;
    int cap = 0, t_turn10 = 0;

    Bytes pack() const;
    static Connect unpack(ByteView body);  // BYTES at least
    static std::pair<std::string, std::string> calls(ByteView body);  // (caller, callee) from the first 18 bytes
};

// subtype, the CONNECT's nonce (2 B), the agreed cap code, the callee's t_turn in tenths
struct ConnectAck {
    static constexpr std::size_t BYTES = 5;
    std::int64_t nonce = 0;
    int cap = 0, t_turn10 = 0;

    Bytes pack() const;
    static ConnectAck unpack(ByteView body);  // BYTES at least
};

// subtype, the CONNECT's nonce (2 B), the reason (NAK_*)
struct ConnectNak {
    static constexpr std::size_t BYTES = 4;
    std::int64_t nonce = 0;
    int reason = 0;

    Bytes pack() const;
    static ConnectNak unpack(ByteView body);  // BYTES at least
};

Bytes pack_cq(const std::string& call, int cap);  // a T_CQ body: the call and its bandwidth cap code
std::pair<std::string, int> unpack_cq(ByteView body);  // invalid_argument: "CQ of n B"
Bytes pack_id(const std::string& call, int key);  // a T_ID body: the call and the session key it identifies for
std::pair<std::string, int> unpack_id(ByteView body);  // invalid_argument: "ID of n B"

// A CONNECT in 20 B, for a CPM control codeword: no Core or TLV, bit-packed
// under its own CRC mask (burst COMPACT_CONNECT). data2g/arq/frames.py.
inline constexpr int COMPACT_BYTES = 20;
Bytes pack_connect(ByteView body);  // a Connect body; throws std::invalid_argument
Bytes unpack_connect(ByteView p);

Bytes to_records(ByteView data);

class RecordReader {
public:
    Bytes buf;
    std::int64_t delivered = 0;  // host bytes out
    Bytes feed(ByteView b);
};

}  // namespace data2g::arq
