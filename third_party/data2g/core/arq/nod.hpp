// data2g/arq/nod.py: nods (docs/arq.md §3b), a CPM control-only reply sent
// as one of M keyed c16r25 tone sequences and detected, not decoded, where
// its listener expects it.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "arq/burst.hpp"
#include "cpm/cpm.hpp"

namespace data2g::arq::nod {

inline constexpr int L = 64;           // tones
inline constexpr int M = 32;           // sequences in a family; meanings past N_MEANINGS are reserved
inline constexpr int N_MEANINGS = 20;  // CODEBOOK
inline constexpr int TOPK = 4;         // the square law's best, rescored coherently
inline constexpr double THR = 37.0;    // acceptance: the best at least thr(), the runner-up under it
inline constexpr int THR_POS = 8;      // THR's starts (+-0.15 s); more raise it by TAIL per e-fold (nod.thr)
inline constexpr double TAIL = 2.08;   // the noise tail's scale (nats)
inline constexpr std::array<int, 5> REPLY_STEPS = {0, 1, -1, 2, -2};

const cpm::Grid& grid();  // c16r25

// (session key, sender's direction, the nod's burst seq mod 8, the burst it acts on mod 8)
using FamilyKey = std::array<int, 4>;
using Family = std::vector<std::array<int, L>>;  // M sequences

// The keyed family: splitmix64 seeded with the key and the turn, 16 tones per output (nod.family).
Family family(const FamilyKey& key);
// A nod's sequence -> audio, as a CPM burst's (tones, glide, the cap's TX filter, ramps).
std::vector<double> waveform(std::span<const int, L> seq, int cap = 2);
inline std::int64_t span() { return static_cast<std::int64_t>(L) * grid().T; }  // samples a nod takes

struct Detection {
    int meaning;
    std::int64_t start;
    double score;
};
// A nod of `fam` starting in [lo, hi] (x's samples; x holds hi + span()) at CFO cfo, or nullopt: none
// over thr(), a runner-up over it too, or a reserved meaning.
// The acceptance threshold for a search over n_pos starts (nod.thr): 1e-6 false accepts per slot.
double thr(std::int64_t n_pos);
std::optional<Detection> detect(std::span<const double> x, std::int64_t lo, std::int64_t hi, double cfo,
                                const Family& fam);

// The codebook: meaning -> (ack all (else unchanged), recommend step, T_DUPCTL).
struct Meaning {
    bool all;
    int step;
    bool dup;
};
Meaning meaning(int index);
std::optional<int> index(bool all, int step, bool dup);
std::string describe(int index);  // as Python prints CODEBOOK[i]: ('all', 0, 1)

// A detected nod as a received control-only burst: its control is the one the listener synthesized
// from the meaning (link Station::nod_payload), or none. No primed or repeat decode.
class NodRx : public RxBurst {
public:
    NodRx(std::string submode, MaskId mask, std::optional<Bytes> payload)
        : submode_(std::move(submode)), mask_(mask), payload_(std::move(payload)) {}
    const std::string& submode() const override { return submode_; }
    int n_cw() const override { return 1; }
    std::optional<Bytes> decode(int slot, const MaskId& mask, int, const SoftKey*) override {
        return slot == 0 && mask == mask_ ? payload_ : std::nullopt;
    }
    void forget(const SoftKey&) override {}
    bool is_nod() const override { return true; }

private:
    std::string submode_;
    MaskId mask_;
    std::optional<Bytes> payload_;
};

}  // namespace data2g::arq::nod
