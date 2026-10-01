// Manual waterfall Black Level on a dBm row (WaterfallLevelMap) — header-only,
// pure logic, no Qt.
//
// The defect, observed on a Hermes-Lite 2 on 2026-10-01: with the Black Level
// button on Off, every new waterfall row was exactly black at slider 0, 25, 50,
// 75 and 100 (12 of 12 frames, mean luma 0.00). The manual branch compared the
// row against `160 - level`, a threshold in Flex TILE-intensity units, while
// the row on that radio is the host-computed pan frame in dBm: negative.
//
// What this file pins:
//   1. THE FLEX PATH IS UNCHANGED. `mainLaw` below is the law as it stood
//      before the change, kept as a frozen copy on purpose: with rowsAreDbm
//      false, WaterfallLevelMap::level must agree with it bit for bit across
//      all three black-point sources, NaN and infinities included.
//   2. rowsAreDbm reaches the MANUAL branch only. SW and HW are untouched.
//   3. The old law blanks a dBm row at every slider position (the defect),
//      and the new one does not: the slider has a lit end and a dark end for
//      every floor measured on that radio, and it moves the same way a Flex's
//      does.
//   4. SpectrumWidget hands the law its own m_panBinsAbsolute. THIS ONE IS A
//      SOURCE-TEXT CHECK: SpectrumWidget links into no test, so it proves the
//      call is WRITTEN that way and fails on a reversal. It does not prove the
//      widget runs, and it does not see MainWindow pushing the capability in.
//
// That HL2 declares panBinsAbsolute() and a Flex does not is pinned on real
// backend instances by noise_floor_auto_adjust_gate_test, not repeated here.
//
// No radio was measured for this file. The floors are quoted from the bench
// record of that observation; nothing here talks to hardware.

#include "gui/WaterfallLevelMap.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>

namespace WLM = AetherSDR::WaterfallLevelMap;

static int g_total = 0;
static int g_failed = 0;

static void report(const char* name, bool ok)
{
    ++g_total;
    if (!ok) {
        ++g_failed;
        std::printf("FAIL: %s\n", name);
    } else {
        std::printf("ok:   %s\n", name);
    }
}

// ── The law before this change, frozen. Do not "tidy" it into a call to the
// header: its whole value is that it is NOT the code under test. ─────────────
static float mainBound(float lo, float value, float hi)
{
    // qBound(min, val, max) == qMax(min, qMin(max, val)).
    const float capped = (hi < value) ? hi : value;
    return (lo < capped) ? capped : lo;
}

static float mainHighThresholdRaw(float lowRaw, int colorGain)
{
    const float low = mainBound(0.0f, lowRaw, 65535.0f);
    const double num = (100.0 - colorGain) / 100.0 * std::cbrt(65535.0 - low);
    double high = low + num * num * num;
    if (high < low + 100.0) {
        high = low + 100.0;
    }
    return static_cast<float>(high);
}

static float mainLaw(float intensity, bool autoBlack, bool radioSide,
                     float radioAutoBlackRaw, float autoBlackThresh,
                     int autoBlackOffset, int blackLevel, int colorGain)
{
    float blackThresh;
    float rangeWidth;
    if (autoBlack && radioSide && radioAutoBlackRaw > 0.0f) {
        const float lowRaw = mainBound(
            0.0f,
            radioAutoBlackRaw + (50 - autoBlackOffset) * 0.5f * 128.0f,
            65535.0f);
        const float highRaw = mainHighThresholdRaw(lowRaw, colorGain);
        blackThresh = lowRaw / 128.0f;
        rangeWidth  = std::max(1.0f, (highRaw - lowRaw) / 128.0f);
    } else if (autoBlack) {
        blackThresh = autoBlackThresh + (50 - autoBlackOffset) * 0.5f;
        rangeWidth  = std::max(1.0f, 120.0f - colorGain * 0.91f);
    } else {
        blackThresh = 160.0f - blackLevel * 1.0f;
        rangeWidth  = std::max(1.0f, 120.0f - colorGain * 0.91f);
    }
    return mainBound(0.0f, (intensity - blackThresh) / rangeWidth, 1.0f);
}

static bool sameBits(float a, float b)
{
    return std::memcmp(&a, &b, sizeof(float)) == 0;
}

