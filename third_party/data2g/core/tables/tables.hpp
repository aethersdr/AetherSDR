// Frozen data, compiled in: one header per area, each defined by its
// generated file (core/generated/<area>.cpp, tools/gen_native_tables.py).
// Include the area's header; this umbrella includes them all.
//
//   format.hpp         submode interleavers and polar info sets (on air)
//   constellation.hpp  constellation points and ACE directions (on air)
//   cpm.hpp            CPM grids, specs and parameters (on air)
//   polar_ga.hpp       polar info sets designed at run time in Python (on air)
//   ldpc.hpp           QC-LDPC base matrices (on air)
//   gamma_q99.hpp      the equalizer's noise quantiles
//   zdict.hpp          the ARQ frames' deflate dictionary (on air)
//   predictor.hpp      the gear shifter's models and tables
#pragma once

#include "tables/constellation.hpp"
#include "tables/cpm.hpp"
#include "tables/format.hpp"
#include "tables/gamma_q99.hpp"
#include "tables/ldpc.hpp"
#include "tables/polar_ga.hpp"
#include "tables/predictor.hpp"
#include "tables/zdict.hpp"
