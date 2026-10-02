#pragma once

// The CTCSS tone set, shared by the RxApplet dropdown and the bridge's
// `slice tone` verb so the bridge accepts exactly the tones an operator can
// dial (50 tones). Motorola PL tones plus EIA interstitials (as in the Icom
// backend, #5140); CISA AUXFOG Appendix G confirms the WZ designation for 69.3
// Hz. Entries outside the numbered sequence have code 0; callers show a bare
// frequency only when the designation is empty.

#include <cmath>
#include <cstddef>

namespace AetherSDR {

struct CtcssTone {
    int code;
    const char* designation;
    double frequency;
};

inline constexpr CtcssTone kCtcssTones[] = {
    { 1, "XZ", 67.0},  { 0, "WZ", 69.3},  { 2, "XA", 71.9},  { 3, "WA", 74.4},
    { 4, "XB", 77.0},
    { 5, "WB", 79.7},  { 6, "YZ", 82.5},  { 7, "YA", 85.4},  { 8, "YB", 88.5},
    { 9, "ZZ", 91.5},  {10, "ZA", 94.8},  {11, "ZB", 97.4},  {12, "1Z",100.0},
    {13, "1A",103.5},  {14, "1B",107.2},  {15, "2Z",110.9},  {16, "2A",114.8},
    {17, "2B",118.8},  {18, "3Z",123.0},  {19, "3A",127.3},  {20, "3B",131.8},
    {21, "4Z",136.5},  {22, "4A",141.3},  {23, "4B",146.2},  {24, "5Z",151.4},
    {25, "5A",156.7},  { 0, "",159.8},    {26, "5B",162.2},  { 0, "",165.5},
    {27, "6Z",167.9},  { 0, "",171.3},    {28, "6A",173.8},  { 0, "",177.3},
    {29, "6B",179.9},  { 0, "",183.5},    {30, "7Z",186.2},  { 0, "",189.9},
    {31, "7A",192.8},  { 0, "",196.6},    { 0, "",199.5},    {32, "M1",203.5},
    {33, "8Z",206.5},  {34, "M2",210.7},  {35, "M3",218.1},  {36, "M4",225.7},
    {37, "9Z",229.1},  {38, "M5",233.6},  {39, "M6",241.8},  {40, "M7",250.3},
    {41, "0Z",254.1},
};

inline constexpr std::size_t kCtcssToneCount =
    sizeof(kCtcssTones) / sizeof(kCtcssTones[0]);

// Tones are quoted to one decimal everywhere (the slice carries "100.0"), so
// the comparison is to that precision rather than exact — a caller typing
// 100.00 means the same tone as one typing 100.0.
inline bool isCtcssFrequency(double hz)
{
    for (const CtcssTone& t : kCtcssTones) {
        if (std::fabs(t.frequency - hz) < 0.05)
            return true;
    }
    return false;
}

} // namespace AetherSDR
