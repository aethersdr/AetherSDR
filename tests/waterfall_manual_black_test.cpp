// Manual waterfall Black Level (WaterfallLevelMap): header-only, no Qt.
// Pins: rowsAreAbsoluteDb false equals the frozen copy `mainLaw` bit for bit;
// the flag reaches the manual branch only; an absolute row has a lit and a dark
// end. Every absolute-row check uses floorsDbm[], one HL2's floors: ANAN and
// RTL-SDR take this path unmeasured. The last block is a source-text pin.

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

// A frozen copy of the tile law, deliberately not a call into the header: it
// must not be the code under test.
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

    // Every black-point source, both units, against the frozen law. Tile-shaped
    // and absolute-dB values both go through, so the comparison covers the
    // range either row can carry.
    long compared = 0;
    long flexMismatch = 0;      // rowsAreAbsoluteDb false, any mode
    long autoDbmMismatch = 0;   // rowsAreAbsoluteDb true, SW or HW
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
                        p.rowsAreAbsoluteDb = false;
                        if (!sameBits(WLM::level(v, p), want)) {
                            ++flexMismatch;
                        }
                        p.rowsAreAbsoluteDb = true;
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
    report("absolute rows: SW and HW are bit-identical to the old law",
           autoDbmMismatch == 0);
    report("the tile black point is still 160 at slider 0 and 60 at slider 100",
           WLM::manualBlackThreshold(0, false) == 160.0f
               && WLM::manualBlackThreshold(100, false) == 60.0f);

    // The scope of every absolute-row claim below: one HL2 on a dummy load at
    // the 384 kHz span, LNA -12 / 0 / +10 / +20 / +30 / +40 dB, plus its lowest
    // floor at the 48 kHz span (bins eight times narrower: 9.03 dB lower). The
    // checks pin direction and monotonicity, not the end points.
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
                // Under the tile law even a signal 60 dB over the floor is
                // black.
                p.rowsAreAbsoluteDb = false;
                if (WLM::level(floorDbm, p) != 0.0f
                        || WLM::level(floorDbm + 60.0f, p) != 0.0f) {
                    oldLawBlanksEverything = false;
                }
                p.rowsAreAbsoluteDb = true;
                const float now = WLM::level(floorDbm, p);
                if (now < previous) {
                    newLawMonotone = false;
                }
                previous = now;
            }
            p.rowsAreAbsoluteDb = true;
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
    report("the tile law blanks an absolute row at every slider position (the defect)",
           oldLawBlanksEverything);
    report("absolute row: slider 100 lights every measured floor",
           newLawHasLitEnd);
    report("absolute row: slider 0 puts every measured floor at black",
           newLawHasDarkEnd);
    report("absolute row: some position blacks the floor and keeps a +30 dB signal",
           newLawSeparatesSignal);
    report("absolute row: raising the slider never darkens (same way as a Flex)",
           newLawMonotone);

    // The same direction on a tile, so the two units cannot be told apart by
    // which way the slider goes.
    {
        WLM::Params p;
        p.autoBlack = false;
        p.rowsAreAbsoluteDb = false;
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
    report("the absolute black point spans -60 to -160 dB, one dB a step",
           WLM::manualBlackThreshold(0, true) == -60.0f
               && WLM::manualBlackThreshold(100, true) == -160.0f
               && WLM::manualBlackThreshold(37, true) == -97.0f);

    // WtrFall Gain keeps its meaning across Off and SW on an absolute row: the same
    // range width, only the black point's source differs.
    {
        WLM::Params manual;
        manual.autoBlack = false;
        manual.rowsAreAbsoluteDb = true;
        manual.blackLevel = 70;                 // black point -130 dB
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
        report("absolute row: Off at a black point equals SW at the same black point",
               same);
    }

    // Source-text pin. The claim: SpectrumWidget passes its m_panBinsAbsolute
    // as rowsAreAbsoluteDb and keeps no second copy of the tile law. No behavioural
    // seam reaches it, because SpectrumWidget links into no test target. It
    // shows how the call is written, not that the widget runs.
    {
        const std::string src =
            readFile(std::string(AETHER_SOURCE_DIR) + "/src/gui/SpectrumWidget.cpp");
        report("SpectrumWidget.cpp was read", !src.empty());
        report("the widget hands the law its panBinsAbsolute flag",
               src.find("params.rowsAreAbsoluteDb = m_panBinsAbsolute;")
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
