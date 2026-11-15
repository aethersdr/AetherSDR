#pragma once

// The TX chain order reaches the audio thread as one uint64_t, so a block
// reads it with a single atomic load. Each slot holds a 4-bit stage value:
// 16 slots, and stage values 1..15 with 0 (None) ending the list. The
// ceiling therefore lands on stage types, not slots.

#include <algorithm>
#include <cstdint>
#include <type_traits>

namespace AetherSDR::TxChainLayout {

inline constexpr int kSlotBits = 4;
inline constexpr int kSlots = 64 / kSlotBits;
inline constexpr unsigned kMaxStageValue = (1u << kSlotBits) - 1;
inline constexpr uint64_t kSlotMask = kMaxStageValue;

template <typename Stage>
[[nodiscard]] constexpr Stage slot(uint64_t packed, int index) noexcept
{
    return static_cast<Stage>((packed >> (index * kSlotBits)) & kSlotMask);
}

// Packs `stages` in order and returns whether every one fit. A list longer
// than kSlots, a value above kMaxStageValue, or a None (which would end the
// list early) makes it return false; the offending entries are left out and
// the caller must report it rather than run a silently shortened chain.
template <typename Range>
[[nodiscard]] constexpr bool pack(const Range& stages, uint64_t& packed) noexcept
{
    packed = 0;
    int index = 0;
    bool fits = true;
    for (const auto stage : stages) {
        using Underlying = std::underlying_type_t<std::remove_cv_t<decltype(stage)>>;
        const auto value = static_cast<unsigned>(static_cast<Underlying>(stage));
        if (value == 0 || value > kMaxStageValue || index >= kSlots) {
            fits = false;
            continue;
        }
        packed |= static_cast<uint64_t>(value) << (index * kSlotBits);
        ++index;
    }
    return fits;
}

template <typename Container>
[[nodiscard]] Container unpack(uint64_t packed)
{
    using Stage = typename Container::value_type;
    Container out;
    for (int index = 0; index < kSlots; ++index) {
        const Stage stage = slot<Stage>(packed, index);
        if (static_cast<unsigned>(stage) == 0) {
            break;
        }
        out.push_back(stage);
    }
    return out;
}

// The first occurrence of each stage, in order. A hand-edited settings file or
// an imported preset can repeat a name; a repeat must not reach pack().
template <typename Container>
[[nodiscard]] Container withoutDuplicates(const Container& stages)
{
    Container out;
    for (const auto& stage : stages) {
        if (std::find(out.begin(), out.end(), stage) == out.end()) {
            out.push_back(stage);
        }
    }
    return out;
}

// Without `retired`: a stage that no longer exists in this chain but whose
// value a saved order may still carry.
template <typename Container, typename Stage>
[[nodiscard]] Container withoutRetired(const Container& stages, Stage retired)
{
    Container out;
    for (const auto& stage : stages) {
        if (stage != retired) {
            out.push_back(stage);
        }
    }
    return out;
}

// The stored order first, then every canonical stage it lacks, in canonical
// order: an upgrade that adds a stage shows it without reordering the rest.
template <typename Container>
[[nodiscard]] Container withMissingAppended(Container stored, const Container& canonical)
{
    for (const auto& stage : canonical) {
        if (std::find(stored.begin(), stored.end(), stage) == stored.end()) {
            stored.push_back(stage);
        }
    }
    return stored;
}

}  // namespace AetherSDR::TxChainLayout
