#include "core/backends/icom/IcomMemoryCodec.h"

#include <array>
#include <cctype>
#include <cmath>
#include <ranges>

namespace AetherSDR::icom {
namespace {

constexpr std::array<int, 104> kDtcsCodes{{
    23, 25, 26, 31, 32, 36, 43, 47, 51, 53, 54, 65, 71, 72, 73, 74,
    114, 115, 116, 122, 125, 131, 132, 134, 143, 145, 152, 155, 156, 162,
    165, 172, 174, 205, 212, 223, 225, 226, 243, 244, 245, 246, 251, 252,
    255, 261, 263, 265, 266, 271, 274, 306, 311, 315, 325, 331, 332, 343,
    346, 351, 356, 364, 365, 371, 411, 412, 413, 423, 431, 432, 445, 446,
    452, 454, 455, 462, 464, 465, 466, 503, 506, 516, 523, 526, 532, 546,
    565, 606, 612, 624, 627, 631, 632, 654, 662, 664, 703, 712, 723, 731,
    732, 734, 743, 754,
}};

struct Layout {
    int addressBytes, groupBytes, firstGroup, lastGroup, firstChannel, lastChannel;
    int singleBytes, splitBytes, selectOffset, frequencyOffset, modeOffset;
    int filterOffset, dataOffset, accessOffset, txToneOffset, rxToneOffset;
    int dtcsOffset, offsetOffset;
};

constexpr Layout layoutFor(MemoryDialect dialect)
{
    switch (dialect) {
    case MemoryDialect::Ic705:
        return {4, 2, 0, 99, 0, 99, 68, 115, 4, 5, 10, 11, 12, 13,
                15, 18, 21, 25};
    case MemoryDialect::Ic7300Mk2:
        return {2, 0, -1, -1, 1, 99, 33, 47, 2, 3, 8, 9, 10, 10,
                11, 14, -1, -1};
    case MemoryDialect::Ic9700:
        return {3, 1, 1, 3, 1, 99, 67, 114, 3, 4, 9, 10, 11, 12,
                14, 17, 20, 24};
    }
    return {};
}

bool validBcd(std::uint8_t value) noexcept
{
    return (value & 0x0f) <= 9 && ((value >> 4) & 0x0f) <= 9;
}

std::optional<int> decodeBcdNumber(std::span<const std::uint8_t> bytes)
{
    int value = 0;
    for (const std::uint8_t byte : bytes) {
        if (!validBcd(byte)) {
            return std::nullopt;
        }
        value = value * 100 + decodeBcdByte(byte);
    }
    return value;
}

void appendBcdNumber(std::vector<std::uint8_t>& out, int value, int bytes)
{
    std::vector<std::uint8_t> encoded(static_cast<std::size_t>(bytes));
    for (int i = bytes - 1; i >= 0; --i) {
        encoded[static_cast<std::size_t>(i)] = encodeBcdByte(value % 100);
        value /= 100;
    }
    out.insert(out.end(), encoded.begin(), encoded.end());
}

std::string decodeName(std::span<const std::uint8_t> bytes)
{
    std::string name;
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

struct DecodedMode { std::string name; std::optional<CivMode> mode; };

DecodedMode decodeMode(std::uint8_t value, bool dataMode)
{
    std::optional<CivMode> mode;
    switch (value) {
    case 0x00: mode = CivMode::Lsb; break;
    case 0x01: mode = CivMode::Usb; break;
    case 0x02: mode = CivMode::Am; break;
    case 0x03: mode = CivMode::Cw; break;
    case 0x04: mode = CivMode::Rtty; break;
    case 0x05: mode = CivMode::Fm; break;
    case 0x07: mode = CivMode::CwR; break;
    case 0x08: mode = CivMode::RttyR; break;
    case 0x17: return {"DV", std::nullopt};
    case 0x22: return {"DD", std::nullopt};
    default: return {};
    }
    return {modeToNeutral(*mode, dataMode), mode};
}

}  // namespace

std::vector<std::uint8_t> cmdReadMemory(
    std::uint8_t to, MemoryDialect dialect, int group, int channel)
{
    const Layout layout = layoutFor(dialect);
    if (channel < layout.firstChannel || channel > layout.lastChannel
        || (layout.groupBytes > 0
            && (group < layout.firstGroup || group > layout.lastGroup))) {
        return {};
    }
    std::vector<std::uint8_t> address;
    if (layout.groupBytes > 0) {
        appendBcdNumber(address, group, layout.groupBytes);
    }
    appendBcdNumber(address, channel, 2);
    return buildFrameSub(to, cmd::kSetting, 0x00, address);
}

std::optional<IcomMemoryChannel> decodeMemory(
    MemoryDialect dialect, std::span<const std::uint8_t> payload)
{
    const Layout layout = layoutFor(dialect);
    if (payload.size() < static_cast<std::size_t>(layout.addressBytes)) {
        return std::nullopt;
    }
    int cursor = 0;
    int group = -1;
    if (layout.groupBytes > 0) {
        const std::optional<int> decoded = decodeBcdNumber(
            payload.subspan(0, static_cast<std::size_t>(layout.groupBytes)));
        if (!decoded || *decoded < layout.firstGroup || *decoded > layout.lastGroup) {
            return std::nullopt;
        }
        group = *decoded;
        cursor += layout.groupBytes;
    }
    const std::optional<int> channel = decodeBcdNumber(payload.subspan(cursor, 2));
    if (!channel || *channel < layout.firstChannel || *channel > layout.lastChannel) {
        return std::nullopt;
    }

    IcomMemoryChannel memory;
    memory.group = group;
    memory.channel = *channel;
    if (payload.size() == static_cast<std::size_t>(layout.addressBytes + 1)
        && payload[static_cast<std::size_t>(layout.addressBytes)] == 0xff) {
        return memory;
    }
    if (payload.size() != static_cast<std::size_t>(layout.singleBytes)
        && payload.size() != static_cast<std::size_t>(layout.splitBytes)) {
        return std::nullopt;
    }

    // The IC-7300MK2 always returns the second 4..17 block documented by its
    // CI-V guide, even when Split is OFF.  Its byte-3 SPLIT flag, decoded
    // below, is therefore the authority; treating the 47-byte reply length as
    // Split made every live channel display-only and prevented spot recall.
    // The older dialects retain their established variable-length contract.
    memory.split = dialect != MemoryDialect::Ic7300Mk2
        && payload.size() == static_cast<std::size_t>(layout.splitBytes);
    const std::optional<std::uint64_t> frequency = decodeFreqExact(
        payload.subspan(static_cast<std::size_t>(layout.frequencyOffset), kFreqBytes),
        kFreqBytes);
    const std::uint8_t dataByte = payload[static_cast<std::size_t>(layout.dataOffset)];
    const int dataNibble = (dataByte >> 4) & 0x0f;
    if ((dialect == MemoryDialect::Ic7300Mk2 && dataNibble > 1)
        || (dialect != MemoryDialect::Ic7300Mk2 && dataByte > 1)) {
        return std::nullopt;
    }
    const bool dataMode = dialect == MemoryDialect::Ic7300Mk2
        ? dataNibble != 0 : dataByte == 0x01;
    const DecodedMode mode = decodeMode(
        payload[static_cast<std::size_t>(layout.modeOffset)], dataMode);
    const std::uint8_t filterByte = payload[static_cast<std::size_t>(layout.filterOffset)];
    const int filter = validBcd(filterByte) ? decodeBcdByte(filterByte) : 0;
    if (!frequency || *frequency == 0 || mode.name.empty()
        || (mode.mode && (filter < 1 || filter > 3))) {
        return std::nullopt;
    }

    const std::uint8_t accessByte = payload[static_cast<std::size_t>(layout.accessOffset)];
    const int duplex = dialect == MemoryDialect::Ic7300Mk2 ? 0 : (accessByte >> 4) & 0x0f;
    const int toneMode = accessByte & 0x0f;
    if (duplex > 3 || toneMode > 3
        || (dialect == MemoryDialect::Ic7300Mk2 && toneMode > 2)) {
        return std::nullopt;
    }
    const std::optional<double> txTone = decodeRepeaterToneHz(
        payload.subspan(static_cast<std::size_t>(layout.txToneOffset), 3));
    const std::optional<double> rxTone = decodeRepeaterToneHz(
        payload.subspan(static_cast<std::size_t>(layout.rxToneOffset), 3));
    if (!txTone || !rxTone) {
        return std::nullopt;
    }

    memory.occupied = true;
    memory.frequencyHz = *frequency;
    memory.mode = mode.name;
    memory.filter = filter;
    memory.dataMode = dataMode ? 1 : 0;
    memory.duplex = duplex;
    memory.toneMode = toneMode;
    memory.txToneHz = *txTone;
    memory.rxToneHz = *rxTone;
    const std::uint8_t selectByte = payload[static_cast<std::size_t>(layout.selectOffset)];
    if (dialect == MemoryDialect::Ic9700) {
        if (!validBcd(selectByte) || decodeBcdByte(selectByte) > 3) {
            return std::nullopt;
        }
    } else {
        if (((selectByte >> 4) & 0x0f) > 1 || (selectByte & 0x0f) > 3) {
            return std::nullopt;
        }
        memory.split = memory.split || ((selectByte >> 4) != 0);
    }
    // Neutral recall cannot represent the second frequency block or RPS.
    // Keep those records visible but non-recallable until their complete
    // semantics are implemented. A fixed-length IC-7300MK2 reply alone is
    // not split: the explicit flag above remains authoritative.
    memory.recallable = mode.mode.has_value() && !memory.split && duplex != 3;

    if (layout.dtcsOffset >= 0) {
        const std::optional<RepeaterToneRegister> dtcs = decodeRepeaterToneRegister(
            payload.subspan(static_cast<std::size_t>(layout.dtcsOffset), 3));
        const std::optional<int> offset = decodeRepeaterOffsetHz(
            payload.subspan(static_cast<std::size_t>(layout.offsetOffset), 3));
        if (!dtcs || !offset
            || std::ranges::find(kDtcsCodes, dtcs->value) == kDtcsCodes.end()) {
            return std::nullopt;
        }
        memory.dtcsCode = dtcs->value;
        memory.dtcsTxReverse = dtcs->txReverse;
        memory.dtcsRxReverse = dtcs->rxReverse;
        memory.offsetHz = *offset;
    }
    memory.name = decodeName(payload.last(16));
    return memory;
}

int memoryIndex(MemoryDialect dialect, int group, int channel) noexcept
{
    const Layout layout = layoutFor(dialect);
    if (channel < layout.firstChannel || channel > layout.lastChannel) {
        return -1;
    }
    if (layout.groupBytes == 0) {
        return channel - layout.firstChannel;
    }
    if (group < layout.firstGroup || group > layout.lastGroup) {
        return -1;
    }
    return (group - layout.firstGroup) * (layout.lastChannel - layout.firstChannel + 1)
        + channel - layout.firstChannel;
}

std::string memoryGroupName(MemoryDialect dialect, int group)
{
    if (dialect == MemoryDialect::Ic9700) {
        static constexpr std::array<std::string_view, 3> names{
            "144 MHz", "430 MHz", "1.2 GHz"};
        if (group >= 1 && group <= 3) {
            return std::string(names[static_cast<std::size_t>(group - 1)]);
        }
    }
    if (dialect == MemoryDialect::Ic705 && group >= 0 && group <= 99) {
        std::string name(2, '0');
        name[0] = static_cast<char>('0' + group / 10);
        name[1] = static_cast<char>('0' + group % 10);
        return name;
    }
    return "Memories";
}

std::optional<std::vector<std::vector<std::uint8_t>>> buildExtendedMemoryRecallFrames(
    std::uint8_t to, CivMode mode, bool dataMode, int filter,
    RepeaterOffsetDirection direction, int offsetHz, std::uint8_t accessMode,
    double txToneHz, double rxToneHz, int dtcsCode,
    bool dtcsTxReverse, bool dtcsRxReverse)
{
    static constexpr std::array<std::uint8_t, 8> accessModes{{0, 1, 2, 3, 6, 7, 8, 9}};
    if (filter < 1 || filter > 3 || offsetHz < 0 || offsetHz > 99'999'900
        || !std::isfinite(txToneHz) || txToneHz < 0.0 || txToneHz > 299.9
        || !std::isfinite(rxToneHz) || rxToneHz < 0.0 || rxToneHz > 299.9
        || std::ranges::find(kDtcsCodes, dtcsCode) == kDtcsCodes.end()
        || std::ranges::find(accessModes, accessMode) == accessModes.end()) {
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
