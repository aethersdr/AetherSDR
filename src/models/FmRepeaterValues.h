#pragma once

#include <algorithm>
#include <array>
#include <cmath>

namespace AetherSDR::fm {

struct CtcssTone {
    int code;
    const char* designation;
    double frequency;
};

// Standard EIA/TIA-603 values. One model-facing table serves the UI and the
// backend boundary validation, so an offered value cannot drift from one the
// engine accepts.
inline constexpr std::array<CtcssTone, 41> kCtcssTones{{
    { 1, "XZ", 67.0},  { 2, "XA", 71.9},  { 3, "WA", 74.4},  { 4, "XB", 77.0},
    { 5, "WB", 79.7},  { 6, "YZ", 82.5},  { 7, "YA", 85.4},  { 8, "YB", 88.5},
    { 9, "ZZ", 91.5},  {10, "ZA", 94.8},  {11, "ZB", 97.4},  {12, "1Z",100.0},
    {13, "1A",103.5},  {14, "1B",107.2},  {15, "2Z",110.9},  {16, "2A",114.8},
    {17, "2B",118.8},  {18, "3Z",123.0},  {19, "3A",127.3},  {20, "3B",131.8},
    {21, "4Z",136.5},  {22, "4A",141.3},  {23, "4B",146.2},  {24, "5Z",151.4},
    {25, "5A",156.7},  {26, "5B",162.2},  {27, "6Z",167.9},  {28, "6A",173.8},
    {29, "6B",179.9},  {30, "7Z",186.2},  {31, "7A",192.8},  {32, "M1",203.5},
    {33, "8Z",206.5},  {34, "M2",210.7},  {35, "M3",218.1},  {36, "M4",225.7},
    {37, "9Z",229.1},  {38, "M5",233.6},  {39, "M6",241.8},  {40, "M7",250.3},
    {41, "0Z",254.1},
}};

inline constexpr std::array<int, 104> kDtcsCodes{
    23,25,26,31,32,36,43,47,51,53,54,65,71,72,73,74,
    114,115,116,122,125,131,132,134,143,145,152,155,156,162,165,172,174,
    205,212,223,225,226,243,244,245,246,251,252,255,261,263,265,266,271,
    274,306,311,315,325,331,332,343,346,351,356,364,365,371,411,412,413,
    423,431,432,445,446,452,454,455,462,464,465,466,503,506,516,523,526,
    532,546,565,606,612,624,627,631,632,654,662,664,703,712,723,731,732,
    734,746,754,
};

[[nodiscard]] inline bool isCtcssTone(double hz) noexcept
{
    if (!std::isfinite(hz)) {
        return false;
    }
    return std::ranges::any_of(kCtcssTones, [hz](const CtcssTone& tone) {
        return std::abs(tone.frequency - hz) < 0.01;
    });
}

[[nodiscard]] inline bool isDtcsCode(int code) noexcept
{
    return std::ranges::find(kDtcsCodes, code) != kDtcsCodes.end();
}

} // namespace AetherSDR::fm
