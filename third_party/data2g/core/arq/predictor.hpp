// data2g/arq/predictor.py's live path: the MIESM features (effective MI
// over the AWGN BICM capacity tables) and the outcome model, the frozen
// bootstrap ensemble (tables::OUTCOME_MEMBERS) with the installed
// LOGIT_OFFSETS. The study toggles (DATA2G_OUTCOME_MODEL, _OUTCOME_LCB,
// _LOGIT_OFFSETS) and the link abstraction are Python only.
#pragma once

#include <array>
#include <complex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "arq/measured.hpp"
#include "tables/tables.hpp"

namespace data2g::arq {

// A submode's constellation -> the capacity table standing in for it.
std::string_view const_family(std::string_view name);
// np.interp over the family's table (clamped at the ends).
double capacity(double snr_db, std::string_view constellation);
// Mean capacity at |h|^2 / var over the channel uses (same length).
double effective_mi(std::span<const std::complex<double>> h, std::span<const double> var,
                    std::string_view constellation);

// A mode's burst peak-to-average under bandwidth cap code `cap`, dB: the
// generated-table form (tables::MODE_PEAK_DB) of arq/phy.py's peak_db, which measures it.
double peak_db(std::string_view mode, int cap = 0);
// What a sync band (OFDM band or CPM grid) occupies, Hz (predictor.band_span_hz).
std::array<double, 2> band_span_hz(std::string_view band);
// How much worse a burst in `band` should fare than the one measured in
// `measured_band`, from the noise profile, as an SNR drop (predictor.noise_shift_db).
double noise_shift_db(const std::optional<NoiseLevels>& noise, std::string_view measured_band, std::string_view band,
                      double tail_weight = 1.0, double deadband_db = 1.0);
// The measurements as if the SNR were shift_db lower (predictor.shifted).
Measured shifted(const Measured& m, double shift_db);

// The burst before the last: its measurements, band, and age (s).
struct Prev {
    Measured m;
    std::string band;
    double age = 0.0;
};

// `energy`: the energy inputs too (a model trained with --energy-inputs; zeros without any)
std::vector<double> outcome_inputs(const Measured& m, std::string_view band, double gap, double seconds,
                                   const Prev* prev = nullptr,
                                   std::span<const std::string_view> bands = tables::OUTCOME_BANDS,
                                   bool energy = false);
// The ensemble's logits (mean member probability), LOGIT_OFFSETS not applied.
// `model`: which model (GearShifter::gate's): 0 the installed one, 1 the gate's
// (tables::OUTCOME_GATE_MEMBERS), 2 the fast region's (tables::OUTCOME_FAST_MEMBERS).
std::vector<double> outcome_logits(std::span<const double> x, int model = 0);
// The same for many inputs at once: each layer's weights read once for all
// of them (a decision's ~30 predictions; the weights outsize a Pi's cache).
std::vector<std::vector<double>> outcome_logits_batch(const std::vector<std::vector<double>>& xs, int model = 0);

int outcome_index(std::string_view submode);  // its row in OUTCOME_MODES, -1: unknown
bool outcome_knows(std::string_view submode);

struct Outcome {
    double burst;  // P(burst usable)
    double cw;     // P(a codeword decodes | usable)
};
// Indexed by outcome_index, LOGIT_OFFSETS applied.
std::vector<Outcome> predict_outcome(const Measured& m, std::string_view band, double gap, double seconds,
                                     const Prev* prev = nullptr, int model = 0);
// predict_outcome for several (measurements, burst length) pairs in one batch.
struct OutcomeQuery {
    Measured m;
    double seconds;
};
std::vector<std::vector<Outcome>> predict_outcomes(const std::vector<OutcomeQuery>& qs, std::string_view band,
                                                   double gap, const Prev* prev = nullptr, int model = 0);
// One measurement, several (burst length, mode index) queries: each its
// Outcome, LOGIT_OFFSETS applied. A CNP encodes once and runs its head per
// query; MLP members run once per distinct burst length.
struct ModeQuery {
    double seconds;
    int mode;
};
std::vector<Outcome> predict_queries(const Measured& m, std::string_view band, double gap, const Prev* prev, int model,
                                     const std::vector<ModeQuery>& qs);

}  // namespace data2g::arq
