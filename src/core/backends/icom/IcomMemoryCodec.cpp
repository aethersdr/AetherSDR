#include "core/backends/icom/IcomMemoryCodec.h"

#include "core/backends/icom/CivCodec.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <ranges>

namespace AetherSDR::icom {
namespace {

constexpr std::size_t kEmptyRecordBytes = 4;
constexpr std::size_t kSingleRecordBytes = 67;
constexpr std::size_t kSplitRecordBytes = 114;

bool validBcd(std::uint8_t value) noexcept
{
    return (value & 0x0f) <= 9 && ((value >> 4) & 0x0f) <= 9;
}

std::optional<int> decodeChannel(std::span<const std::uint8_t> bytes)
{
    if (bytes.size() != 2 || !validBcd(bytes[0]) || !validBcd(bytes[1])) {
        return std::nullopt;
    }
    return decodeBcdByte(bytes[0]) * 100 + decodeBcdByte(bytes[1]);
}

std::string decodeName(std::span<const std::uint8_t> bytes)
{
    std::string name;
    name.reserve(bytes.size());
    for (const std::uint8_t value : bytes) {
        if (value == 0x00 || value == 0xff) {
            break;
        }
        if (value >= 0x20 && value <= 0x7e) {
            name.push_back(static_cast<char>(value));
        }
    }
    while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back()))) {
        name.pop_back();
    }
    return name;
}

std::optional<CivMode> decodeMode(std::uint8_t value)
{
    switch (value) {
    case 0x00: return CivMode::Lsb;
    case 0x01: return CivMode::Usb;
    case 0x02: return CivMode::Am;
    case 0x03: return CivMode::Cw;
    case 0x04: return CivMode::Rtty;
    case 0x05: return CivMode::Fm;
    case 0x07: return CivMode::CwR;
    case 0x08: return CivMode::RttyR;
    case 0x17: return CivMode::Dv;
    default: return std::nullopt;
    }
}

std::string nativeModeName(CivMode mode)
{
    switch (mode) {
    case CivMode::Lsb:   return "LSB";
    case CivMode::Usb:   return "USB";
    case CivMode::Am:    return "AM";
    case CivMode::Cw:    return "CW";
    case CivMode::Rtty:  return "RTTY";
    case CivMode::Fm:    return "FM";
    case CivMode::CwR:   return "CW-R";
    case CivMode::RttyR: return "RTTY-R";
    case CivMode::Dv:    return "DSTAR";
    case CivMode::Wfm:   return "WFM";
    }
    return {};
}

}  // namespace

std::vector<std::uint8_t> cmdReadIc9700Memory(
    std::uint8_t to, int band, int channel)
{
    if (band < 1 || band > 3 || channel < 1 || channel > 99) {
        return {};
    }
    const std::array<std::uint8_t, 3> address{
        encodeBcdByte(band),
        encodeBcdByte(channel / 100),
        encodeBcdByte(channel % 100),
    };
    return buildFrameSub(to, cmd::kSetting, 0x00, address);
}

std::optional<IcomMemoryChannel> decodeIc9700Memory(
    std::span<const std::uint8_t> payload)
{
    if (payload.size() < 3 || !validBcd(payload[0])) {
        return std::nullopt;
    }
    const int band = decodeBcdByte(payload[0]);
    const std::optional<int> channel = decodeChannel(payload.subspan(1, 2));
    if (band < 1 || band > 3 || !channel || *channel < 1 || *channel > 99) {
        return std::nullopt;
    }

    IcomMemoryChannel memory;
    memory.band = band;
    memory.channel = *channel;

    // An unused channel is returned in the same short shape the guide defines
    // for clearing it: address plus FF. Accept the address-only form as well;
    // older firmware has been observed to omit the marker on reads.
    if (payload.size() == 3
        || (payload.size() == kEmptyRecordBytes && payload[3] == 0xff)) {
        return memory;
    }
    if (payload.size() != kSingleRecordBytes
        && payload.size() != kSplitRecordBytes) {
        return std::nullopt;
    }

    const std::optional<std::uint64_t> frequency =
        decodeFreqExact(payload.subspan(4, kFreqBytes), kFreqBytes);
    const std::optional<CivMode> mode = decodeMode(payload[9]);
    const int filter = validBcd(payload[10]) ? decodeBcdByte(payload[10]) : 0;
    if (!frequency || *frequency == 0 || !mode || filter < 1 || filter > 3
        || (payload[11] != 0x00 && payload[11] != 0x01)) {
        return std::nullopt;
    }
    const int duplex = (payload[12] >> 4) & 0x0f;
    const int toneMode = payload[12] & 0x0f;
    static constexpr std::array<int, 8> kToneModes{0, 1, 2, 3, 6, 7, 8, 9};
    if (duplex > 3
        || std::ranges::find(kToneModes, toneMode) == kToneModes.end()) {
        return std::nullopt;
    }
    const std::optional<double> txTone =
        decodeRepeaterToneHz(payload.subspan(14, 3));
    const std::optional<double> rxTone =
        decodeRepeaterToneHz(payload.subspan(17, 3));
    const std::optional<RepeaterToneRegister> dtcs =
        decodeRepeaterToneRegister(payload.subspan(20, 3));
    const std::optional<int> offset =
        decodeRepeaterOffsetHz(payload.subspan(24, 3));
    if (!txTone || !rxTone || !dtcs || !offset) {
        return std::nullopt;
    }

    memory.occupied = true;
    memory.frequencyHz = *frequency;
    // Preserve the two fields the radio stores. Combining DATA with MODE here
    // (DFM/DIGU/DIGL) makes the Memory Channels table cease to represent the
    // native record and duplicates the separate Data Mode column. Recall
    // derives the neutral operating mode at the point where it is applied.
    memory.mode = nativeModeName(*mode);
    if (memory.mode.empty()) {
        return std::nullopt;
    }
    memory.filter = filter;
    memory.dataMode = payload[11];
    memory.duplex = duplex;
    memory.toneMode = toneMode;
    memory.txToneHz = *txTone;
    memory.rxToneHz = *rxTone;
    memory.dtcsCode = dtcs->value;
    memory.dtcsTxReverse = dtcs->txReverse;
    memory.dtcsRxReverse = dtcs->rxReverse;
    memory.offsetHz = *offset;
    const std::size_t nameOffset = payload.size() - 16;
    memory.name = decodeName(payload.subspan(nameOffset, 16));
    return memory;
}