static std::string readFile(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

int main()
{
    const float kNan = std::numeric_limits<float>::quiet_NaN();
    const float kInf = std::numeric_limits<float>::infinity();

    // ── 1 and 2. Every black-point source, both units, against the frozen
    // law. Tile-shaped values AND dBm-shaped values go through, so the
    // comparison covers the range either row can carry.
    long compared = 0;
    long flexMismatch = 0;      // rowsAreDbm false, any mode
    long autoDbmMismatch = 0;   // rowsAreDbm true, SW or HW
    const float specials[] = {kNan, kInf, -kInf, 0.0f, -0.0f};
    for (int mode = 0; mode < 3; ++mode) {          // 0 manual, 1 SW, 2 HW
        for (int gain = 0; gain <= 100; gain += 5) {
            for (int knob = 0; knob <= 100; knob += 5) {
                for (int rawStep = 0; rawStep < 3; ++rawStep) {
                    // HW with raw 0 falls back to SW: keep that row in.
                    const float raw = (rawStep == 0) ? 0.0f
                                    : (rawStep == 1) ? 12800.0f : 65000.0f;
                    WLM::Params p;
                    p.autoBlack = (mode != 0);
                    p.radioSideAutoBlack = (mode == 2);
                    p.radioAutoBlackRaw = raw;
                    p.autoBlackThresh = 104.5f;
                    p.autoBlackOffset = knob;
                    p.blackLevel = knob;
                    p.colorGain = gain;
                    auto one = [&](float v) {
                        const float want = mainLaw(
                            v, p.autoBlack, p.radioSideAutoBlack,
                            p.radioAutoBlackRaw, p.autoBlackThresh,
                            p.autoBlackOffset, p.blackLevel, p.colorGain);
                        p.rowsAreDbm = false;
                        if (!sameBits(WLM::level(v, p), want)) {
                            ++flexMismatch;
                        }
                        p.rowsAreDbm = true;
                        if (mode != 0 && !sameBits(WLM::level(v, p), want)) {
                            ++autoDbmMismatch;
                        }
                        ++compared;
                    };
                    for (float v = -200.0f; v <= 600.0f; v += 0.37f) {
                        one(v);
                    }
                    for (float v : specials) {
                        one(v);
                    }
                }
            }
        }
    }
    std::printf("compared %ld samples against the frozen law\n", compared);
    report("the comparison ran (more than a million samples)", compared > 1000000);
    report("Flex-shaped rows: Off, SW and HW are bit-identical to the old law",
           flexMismatch == 0);
    report("dBm rows: SW and HW are bit-identical to the old law",
           autoDbmMismatch == 0);
    report("the tile black point is still 160 at slider 0 and 60 at slider 100",
           WLM::manualBlackThreshold(0, false) == 160.0f
               && WLM::manualBlackThreshold(100, false) == 60.0f);

    // ── 3. The defect, and its repair. Floors as measured on one HL2 on a
    // dummy load at the 384 kHz span, LNA -12 / 0 / +10 / +20 / +30 / +40 dB,
    // plus the same radio's lowest floor at the 48 kHz span (bins eight times
    // narrower: 10*log10(8) = 9.03 dB lower).
    const float floorsDbm[] = {-111.0f, -121.7f, -128.8f, -140.1f,
                               -147.6f, -148.1f, -148.1f - 9.03f};
    bool oldLawBlanksEverything = true;
    bool newLawHasLitEnd = true;
    bool newLawHasDarkEnd = true;
    bool newLawSeparatesSignal = true;
    bool newLawMonotone = true;
    for (float floorDbm : floorsDbm) {
        for (int gain = 0; gain <= 100; gain += 50) {
            WLM::Params p;
            p.autoBlack = false;
            p.colorGain = gain;
            float previous = -1.0f;
            for (int slider = 0; slider <= 100; ++slider) {
                p.blackLevel = slider;
                // A signal 60 dB over the floor is still black under the old
                // law: that is "the waterfall goes fully black".
                p.rowsAreDbm = false;
                if (WLM::level(floorDbm, p) != 0.0f
                        || WLM::level(floorDbm + 60.0f, p) != 0.0f) {
                    oldLawBlanksEverything = false;
                }
                p.rowsAreDbm = true;
                const float now = WLM::level(floorDbm, p);
                if (now < previous) {
                    newLawMonotone = false;
                }
                previous = now;
            }
            p.rowsAreDbm = true;
            p.blackLevel = 100;
            if (!(WLM::level(floorDbm, p) > 0.0f)) {
                newLawHasLitEnd = false;
            }
            p.blackLevel = 0;
            if (WLM::level(floorDbm, p) != 0.0f) {
                newLawHasDarkEnd = false;
            }
            // Some slider position puts the floor at black and a signal 30 dB
            // above it clearly on: the control can do its job.
            bool separated = false;
            for (int slider = 0; slider <= 100; ++slider) {
                p.blackLevel = slider;
                if (WLM::level(floorDbm, p) == 0.0f
                        && WLM::level(floorDbm + 30.0f, p) > 0.2f) {
                    separated = true;
                }
            }
            if (!separated) {
                newLawSeparatesSignal = false;
            }
        }
    }
    report("the tile law blanks a dBm row at every slider position (the defect)",
           oldLawBlanksEverything);
    report("dBm row: slider 100 lights every measured floor",
           newLawHasLitEnd);
    report("dBm row: slider 0 puts every measured floor at black",
           newLawHasDarkEnd);
    report("dBm row: some position blacks the floor and keeps a +30 dB signal",
           newLawSeparatesSignal);
    report("dBm row: raising the slider never darkens (same way as a Flex)",
           newLawMonotone);

    // The same direction on a tile, so the two units cannot be told apart by
    // which way the slider goes.
    {
        WLM::Params p;
        p.autoBlack = false;
        p.rowsAreDbm = false;
        bool tileMonotone = true;
        float previous = -1.0f;
        for (int slider = 0; slider <= 100; ++slider) {
            p.blackLevel = slider;
            const float now = WLM::level(110.0f, p);
            if (now < previous) {
                tileMonotone = false;
            }
            previous = now;
        }
        p.blackLevel = 100;
        const float lit = WLM::level(110.0f, p);
        p.blackLevel = 0;
        const float dark = WLM::level(110.0f, p);
        report("tile row: raising the slider never darkens, and both ends differ",
               tileMonotone && lit > 0.0f && dark == 0.0f);
    }
    report("the dBm black point spans -60 to -160 dBm, one dB a step",
           WLM::manualBlackThreshold(0, true) == -60.0f
               && WLM::manualBlackThreshold(100, true) == -160.0f
               && WLM::manualBlackThreshold(37, true) == -97.0f);

    // WtrFall Gain keeps its meaning across Off and SW on a dBm row: the same
    // range width, only the black point's source differs.
    {
        WLM::Params manual;
        manual.autoBlack = false;
        manual.rowsAreDbm = true;
        manual.blackLevel = 70;                 // black point -130 dBm
        WLM::Params sw = manual;
        sw.autoBlack = true;
        sw.autoBlackThresh = -130.0f;           // the same black point, measured
        sw.autoBlackOffset = 50;
        bool same = true;
        for (int gain = 0; gain <= 100; gain += 10) {
            manual.colorGain = gain;
            sw.colorGain = gain;
            for (float dbm = -150.0f; dbm <= -20.0f; dbm += 1.5f) {
                if (!sameBits(WLM::level(dbm, manual), WLM::level(dbm, sw))) {
                    same = false;
                }
            }
        }
        report("dBm row: Off at a black point equals SW at the same black point",
               same);
    }

    // ── 4. The widget's call, as written. Source text: see the header note.
    {
        const std::string src =
            readFile(std::string(AETHER_SOURCE_DIR) + "/src/gui/SpectrumWidget.cpp");
        report("SpectrumWidget.cpp was read", !src.empty());
        report("the widget hands the law its panBinsAbsolute flag",
               src.find("params.rowsAreDbm = m_panBinsAbsolute;")
                   != std::string::npos);
        report("the widget calls the shared law",
               src.find("return WaterfallLevelMap::level(intensity, params);")
                   != std::string::npos);
        report("no second copy of the tile threshold is left in the widget",
               src.find("160.0f - m_wfBlackLevel") == std::string::npos);
    }

    std::printf("%d checks, %d failed\n", g_total, g_failed);
    return g_failed == 0 ? 0 : 1;
}
