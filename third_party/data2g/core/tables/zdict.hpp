// The ARQ frames' deflate dictionary, compiled in: generated/zdict.cpp.
#pragma once

#include <cstdint>
#include <span>

namespace data2g::tables {

// Deflate's priming dictionary (data2g/arq/frames.py ZDICT, zdict.bin).
extern const std::span<const std::uint8_t> ZDICT;

}  // namespace data2g::tables
