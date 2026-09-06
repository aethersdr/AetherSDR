#include "core/backends/icom/IcomMemoryCodec.h"
#include "core/backends/icom/IcomModels.h"

#include <algorithm>
#include <cstdio>
#include <vector>

using namespace AetherSDR::icom;

namespace {
int failures = 0;

void check(bool condition, const char* what)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

void putFrequency(std::vector<std::uint8_t>& record, int offset)
{
    const std::vector<std::uint8_t> frequency = encodeFreq(446'125'000);
    std::copy(frequency.begin(), frequency.end(), record.begin() + offset);
}

void putName(std::vector<std::uint8_t>& record)
{
    const char name[] = "LOCAL REPEATER";
    std::copy(std::begin(name), std::end(name) - 1, record.end() - 16);
}

std::vector<std::uint8_t> ic9700Record(std::size_t size = 67)
{
    std::vector<std::uint8_t> p(size, 0);
    p[0] = 0x02; p[1] = 0x00; p[2] = 0x42; p[3] = 0x00;
    putFrequency(p, 4);
    p[9] = 0x05; p[10] = 0x02; p[11] = 0x01; p[12] = 0x21;
    p[14] = 0x00; p[15] = 0x10; p[16] = 0x00;
    p[17] = 0x00; p[18] = 0x10; p[19] = 0x00;
    p[20] = 0x11; p[21] = 0x00; p[22] = 0x23;
    const auto offset = cmdSetRepeaterOffset(0xA2, 5'000'000);
    std::copy(offset.end() - 4, offset.end() - 1, p.begin() + 24);
    putName(p);
    return p;
}

std::vector<std::uint8_t> ic705Record()
{
    std::vector<std::uint8_t> p(68, 0);
    p[0] = 0x00; p[1] = 0x12; p[2] = 0x00; p[3] = 0x42; p[4] = 0x00;
    putFrequency(p, 5);
    p[10] = 0x05; p[11] = 0x02; p[12] = 0x00; p[13] = 0x12;
    p[15] = 0x00; p[16] = 0x10; p[17] = 0x00;
    p[18] = 0x00; p[19] = 0x10; p[20] = 0x00;
    p[21] = 0x00; p[22] = 0x00; p[23] = 0x23;
    const auto offset = cmdSetRepeaterOffset(0xA4, 600'000);
    std::copy(offset.end() - 4, offset.end() - 1, p.begin() + 25);
    putName(p);
    return p;
}

std::vector<std::uint8_t> ic7300Record()
{
    // The IC-7300MK2 guide documents both 4..17 blocks in every record and a
    // separate SPLIT flag in byte 3. Live radio replies are consequently 47
    // bytes even for ordinary non-split memories.
    std::vector<std::uint8_t> p(47, 0);
    p[0] = 0x00; p[1] = 0x42; p[2] = 0x00;
    putFrequency(p, 3);
    p[8] = 0x05; p[9] = 0x02; p[10] = 0x11;
    p[11] = 0x00; p[12] = 0x10; p[13] = 0x00;
    p[14] = 0x00; p[15] = 0x10; p[16] = 0x00;
    putName(p);
    return p;
}
}  // namespace

int main()
{
    check(cmdReadMemory(0xA2, MemoryDialect::Ic9700, 3, 99)
              == std::vector<std::uint8_t>({0xFE, 0xFE, 0xA2, 0xE0, 0x1A,
                                             0x00, 0x03, 0x00, 0x99, 0xFD}),
          "IC-9700 address is band plus two-byte channel");
    check(cmdReadMemory(0xA4, MemoryDialect::Ic705, 99, 99)
              == std::vector<std::uint8_t>({0xFE, 0xFE, 0xA4, 0xE0, 0x1A,
                                             0x00, 0x00, 0x99, 0x00, 0x99, 0xFD}),
          "IC-705 address is two-byte group plus two-byte channel");
    check(cmdReadMemory(0xB6, MemoryDialect::Ic7300Mk2, -1, 99)
              == std::vector<std::uint8_t>({0xFE, 0xFE, 0xB6, 0xE0, 0x1A,
                                             0x00, 0x00, 0x99, 0xFD}),
          "IC-7300MK2 address is a two-byte channel");

    const auto m9700 = decodeMemory(MemoryDialect::Ic9700, ic9700Record());
    check(m9700 && m9700->group == 2 && m9700->channel == 42
              && m9700->mode == "DFM" && m9700->duplex == 2
              && m9700->toneMode == 1 && m9700->recallable,
          "IC-9700 official memory layout decodes");
    const auto m705 = decodeMemory(MemoryDialect::Ic705, ic705Record());
    check(m705 && m705->group == 12 && m705->channel == 42
              && m705->duplex == 1 && m705->toneMode == 2 && m705->recallable,
          "IC-705 official grouped memory layout decodes");
    const auto m7300 = decodeMemory(MemoryDialect::Ic7300Mk2, ic7300Record());
    check(m7300 && m7300->group == -1 && m7300->channel == 42
              && m7300->mode == "DFM" && m7300->toneMode == 1 && m7300->recallable,
          "IC-7300MK2 official compact memory layout decodes");
    auto ic7300SplitRecord = ic7300Record();
    ic7300SplitRecord[2] = 0x10;
    const auto m7300Split = decodeMemory(
        MemoryDialect::Ic7300Mk2, ic7300SplitRecord);
    check(m7300Split && m7300Split->split && !m7300Split->recallable,
          "IC-7300MK2 split memory remains display-only");

    auto cwRecord = ic7300Record();
    cwRecord[8] = 0x07;
    cwRecord[10] = 0x00;
    const auto cw = decodeMemory(MemoryDialect::Ic7300Mk2, cwRecord);
    check(cw && cw->mode == "CWL" && cw->recallable && !cw->split,
          "ordinary 47-byte CW-R memory is recallable on explicit sync");

    for (int toneMode = 0; toneMode <= 3; ++toneMode) {
        auto record = ic9700Record();
        record[12] = static_cast<std::uint8_t>(0x20 | toneMode);
        check(decodeMemory(MemoryDialect::Ic9700, record).has_value(),
              "IC-9700 OFF/TONE/TSQL/DTCS memory tone nibble decodes");
    }
    auto invalidTone = ic9700Record();
    invalidTone[12] = 0x26;
    check(!decodeMemory(MemoryDialect::Ic9700, invalidTone),
          "live 16 5D access values are not accepted in a memory record");

    auto reverseSplit = ic9700Record();
    reverseSplit[12] = 0x31;
    const auto rps = decodeMemory(MemoryDialect::Ic9700, reverseSplit);
    check(rps && !rps->recallable, "IC-9700 RPS remains display-only");
    const auto split = decodeMemory(MemoryDialect::Ic9700, ic9700Record(114));
    check(split && split->split && !split->recallable,
          "split records remain display-only");
    auto dvRecord = ic705Record();
    dvRecord[10] = 0x17; dvRecord[11] = 0x00;
    const auto dv = decodeMemory(MemoryDialect::Ic705, dvRecord);
    check(dv && dv->mode == "DV" && !dv->recallable,
          "DV memory remains visible without pretending it is a neutral demodulator");

    check(memoryIndex(MemoryDialect::Ic9700, 1, 1) == 0
              && memoryIndex(MemoryDialect::Ic9700, 3, 99) == 296
              && memoryIndex(MemoryDialect::Ic705, 99, 99) == 9999
              && memoryIndex(MemoryDialect::Ic7300Mk2, -1, 99) == 98,
          "each model maps its documented address space to stable cache indices");

    const IcomModel* ic705 = modelForCivAddress(0xA4);
    const IcomModel* ic7300 = modelForCivAddress(0xB6);
    const IcomModel* ic9700 = modelForCivAddress(0xA2);
    check(ic705 && profileFor(*ic705).memory
              && profileFor(*ic705).memory->requiresGroupSelection,
          "IC-705 profile requires explicit group selection");
    check(ic7300 && profileFor(*ic7300).memory
              && profileFor(*ic7300).memory->dialect == MemoryDialect::Ic7300Mk2,
          "IC-7300MK2 owns its memory dialect");
    check(ic9700 && profileFor(*ic9700).memory
              && profileFor(*ic9700).memory->dialect == MemoryDialect::Ic9700,
          "IC-9700 owns its memory dialect");

    const auto recall = buildExtendedMemoryRecallFrames(
        0xA2, CivMode::Fm, false, 2, RepeaterOffsetDirection::Up,
        600'000, 0x09, 100.0, 100.0, 23, false, false);
    check(recall && recall->size() == 7,
          "extended IC-705/IC-9700 recall preserves the ordered command plan");

    if (failures == 0) {
        std::printf("icom_memory_test: all checks passed\n");
        return 0;
    }
    return 1;
}
