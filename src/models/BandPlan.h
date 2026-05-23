#pragma once

#include <QColor>

namespace MasterSDR {

// ARRL US Amateur Radio Band Plan
// Sources: ARRL band chart (rev. 1/16/2026) + Considerate Operator's Frequency Guide
//
// Color key (ARRL):
//   Blue    = CW only
//   Red     = RTTY/Data
//   Orange  = Phone/image
//   Green   = SSB phone
//   Yellow  = USB phone, CW, RTTY, data (all modes)
//   Purple  = Satellite
//   Cyan    = Beacons
//
// License classes (active only): E=Extra, G=General, T=Technician

struct BandSegment {
    double lowMhz;
    double highMhz;
    const char* label;
    const char* license; // "E", "E,G", "E,G,T", "T", "" = beacon/no TX
    int r, g, b;
};

// ARRL colors
//   CW only:              blue    (0x30, 0x60, 0xff)
//   RTTY/data:            red     (0xc0, 0x30, 0x30)
//   Phone/image:          orange  (0xff, 0x80, 0x00)
//   SSB phone:            green   (0x30, 0xb0, 0x30)
//   USB/CW/RTTY/data:     yellow  (0xff, 0xd0, 0x00)
//   Satellite:            purple  (0x90, 0x30, 0xc0)
//   Beacon:               cyan    (0x00, 0xd0, 0xd0)

inline constexpr BandSegment kBandPlan[] = {
    // 2200m (135 kHz)
    {0.1357, 0.1378, "CW",         "E,G",    0x30, 0x60, 0xff},

    // 630m (472 kHz)
    {0.472,  0.479,  "CW",         "E,G",    0x30, 0x60, 0xff},

    // 160m (1.800 - 2.000)
    {1.800,  1.810,  "DATA",       "E,G",    0xc0, 0x30, 0x30},
    {1.810,  1.843,  "CW",         "E,G",    0x30, 0x60, 0xff},
    {1.843,  1.995,  "SSB",        "E,G",    0xff, 0x80, 0x00},
    {1.995,  1.999,  "EXPER",      "E,G",    0xff, 0xd0, 0x00},
    {1.999,  2.000,  "BCN",        "",        0x00, 0xd0, 0xd0},

    // 80m (3.500 - 4.000)
    {3.500,  3.510,  "CW DX",      "E",      0x30, 0x60, 0xff},
    {3.510,  3.570,  "CW",         "E,G",    0x30, 0x60, 0xff},
    {3.570,  3.600,  "DATA",       "E,G",    0xc0, 0x30, 0x30},
    {3.600,  3.800,  "PHONE",      "E",      0xff, 0x80, 0x00},
    {3.800,  4.000,  "PHONE",      "E,G",    0xff, 0x80, 0x00},

    // 60m (5.330 - 5.405) — channelized, USB/CW/data
    {5.3305, 5.3333, "USB",        "E,G",    0xff, 0xd0, 0x00},
    {5.3465, 5.3493, "USB",        "E,G",    0xff, 0xd0, 0x00},
    {5.3515, 5.3543, "USB",        "E,G",    0xff, 0xd0, 0x00},
    {5.3570, 5.3598, "USB",        "E,G",    0xff, 0xd0, 0x00},
    {5.3715, 5.3743, "USB",        "E,G",    0xff, 0xd0, 0x00},
    {5.4035, 5.4063, "USB",        "E,G",    0xff, 0xd0, 0x00},

    // 40m (7.000 - 7.300)
    {7.000,  7.025,  "CW",         "E",      0x30, 0x60, 0xff},
    {7.025,  7.040,  "CW",         "E,G",    0x30, 0x60, 0xff},
    {7.040,  7.070,  "DATA DX",    "E,G",    0xc0, 0x30, 0x30},
    {7.070,  7.125,  "DATA",       "E,G",    0xc0, 0x30, 0x30},
    {7.125,  7.175,  "PHONE",      "E",      0xff, 0x80, 0x00},
    {7.175,  7.290,  "PHONE",      "E,G",    0xff, 0x80, 0x00},
    {7.290,  7.300,  "AM",         "E,G",    0xff, 0x80, 0x00},

    // 30m (10.100 - 10.150) — CW/data only, 200W PEP
    {10.100, 10.130, "CW",         "E,G",    0x30, 0x60, 0xff},
    {10.130, 10.140, "DATA",       "E,G",    0xc0, 0x30, 0x30},
    {10.140, 10.150, "AUTO",       "E,G",    0xc0, 0x30, 0x30},

    // 20m (14.000 - 14.350)
    {14.000, 14.025, "CW",         "E",      0x30, 0x60, 0xff},
    {14.025, 14.070, "CW",         "E,G",    0x30, 0x60, 0xff},
    {14.070, 14.095, "DATA",       "E,G",    0xc0, 0x30, 0x30},
    {14.095, 14.100, "AUTO",       "E,G",    0xc0, 0x30, 0x30},
    {14.100, 14.101, "BCN",        "",        0x00, 0xd0, 0xd0},
    {14.101, 14.150, "CW",         "E,G",    0x30, 0x60, 0xff},
    {14.150, 14.225, "PHONE",      "E",      0xff, 0x80, 0x00},
    {14.225, 14.350, "PHONE",      "E,G",    0xff, 0x80, 0x00},

    // 17m (18.068 - 18.168)
    {18.068, 18.100, "CW",         "E,G",    0x30, 0x60, 0xff},
    {18.100, 18.105, "DATA",       "E,G",    0xc0, 0x30, 0x30},
    {18.105, 18.110, "AUTO",       "E,G",    0xc0, 0x30, 0x30},
    {18.110, 18.111, "BCN",        "",        0x00, 0xd0, 0xd0},
    {18.111, 18.168, "PHONE",      "E,G",    0xff, 0x80, 0x00},

    // 15m (21.000 - 21.450)
    {21.000, 21.025, "CW",         "E",      0x30, 0x60, 0xff},
    {21.025, 21.070, "CW",         "E,G",    0x30, 0x60, 0xff},
    {21.070, 21.090, "DATA",       "E,G",    0xc0, 0x30, 0x30},
    {21.090, 21.100, "AUTO",       "E,G",    0xc0, 0x30, 0x30},
    {21.100, 21.150, "DATA",       "E,G",    0xc0, 0x30, 0x30},
    {21.150, 21.151, "BCN",        "",        0x00, 0xd0, 0xd0},
    {21.151, 21.275, "PHONE",      "E",      0xff, 0x80, 0x00},
    {21.275, 21.450, "PHONE",      "E,G",    0xff, 0x80, 0x00},

    // 12m (24.890 - 24.990)
    {24.890, 24.920, "CW",         "E,G",    0x30, 0x60, 0xff},
    {24.920, 24.925, "DATA",       "E,G",    0xc0, 0x30, 0x30},
    {24.925, 24.930, "AUTO",       "E,G",    0xc0, 0x30, 0x30},
    {24.930, 24.931, "BCN",        "",        0x00, 0xd0, 0xd0},
    {24.931, 24.990, "PHONE",      "E,G",    0xff, 0x80, 0x00},

    // 10m (28.000 - 29.700)
    // Tech privileges start at 28.300
    {28.000, 28.070, "CW",         "E,G",    0x30, 0x60, 0xff},
    {28.070, 28.120, "DATA",       "E,G",    0xc0, 0x30, 0x30},
    {28.120, 28.190, "AUTO",       "E,G",    0xc0, 0x30, 0x30},
    {28.190, 28.225, "BCN",        "",        0x00, 0xd0, 0xd0},
    {28.225, 28.300, "BCN",        "E,G",    0x00, 0xd0, 0xd0},
    {28.300, 29.000, "PHONE",      "E,G,T",  0xff, 0x80, 0x00},
    {29.000, 29.200, "AM",         "E,G,T",  0xff, 0x80, 0x00},
    {29.200, 29.300, "PHONE",      "E,G,T",  0xff, 0x80, 0x00},
    {29.300, 29.510, "SAT",        "E,G,T",  0x90, 0x30, 0xc0},
    {29.520, 29.580, "RPT IN",     "E,G,T",  0xff, 0x80, 0x00},
    {29.600, 29.601, "FM CALL",    "E,G,T",  0xff, 0x80, 0x00},
    {29.620, 29.680, "RPT OUT",    "E,G,T",  0xff, 0x80, 0x00},

    // 6m (50.000 - 54.000) — Tech has full 6m privileges
    {50.000, 50.100, "CW",         "E,G,T",  0x30, 0x60, 0xff},
    {50.100, 50.300, "SSB",        "E,G,T",  0x30, 0xb0, 0x30},
    {50.300, 50.600, "DATA",       "E,G,T",  0xc0, 0x30, 0x30},
    {50.600, 51.000, "PHONE",      "E,G,T",  0xff, 0x80, 0x00},
    {51.000, 54.000, "FM/RPT",     "E,G,T",  0xff, 0x80, 0x00},
};

inline constexpr int kBandPlanCount = static_cast<int>(std::size(kBandPlan));

// Single-frequency spot markers (white dots with tooltip on hover)
// Source: ARRL Considerate Operator's Frequency Guide
struct BandSpot {
    double freqMhz;
    const char* tooltip;
};

inline constexpr BandSpot kBandSpots[] = {
    // 160m
    {1.810,   "CW QRP calling frequency"},
    {1.8366,  "WSPR"},
    {1.840,   "FT8"},
    {1.842,   "JS8Call"},
    {1.910,   "SSB QRP frequency"},
    // 80m
    {3.560,   "QRP CW calling frequency"},
    {3.573,   "FT8"},
    {3.575,   "FT4"},
    {3.578,   "JS8Call"},
    {3.580,   "PSK31 / RTTY"},
    {3.583,   "Olivia"},
    {3.590,   "RTTY/Data DX"},
    {3.5686,  "WSPR"},
    {3.5975,  "Winlink/VARA"},
    {3.625,   "FreeDV"},
    {3.790,   "DX window (3.790-3.800)"},
    {3.845,   "SSTV"},
    {3.885,   "AM calling frequency"},
    {3.985,   "QRP SSB calling frequency"},
    // 40m
    {7.030,   "QRP CW calling frequency"},
    {7.0386,  "WSPR"},
    {7.040,   "RTTY/Data DX"},
    {7.0475,  "FT4"},
    {7.070,   "PSK31"},
    {7.073,   "Olivia"},
    {7.074,   "FT8"},
    {7.078,   "JS8Call"},
    {7.080,   "RTTY"},
    {7.083,   "Winlink/VARA"},
    {7.171,   "SSTV"},
    {7.173,   "D-SSTV"},
    {7.177,   "FreeDV"},
    {7.285,   "QRP SSB calling frequency"},
    {7.290,   "AM calling frequency"},
    // 30m
    {10.106,  "CW calling"},
    {10.130,  "JS8Call"},
    {10.136,  "FT8"},
    {10.1387, "WSPR"},
    {10.140,  "FT4"},
    {10.142,  "PSK31"},
    {10.143,  "RTTY"},
    {10.144,  "Olivia"},
    // 20m
    {14.060,  "QRP CW calling frequency"},
    {14.070,  "PSK31"},
    {14.073,  "Olivia"},
    {14.074,  "FT8"},
    {14.078,  "JS8Call"},
    {14.080,  "FT4"},
    {14.085,  "RTTY"},
    {14.0956, "WSPR"},
    {14.0985, "Winlink/VARA"},
    {14.100,  "IBP/NCDXF beacons"},
    {14.230,  "SSTV"},
    {14.233,  "D-SSTV"},
    {14.236,  "FreeDV"},
    {14.285,  "QRP SSB calling frequency"},
    {14.286,  "AM calling frequency"},
    // 17m
    {18.100,  "FT8 / PSK31"},
    {18.104,  "FT4 / JS8Call"},
    {18.1046, "WSPR"},
    {18.105,  "RTTY"},
    {18.110,  "IBP/NCDXF beacons"},
    {18.1625, "Digital Voice"},
    // 15m
    {21.060,  "QRP CW calling frequency"},
    {21.070,  "PSK31"},
    {21.074,  "FT8"},
    {21.078,  "JS8Call"},
    {21.080,  "RTTY"},
    {21.0946, "WSPR"},
    {21.140,  "FT4"},
    {21.150,  "IBP/NCDXF beacons"},
    {21.340,  "SSTV"},
    {21.385,  "QRP SSB calling frequency"},
    // 12m
    {24.910,  "CW calling"},
    {24.915,  "FT8"},
    {24.919,  "FT4"},
    {24.920,  "PSK31"},
    {24.922,  "JS8Call"},
    {24.9246, "WSPR"},
    {24.925,  "RTTY"},
    {24.930,  "IBP/NCDXF beacons"},
    // 10m
    {28.060,  "QRP CW calling frequency"},
    {28.074,  "FT8"},
    {28.078,  "JS8Call"},
    {28.080,  "RTTY"},
    {28.120,  "PSK31"},
    {28.1246, "WSPR"},
    {28.180,  "FT4"},
    {28.200,  "IBP/NCDXF beacons"},
    {28.330,  "FreeDV"},
    {28.385,  "QRP SSB calling frequency"},
    {28.680,  "SSTV"},
    {29.600,  "FM simplex calling frequency"},
    // 6m
    {50.090,  "CW calling"},
    {50.100,  "SSB calling"},
    {50.290,  "PSK31"},
    {50.2936, "WSPR"},
    {50.313,  "FT8"},
    {50.318,  "FT4 / JS8Call"},
};

inline constexpr int kBandSpotCount = static_cast<int>(std::size(kBandSpots));

} // namespace MasterSDR