int ic9700MemoryIndex(int band, int channel) noexcept
{
    if (band < 1 || band > 3 || channel < 1 || channel > 99) {
        return -1;
    }
    return (band - 1) * 99 + (channel - 1);
}

std::optional<std::vector<std::vector<std::uint8_t>>> buildIc9700MemoryRecallFrames(
    std::uint8_t to, CivMode mode, bool dataMode, int filter,
    RepeaterOffsetDirection direction, int offsetHz, std::uint8_t accessMode,
    double txToneHz, double rxToneHz, int dtcsCode,
    bool dtcsTxReverse, bool dtcsRxReverse)
{
    static constexpr std::array<int, 104> kDtcsCodes{{
        23, 25, 26, 31, 32, 36, 43, 47, 51, 53, 54, 65, 71, 72, 73, 74,
        114, 115, 116, 122, 125, 131, 132, 134, 143, 145, 152, 155, 156, 162,
        165, 172, 174, 205, 212, 223, 225, 226, 243, 244, 245, 246, 251, 252,
        255, 261, 263, 265, 266, 271, 274, 306, 311, 315, 325, 331, 332, 343,
        346, 351, 356, 364, 365, 371, 411, 412, 413, 423, 431, 432, 445, 446,
        452, 454, 455, 462, 464, 465, 466, 503, 506, 516, 523, 526, 532, 546,
        565, 606, 612, 624, 627, 631, 632, 654, 662, 664, 703, 712, 723, 731,
        732, 734, 743, 754,
    }};
    static constexpr std::array<std::uint8_t, 8> kAccessModes{{0, 1, 2, 3, 6, 7, 8, 9}};
    if (filter < 1 || filter > 3 || offsetHz < 0 || offsetHz > 99'999'900
        || !std::isfinite(txToneHz) || txToneHz < 0.0 || txToneHz > 299.9
        || !std::isfinite(rxToneHz) || rxToneHz < 0.0 || rxToneHz > 299.9
        || std::ranges::find(kDtcsCodes, dtcsCode) == kDtcsCodes.end()
        || std::ranges::find(kAccessModes, accessMode) == kAccessModes.end()) {
        return std::nullopt;
    }
    return std::vector<std::vector<std::uint8_t>>{
        cmdSetVfoMode(to, mode, dataMode, filter),
        cmdSetRepeaterOffset(to, offsetHz),
        cmdSetRepeaterOffsetDirection(to, direction),
        cmdSetRepeaterToneRegister(to, repeaterTone::kTxCtcss,
                                   static_cast<int>(std::lround(txToneHz * 10.0))),
        cmdSetRepeaterToneRegister(to, repeaterTone::kRxCtcss,
                                   static_cast<int>(std::lround(rxToneHz * 10.0))),
        cmdSetRepeaterToneRegister(to, repeaterTone::kDtcs, dtcsCode,
                                   dtcsTxReverse, dtcsRxReverse),
        cmdSetFunction(to, repeaterAccess::kFunction, accessMode),
    };
}

}  // namespace AetherSDR::icom
