// The gear shifter's models and tables (data2g/arq/predictor.py, policy.py,
// phy.py peak_db), compiled in: generated/predictor.cpp. Not on air.
#pragma once

#include <array>
#include <span>
#include <string_view>

namespace data2g::tables {

// The outcome model: bootstrap members, each an MLP (x - mean) / std ->
// tanh layers -> logits, W row-major (in, out), float32 (inputs scaled in
// double, then float). Outputs: P(burst usable) per OUTCOME_MODES, then P(codeword).
struct MlpLayer {
    int in, out;
    std::span<const float> W, b;
};
struct OutcomeMember {
    std::span<const double> mean, std;
    std::span<const MlpLayer> layers;
};
extern const std::span<const OutcomeMember> OUTCOME_MEMBERS;
// Or the installed model is a CNP (predictor.OutcomeCNP, scripts/cnp_outcome.py):
// an encoder of the inputs without log2(seconds) (column secs_col), scaled by
// mean/std, tanh after every layer; and a head on [encoding, the mode's
// descriptor row, log2(seconds)], tanh between layers, 2 logits (P(usable),
// P(codeword)). OUTCOME_MEMBERS is then empty.
struct CnpModel {
    std::span<const double> mean, std;
    std::span<const MlpLayer> enc, head;
    std::span<const float> desc;  // (OUTCOME_MODES, n_desc), normalized
    int n_desc, secs_col;
};
extern const bool OUTCOME_IS_CNP;
extern const CnpModel OUTCOME_CNP;
// The gate's model (predictor.GATE_MODEL), its outputs in OUTCOME_MODES' order,
// and the gate (policy.GearShifter.gate): median spread_est of the last
// OUTCOME_GATE_HIST peer bursts under SPREAD_HZ and snr_est under SNR_DB.
extern const std::span<const OutcomeMember> OUTCOME_GATE_MEMBERS;
extern const double OUTCOME_GATE_SPREAD_HZ, OUTCOME_GATE_SNR_DB;
extern const int OUTCOME_GATE_HIST;
// The fast region's model (predictor.FAST_MODEL), in OUTCOME_MODES' order: median
// spread_est at least FAST_SPREAD_HZ and snr_est in [FAST_SNR_LO_DB, FAST_SNR_HI_DB).
extern const std::span<const OutcomeMember> OUTCOME_FAST_MEMBERS;
extern const double OUTCOME_FAST_SPREAD_HZ, OUTCOME_FAST_SNR_LO_DB, OUTCOME_FAST_SNR_HI_DB;
// whether each model takes the energy inputs (predictor.N_ENERGY)
extern const bool OUTCOME_ENERGY, OUTCOME_GATE_ENERGY, OUTCOME_FAST_ENERGY;
// each mode's burst peak-to-average (arq/phy.py peak_db) by bandwidth cap
// code: the energy inputs' peak reference
struct ModePeak {
    std::string_view mode;
    double db[3];
};
extern const std::span<const ModePeak> MODE_PEAK_DB;
extern const std::span<const std::string_view> OUTCOME_MODES, OUTCOME_BANDS;
// The data ladder's robustness order (data2g/arq/policy.py MODE_THRESHOLDS):
// per mode, its 10% codeword failure SNR (dB) on awgn, mpg, mpp, mpd.
struct ModeThreshold {
    std::string_view name;
    std::array<double, 4> db;
};
extern const std::span<const ModeThreshold> MODE_THRESHOLDS;
// AWGN BICM capacity (bits per coded bit) over CAPACITY_GRID (dB), per
// constellation family (predictor.CONSTS order).
struct CapacityTable {
    std::string_view name;
    std::span<const double> mi;
};
extern const std::span<const double> CAPACITY_GRID;
extern const std::span<const CapacityTable> CAPACITY;

}  // namespace data2g::tables
