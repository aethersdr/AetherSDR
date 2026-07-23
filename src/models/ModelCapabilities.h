#pragma once

#include <QString>

namespace AetherSDR {

// Hardware platform generations, mirroring FlexLib/ModelInfo.cs's
// RadioPlatform enum (Principle I — FlexLib is the protocol/model
// authority).  The extended firmware DSP filters (NRL/NRS/RNN/NRF) are a
// BigBend + DragonFire capability; the earlier Microburst/DeepEddy
// 6000-series platforms don't expose them, which is why the UI hides
// those toggles there. (#2177)
enum class RadioPlatform {
    Unknown,     // model not yet reported, or not in the table
    Microburst,  // FLEX-6300 / 6500 / 6700 / 6700R
    DeepEddy,    // FLEX-6400(M) / 6600(M)
    BigBend,     // FLEX-8400(M) / 8600(M), ML-/MLS-/CL-/CLS-/AU- series
    DragonFire,  // RT-2122
};

// Per-model feature flags mirroring FlexLib/ModelInfo.cs (Platform,
// Has2Meters, Has4Meters, HasLoopA, HasLoopB, ...).  Authority for any UI
// surface that varies by model capability — the band selector in the
// Spectrum Overlay Band menu (#695) and the per-slice extended-DSP
// toggles (#2177) are consumers; additional flags should be added here as
// more `model.contains("6700")`-style checks get migrated.
struct ModelCapabilities {
    RadioPlatform platform{RadioPlatform::Unknown};
    bool has4Meters{false};  // Built-in 70 MHz transverter (FLEX-6500 Region 1, FLEX-6700)
    bool has2Meters{false};  // Built-in 144 MHz transverter (FLEX-6700)
    bool hasLoopA{false};    // RX loop/preselector path (FLEX-6500, FLEX-6700)
    bool hasLoopB{false};    // Second RX loop path (FLEX-6700)
    bool isDiversityAllowed{false};  // 2-SCU diversity RX (FlexLib IsDiversityAllowed)
    // FLEX-8000 hardware manual §25.2: the integral GNSS receiver provides a
    // Stratum 1 NTP server. GPSDO-equipped 6000-series radios do not.
    bool hasNtpServer{false};
    // Max independent receivers (FlexLib SliceList size).  Also the max
    // panadapter count — pan capacity tracks the radio's SCU/slice capacity,
    // which is identical across every current model.  Default 2 mirrors
    // FlexLib's DEFAULT entry (SliceList {A,B}); the radio's live "slices=N"
    // status overrides this initial estimate once connected.
    int maxSlices{2};

    // Extended firmware DSP filters (NRL / NRS / RNN / NRF) exist on the
    // BigBend and DragonFire platforms.  Unknown (pre-discovery) and the
    // Microburst/DeepEddy 6000-series resolve to false so the toggles stay
    // hidden rather than showing controls the firmware would no-op. (#2177)
    bool hasExtendedDsp() const {
        return platform == RadioPlatform::BigBend
            || platform == RadioPlatform::DragonFire;
    }
};

// Returns capabilities for the given model string.  Uses substring match
// (case-insensitive) so vendor suffixes like "FLEX-6700/A" and the M
// variants ("AU-510M") still resolve to their family entry.  Unknown
// models default to Unknown platform + all-false flags — forward-
// compatible for radios released after this build.
ModelCapabilities capabilitiesFor(const QString& model);

} // namespace AetherSDR
