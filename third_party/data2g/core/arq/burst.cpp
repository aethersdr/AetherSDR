#include "arq/burst.hpp"

#include <zlib.h>

#include <algorithm>

namespace data2g::arq {

bool dup_ctl(const TxBurst& b) {
    return std::any_of(b.slots.begin(), b.slots.end(), [](const Slot& s) { return s.rv && s.mask_id.seq >= SEQ_MOD; });
}

Bytes inline_check(const MaskId& m, ByteView payload) {
    const std::uint8_t id[] = {static_cast<std::uint8_t>(m.key >> 8), static_cast<std::uint8_t>(m.key & 255),
                               static_cast<std::uint8_t>(m.direction), static_cast<std::uint8_t>(m.seq)};
    uLong c = ::crc32(0L, id, 4);
    c = ::crc32(c, payload.data(), static_cast<uInt>(payload.size()));
    return Bytes{static_cast<std::uint8_t>(c >> 8 & 255), static_cast<std::uint8_t>(c & 255)};
}

}  // namespace data2g::arq
