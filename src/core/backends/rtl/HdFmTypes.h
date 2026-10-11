#pragma once

#include <array>
#include <cstdint>
#include <optional>

namespace AetherSDR::rtl {
struct HdFmRecipe {
    int program = 0;
    bool operator==(const HdFmRecipe&) const = default;
};
struct HdFmAudioIdentity {
    std::uint64_t sessionId = 0;
    std::uint64_t revision = 0;
    std::uint64_t audioEpoch = 1;
    std::uint64_t producedMonotonicMs = 0;
};
struct HdFmRawService {
    int program = -1;
    std::array<char, 64> name{};
    bool audioAvailable = false;
};
// Fixed-size worker/acquisition value. Qt text/model construction belongs to
// the backend owner. Publication sequence is distinct from real audio progress.
struct HdFmRawReception {
    bool valid = false;
    std::uint64_t sessionId = 0;
    std::uint64_t receiverEpoch = 0;
    std::uint64_t revision = 0;
    std::int64_t frequencyHz = 0;
    int selectedProgram = 0;
    bool synced = false;
    bool audioValid = false;
    std::array<HdFmRawService, 8> services{};
    std::array<char, 128> stationName{};
    std::array<char, 256> title{};
    std::array<char, 128> artist{};
    std::optional<double> merLowerDb;
    std::optional<double> merUpperDb;
    std::optional<double> cber;
    std::optional<double> frequencyOffsetHz;
    std::uint64_t observationMonotonicMs = 0;
    std::uint64_t audioMonotonicMs = 0;
    // Acquisition publication order is independent of the decoder measurement clock.
    std::uint64_t publicationSequence = 0;
    std::uint64_t observationSequence = 0;
    std::uint64_t audioSequence = 0;
    std::uint64_t audioEpoch = 1;
    std::uint32_t syncLossCount = 0;
    std::uint32_t reacquisitionCount = 0;
    std::uint32_t syncDurationMs = 0;
    std::uint64_t iqDrops = 0;
    std::uint64_t pcmDrops = 0;
    std::uint64_t playoutUnderruns = 0;
};
} // namespace AetherSDR::rtl
