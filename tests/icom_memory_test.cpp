#include "core/backends/icom/CivCodec.h"
#include "core/backends/icom/IcomMemoryCodec.h"

#include <algorithm>
#include <cstdio>
#include <vector>

using namespace AetherSDR::icom;

namespace {
int g_failures = 0;

void check(bool condition, const char* what)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

std::vector<std::uint8_t> occupiedRecord(std::size_t size)
{
    std::vector<std::uint8_t> p(size, 0);
    p[0] = 0x02;
    p[1] = 0x00;
    p[2] = 0x42;
    p[3] = 0x01;
    const std::vector<std::uint8_t> frequency = encodeFreq(446'125'000);
    std::copy(frequency.begin(), frequency.end(), p.begin() + 4);
    p[9] = 0x05;
    p[10] = 0x02;
    p[11] = 0x01;
    p[12] = 0x21; // DUP+, TX CTCSS
    p[13] = 0x00;
    p[14] = 0x00; p[15] = 0x10; p[16] = 0x00; // 100.0 Hz
    p[17] = 0x00; p[18] = 0x10; p[19] = 0x00;
    p[20] = 0x11; p[21] = 0x00; p[22] = 0x23;
    const std::vector<std::uint8_t> offsetFrame =
        cmdSetRepeaterOffset(0xA2, 5'000'000);
    std::copy(offsetFrame.end() - 4, offsetFrame.end() - 1, p.begin() + 24);
    const char name[] = "LOCAL REPEATER";
    std::copy(std::begin(name), std::end(name) - 1, p.begin() + size - 16);
    return p;
}
}  // namespace

int main()
{
    const std::vector<std::uint8_t> query = cmdReadIc9700Memory(0xA2, 3, 99);
    check(query == std::vector<std::uint8_t>({
              0xFE, 0xFE, 0xA2, 0xE0, 0x1A, 0x00, 0x03, 0x00, 0x99, 0xFD}),
          "read command carries IC-9700 band and channel address");
    check(cmdReadIc9700Memory(0xA2, 0, 1).empty()
              && cmdReadIc9700Memory(0xA2, 1, 100).empty(),
          "out-of-range slots are refused at the codec boundary");

    const auto empty = decodeIc9700Memory(std::vector<std::uint8_t>{0x01, 0x00, 0x07, 0xFF});
    check(empty && !empty->occupied && empty->band == 1 && empty->channel == 7,
          "short FF record removes an unused channel");

    for (const std::size_t size : {std::size_t{67}, std::size_t{114}}) {
        const auto decoded = decodeIc9700Memory(occupiedRecord(size));
        check(decoded && decoded->occupied, "occupied record decodes");
        check(decoded && decoded->band == 2 && decoded->channel == 42,
              "native group and channel survive decode");
        check(decoded && decoded->frequencyHz == 446'125'000 && decoded->mode == "FM",
              "native frequency and mode decode without folding in data mode");
        check(decoded && decoded->dataMode == 1,
              "data mode remains an independent native memory field");
        check(decoded && decoded->duplex == 2 && decoded->offsetHz == 5'000'000,
              "duplex direction and offset decode");
        check(decoded && decoded->toneMode == 1 && decoded->txToneHz == 100.0,
              "TX CTCSS state decodes");
        check(decoded && decoded->dtcsCode == 23
                  && decoded->dtcsTxReverse && decoded->dtcsRxReverse,
              "DTCS code and independent polarity decode from the memory record");
        check(decoded && decoded->name == "LOCAL REPEATER",
              "name is read from the final fixed-width field");
    }

    for (const int toneMode : {0, 1, 2, 3, 6, 7, 8, 9}) {
        std::vector<std::uint8_t> record = occupiedRecord(67);
        record[12] = static_cast<std::uint8_t>(0x20 | toneMode);
        check(decodeIc9700Memory(record).has_value(),
              "every documented IC-9700 memory access mode decodes");
    }
    std::vector<std::uint8_t> reservedToneMode = occupiedRecord(67);
    reservedToneMode[12] = 0x24;
    check(!decodeIc9700Memory(reservedToneMode),
          "reserved IC-9700 memory access mode is rejected");

    const auto recall = buildIc9700MemoryRecallFrames(
        0xA2, CivMode::Am, true, 3, RepeaterOffsetDirection::Up,
        600'000, 0x08, 100.0, 123.0, 23, true, false);
    check(recall && recall->size() == 7, "native recall produces one ordered command plan");
    std::vector<CivFrame> recallFrames;
    for (const auto& bytes : recall.value_or(std::vector<std::vector<std::uint8_t>>{})) {
        const auto frame = parseFrame(bytes);
        check(frame.has_value(), "every native recall command is a valid CI-V frame");
        if (frame) {
            recallFrames.push_back(*frame);
        }
    }
    check(recallFrames.size() == 7
              && recallFrames[0].cmd == cmd::kVfoMode
              && recallFrames[0].data == std::vector<std::uint8_t>({0x02, 0x01, 0x03}),
          "native recall preserves AM DATA mode and FIL3 atomically");
    check(recallFrames.size() == 7
              && recallFrames[3].cmd == cmd::kTone && recallFrames[3].sub == 0x00
              && recallFrames[4].cmd == cmd::kTone && recallFrames[4].sub == 0x01
              && recallFrames[5].cmd == cmd::kTone && recallFrames[5].sub == 0x02,
          "native recall writes independent TX, RX, and DTCS registers");
    check(recallFrames.size() == 7
              && recallFrames.back().cmd == cmd::kFunction
              && recallFrames.back().sub == repeaterAccess::kFunction
              && recallFrames.back().data == std::vector<std::uint8_t>({0x08}),
          "native recall enables the access mode last");
    check(!buildIc9700MemoryRecallFrames(
              0xA2, CivMode::Fm, false, 0, RepeaterOffsetDirection::Simplex,
              0, 0x00, 100.0, 100.0, 23, false, false),
          "native recall rejects an invalid filter preset");
    check(!buildIc9700MemoryRecallFrames(
              0xA2, CivMode::Fm, false, 1, RepeaterOffsetDirection::Simplex,
              0, 0x04, 100.0, 100.0, 23, false, false),
          "native recall rejects a reserved access mode");
    check(!buildIc9700MemoryRecallFrames(
              0xA2, CivMode::Fm, false, 1, RepeaterOffsetDirection::Simplex,
              0, 0x00, 100.0, 100.0, 24, false, false),
          "native recall rejects a non-standard DTCS code");
    check(!buildIc9700MemoryRecallFrames(
              0xA2, CivMode::Fm, false, 1, RepeaterOffsetDirection::Simplex,
              0, 0x00, 300.0, 100.0, 23, false, false),
          "native recall rejects an out-of-range CTCSS tone");

    std::vector<std::uint8_t> corrupt = occupiedRecord(67);
    corrupt[4] = 0xFA;
    check(!decodeIc9700Memory(corrupt), "invalid frequency BCD is rejected");
    corrupt = occupiedRecord(67);
    corrupt.resize(68);
    check(!decodeIc9700Memory(corrupt), "unknown record length is rejected");
    check(ic9700MemoryIndex(1, 1) == 0 && ic9700MemoryIndex(3, 99) == 296,
          "three native groups map to stable neutral indices");

    if (g_failures == 0) {
        std::printf("icom_memory_test: all checks passed\n");
        return 0;
    }
    return 1;
}
