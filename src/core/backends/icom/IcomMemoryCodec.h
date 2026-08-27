#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "core/backends/icom/CivCodec.h"

namespace AetherSDR::icom {

// IC-9700 CI-V Reference Guide, command 1A 00. This first implementation is
// intentionally read-only and covers the 99 ordinary channels in each of the
// radio's three RF-band groups. Scan-edge, call and satellite memories are a
// separate product surface and are not inferred from this record.
struct IcomMemoryChannel {
    int band = 0;       // 1=144 MHz, 2=430 MHz, 3=1.2 GHz
    int channel = 0;    // 1..99
    bool occupied = false;
    std::uint64_t frequencyHz = 0;
    std::string mode;
    int filter = 0;
    int dataMode = 0;
    int duplex = 0;
    int toneMode = 0;
    double txToneHz = 0.0;
    double rxToneHz = 0.0;
    int dtcsCode = 23;
    bool dtcsTxReverse = false;
    bool dtcsRxReverse = false;
    std::uint64_t offsetHz = 0;
    std::string name;
};

[[nodiscard]] std::vector<std::uint8_t> cmdReadIc9700Memory(
    std::uint8_t to, int band, int channel);

// Returns nullopt for malformed or out-of-scope records. Empty channels are a
// successful decode with occupied=false so a refresh can remove stale cache.
[[nodiscard]] std::optional<IcomMemoryChannel> decodeIc9700Memory(
    std::span<const std::uint8_t> payload);

[[nodiscard]] int ic9700MemoryIndex(int band, int channel) noexcept;

[[nodiscard]] std::optional<std::vector<std::vector<std::uint8_t>>>
buildIc9700MemoryRecallFrames(
    std::uint8_t to, CivMode mode, bool dataMode, int filter,
    RepeaterOffsetDirection direction, int offsetHz, std::uint8_t accessMode,
    double txToneHz, double rxToneHz, int dtcsCode,
    bool dtcsTxReverse, bool dtcsRxReverse);

}  // namespace AetherSDR::icom
