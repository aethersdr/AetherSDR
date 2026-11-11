// data2g/tnc.py's KISS byte codec: frames to and from a KISS client's byte
// stream. The TCP server is the app's (app/tcp_servers.hpp); what the
// frames carry over the air is kisslink/.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace data2g::tnc {

using Bytes = std::vector<std::uint8_t>;

inline constexpr std::uint8_t FEND = 0xC0, FESC = 0xDB, TFEND = 0xDC, TFESC = 0xDD;
inline constexpr std::size_t KISS_MAX_FRAME = 1u << 16;  // a longer frame is junk (a client that never sends FEND)

inline constexpr int KISS_DATA = 0x00, KISS_ACKMODE = 0x0C;  // KISS commands (low nibble; the port is the high one)
// A KISS frame: a data frame (command 0) for `data` on `port` by default.
Bytes kiss_encode(std::span<const std::uint8_t> data, int port = 0, int cmd = KISS_DATA);

// Bytes in, whole frames (command byte, data) out, across reads.
class KissDecoder {
public:
    std::vector<std::pair<int, Bytes>> feed(std::span<const std::uint8_t> data);

private:
    Bytes buf_;
    bool esc_ = false, skip_ = false;  // skip_: over-long frame, discarding to the next FEND
};

}  // namespace data2g::tnc
