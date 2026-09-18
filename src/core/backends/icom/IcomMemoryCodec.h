#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "core/backends/icom/CivCodec.h"
#include "core/backends/icom/IcomModels.h"

namespace AetherSDR::icom {

struct IcomMemoryChannel {
    int group = -1;
    int channel = 0;
    bool occupied = false;
    bool recallable = true;
    bool split = false;
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

[[nodiscard]] std::vector<std::uint8_t> cmdReadMemory(
    std::uint8_t to, MemoryDialect dialect, int group, int channel);
[[nodiscard]] std::optional<IcomMemoryChannel> decodeMemory(
    MemoryDialect dialect, std::span<const std::uint8_t> payload);
[[nodiscard]] int memoryIndex(MemoryDialect dialect, int group, int channel) noexcept;
[[nodiscard]] std::string memoryGroupName(MemoryDialect dialect, int group);

[[nodiscard]] std::optional<std::vector<std::vector<std::uint8_t>>>
buildExtendedMemoryRecallFrames(
    std::uint8_t to, CivMode mode, bool dataMode, int filter,
    RepeaterOffsetDirection direction, int offsetHz, std::uint8_t accessMode,
    double txToneHz, double rxToneHz, int dtcsCode,
    bool dtcsTxReverse, bool dtcsRxReverse);

}  // namespace AetherSDR::icom
