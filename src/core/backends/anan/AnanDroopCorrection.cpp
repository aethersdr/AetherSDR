#include "core/backends/anan/AnanDroopCorrection.h"

namespace AetherSDR::anan {

namespace {

// Generated bulk data, included from this one .cpp only -- mirrors
// SpectralNR.cpp's own #include "Nr2GammaTables.inc" precedent. Regenerating
// the measured tables (tools/anan_droop_calibration.py) is then a drop-in
// file replace with zero CMakeLists.txt changes.
#include "AnanDroopCorrectionTables.inc"

constexpr DroopCorrectionTable kZeroTable{};

}  // namespace

const DroopCorrectionTable& kDroopCorrectionZero = kZeroTable;

const DroopCorrectionTable& droopCorrectionTableForRateKsps(int ddc0RateKsps) noexcept
{
    switch (ddc0RateKsps) {
    case 48:   return kDroopTable48Ksps;
    case 96:   return kDroopTable96Ksps;
    case 192:  return kDroopTable192Ksps;
    case 384:  return kDroopTable384Ksps;
    case 768:  return kDroopTable768Ksps;
    case 1536: return kDroopTable1536Ksps;
    default:   return kZeroTable;
    }
}

}  // namespace AetherSDR::anan
