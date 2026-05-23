#pragma once

#include <array>
#include <cstddef>

namespace MasterSDR {

struct BandDef {
    const char* name;        // "20m", "40m", "GEN", "WWV"
    double lowMhz;           // inclusive lower edge
    double highMhz;          // inclusive upper edge
    double defaultFreqMhz;   // default tune frequency
    const char* defaultMode; // "USB", "LSB", "CW", "AM"
};

// Amateur radio bands (ARRL band plan, US allocation)
inline constexpr BandDef kBands[] = {
    {"2200m",  0.1357,   0.1378,   0.1375,  "CW"},
    {"630m",   0.472,    0.479,    0.475,   "CW"},
    {"160m",   1.800,    2.000,    1.900,   "LSB"},
    {"80m",    3.500,    4.000,    3.800,   "LSB"},
    {"60m",    5.330,    5.405,    5.357,   "USB"},
    {"40m",    7.000,    7.300,    7.200,   "LSB"},
    {"30m",   10.100,   10.150,   10.125,   "DIGU"},
    {"20m",   14.000,   14.350,   14.225,   "USB"},
    {"17m",   18.068,   18.168,   18.130,   "USB"},
    {"15m",   21.000,   21.450,   21.300,   "USB"},
    {"12m",   24.890,   24.990,   24.950,   "USB"},
    {"10m",   28.000,   29.700,   28.400,   "USB"},
    {"6m",    50.000,   54.000,   50.150,   "USB"},
    // VHF/UHF — for XVTR band stack matching (#346, #695)
    {"4m",    70.000,   70.500,   70.200,   "USB"},
    {"2m",   144.000,  148.000,  144.200,   "USB"},
    {"1.25m",222.000,  225.000,  222.100,   "USB"},
    {"440",  420.000,  450.000,  432.100,   "FM"},
    {"33cm", 902.000,  928.000,  903.100,   "FM"},
    {"23cm",1240.000, 1300.000, 1296.100,   "USB"},
};

inline constexpr int kBandCount = static_cast<int>(std::size(kBands));

// Special bands — not auto-detected from frequency
inline constexpr BandDef kWwvBand = {"WWV", 0.0, 0.0, 10.000, "AM"};
inline constexpr BandDef kGenBand = {"GEN", 0.0, 54.0, 0.500, "AM"};

} // namespace MasterSDR
