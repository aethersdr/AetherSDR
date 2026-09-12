// Per-server waterfall start fixed-point scale (1024 << zoom_max, not a
// universal 2^24). Socket-free regression guard for the band-switch
// waterfall freeze on Kiwi-path servers with zoom_max < 14: starts encoded
// with the wrong scale are clamped by the server to MAX_START, pinning the
// view at the band edge. Values mirror the live A/B proof on the
// zoom_max=11 Web-888 at 192.168.0.71:8073 (old-scale start clamped by the
// server to MAX_START(7)=2080768; per-server-scale start echoed back
// unclamped). Protocol source: RaspSDR/server rx/rx_waterfall.cpp,
// revision 68a64e1b39a3f291762904576f47c9d437fd3509 (MAX_START, HZperStart):
// https://github.com/RaspSDR/server/blob/68a64e1b39a3f291762904576f47c9d437fd3509/rx/rx_waterfall.cpp
#include "core/KiwiSdrProtocol.h"

#include <cmath>
#include <cstdio>

namespace {

int fail(const char* message)
{
    std::fprintf(stderr, "kiwi_sdr_waterfall_scale_test: %s\n", message);
    return 1;
}

bool nearlyEqualMhz(double a, double b, double epsilonMhz = 1.0e-4)
{
    return std::fabs(a - b) <= epsilonMhz;
}

} // namespace

int main()
{
    using namespace AetherSDR::KiwiSdrProtocol;

    // The scale is WF_WIDTH << zoom_max: a KiwiSDR (zoom_max=14) uses 2^24
    // and its behavior must not change; a Web-888 (zoom_max=11) uses 2^21.
    // Out-of-range zoom_max values clamp to [0, 20].
    if (waterfallStartFixedPointScale(14) != 16777216.0) {
        return fail("zoom_max=14 must keep the KiwiSDR 2^24 scale");
    }
    if (waterfallStartFixedPointScale(11) != 2097152.0) {
        return fail("zoom_max=11 must use the 2^21 scale");
    }
    if (waterfallStartFixedPointScale(-1) != waterfallStartFixedPointScale(0)
        || waterfallStartFixedPointScale(25)
               != waterfallStartFixedPointScale(20)) {
        return fail("zoom_max must clamp to [0, 20]");
    }

    // Web-888 scenario, 0-30.72 MHz full band, row starting at 14.104 MHz:
    // encode/decode round trip with the server's own scale is lossless up
    // to fixed-point quantization.
    const double fullLowMhz = 0.0;
    const double fullBandwidthMhz = 30.72;
    const double rowLowMhz = 14.104;
    const double web888Scale = waterfallStartFixedPointScale(11);
    const quint32 start =
        waterfallStartFixedPoint(fullLowMhz, fullBandwidthMhz,
                                 rowLowMhz, web888Scale);
    if (start != 962833u) {
        return fail("Web-888 row start did not encode to the expected value");
    }
    const double decodedMhz = waterfallStartFixedPointToLowMhz(
        fullLowMhz, fullBandwidthMhz, start, web888Scale);
    if (!nearlyEqualMhz(decodedMhz, rowLowMhz)) {
        return fail("Web-888 scale round trip did not decode the row low");
    }

    // The freeze scenario: the same row encoded with the old universal 2^24
    // scale overshoots the zoom_max=11 server's scale, so the server clamps
    // it to MAX_START(7) = (1024 << 11) - (1024 << (11 - 7)) = 2080768 — the
    // value the live server was observed clamping to. Decoded at the
    // server's scale that pins the view at the band edge, not at 14.104 MHz.
    const double kiwiScale = waterfallStartFixedPointScale(14);
    const quint32 wrongScaleStart = waterfallStartFixedPoint(
        fullLowMhz, fullBandwidthMhz, rowLowMhz, kiwiScale);
    if (wrongScaleStart <= static_cast<quint32>(web888Scale - 1.0)) {
        return fail("2^24-encoded start should overshoot the 2^21 server scale");
    }
    const quint32 maxStart7 = static_cast<quint32>(
        web888Scale - static_cast<double>(1024 << (11 - 7)));
    if (maxStart7 != 2080768u) {
        return fail("MAX_START(7) does not match the live clamp observation");
    }
    const double clampedMhz = waterfallStartFixedPointToLowMhz(
        fullLowMhz, fullBandwidthMhz, maxStart7, web888Scale);
    if (nearlyEqualMhz(clampedMhz, rowLowMhz, 1.0)) {
        return fail("clamped start must land at the band edge, not the row low");
    }

    // KiwiSDR regression: the same round trip at zoom_max=14 is unchanged.
    const quint32 kiwiStart = waterfallStartFixedPoint(
        fullLowMhz, fullBandwidthMhz, rowLowMhz, kiwiScale);
    if (!nearlyEqualMhz(waterfallStartFixedPointToLowMhz(
            fullLowMhz, fullBandwidthMhz, kiwiStart, kiwiScale),
            rowLowMhz)) {
        return fail("KiwiSDR 2^24 scale round trip regressed");
    }

    std::printf("All tests passed.\n");
    return 0;
}
