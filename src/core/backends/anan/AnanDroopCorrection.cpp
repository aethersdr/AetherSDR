#include "core/backends/anan/AnanDroopCorrection.h"

namespace AetherSDR::anan {

namespace {
constexpr DroopCorrectionTable kZeroTable{};
}  // namespace

const DroopCorrectionTable& kDroopCorrectionZero = kZeroTable;

}  // namespace AetherSDR::anan
