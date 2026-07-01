#include "gui/KiwiSdrTraceMath.h"

#include <QVector>

#include <cmath>
#include <cstdio>

namespace {

constexpr float kMinDbm = -200.0f;
constexpr float kMaxDbm = 0.0f;

bool nearlyEqual(float a, float b, float epsilon = 0.001f)
{
    return std::fabs(a - b) <= epsilon;
}

int fail(const char* message)
{
    std::fprintf(stderr, "kiwi_sdr_trace_math_test: %s\n", message);
    return 1;
}

float floorOf(const QVector<float>& bins)
{
    return AetherSDR::KiwiSdrTraceMath::estimateTraceFloorDbm(bins, kMinDbm);
}

} // namespace

int main()
{
    using namespace AetherSDR::KiwiSdrTraceMath;

    QVector<float> trace(128, -100.0f);
    for (int i = 0; i < 8; ++i) {
        trace[i] = kMinDbm;
    }
    for (int i = 96; i < 112; ++i) {
        trace[i] = -45.0f;
    }
    if (!nearlyEqual(floorOf(trace), -100.0f)) {
        return fail("floor estimate should ignore blanks and signals");
    }

    QVector<float> gestureTrace(128, -80.0f);
    gestureTrace[10] = kMinDbm;
    gestureTrace[64] = -50.0f;
    TraceFloorState lockedFloor{-100.0f, true};
    stabilizeTraceFloor(gestureTrace, lockedFloor, false, kMinDbm, kMaxDbm);
    if (!nearlyEqual(lockedFloor.floorDbm, -100.0f)
        || !nearlyEqual(floorOf(gestureTrace), -100.0f)
        || !nearlyEqual(gestureTrace[64], -70.0f)
        || !nearlyEqual(gestureTrace[10], kMinDbm)) {
        return fail("gesture-time stabilization should keep the FFT floor anchored");
    }

    QVector<float> mappedRow(256, -80.0f);
    mappedRow[128] = -50.0f;
    QVector<float> mappedTrace = mapRowToTrace(
        mappedRow, 128,
        14.0, 1.0,
        14.0, 0.5,
        kMinDbm);
    TraceFloorState mappedFloor{-100.0f, true};
    stabilizeTraceFloor(mappedTrace, mappedFloor, false, kMinDbm, kMaxDbm);
    if (!nearlyEqual(mappedFloor.floorDbm, -100.0f)
        || !nearlyEqual(floorOf(mappedTrace), -100.0f)) {
        return fail("mapped gesture row should keep the FFT trace floor anchored");
    }

    QVector<float> dcEdgeRow(1024, -90.0f);
    dcEdgeRow[512] = -45.0f;
    const QVector<float> dcEdgeTrace = mapRowToTrace(
        dcEdgeRow, 128,
        7.5, 15.0,
        7.436213, 14.543089,
        kMinDbm);
    if (dcEdgeTrace.isEmpty() || floorOf(dcEdgeTrace) <= kMinDbm) {
        return fail("DC-edge Kiwi row should map into the visible trace");
    }

    QVector<float> wideServerRow(1024, -100.0f);
    const double serverLowMhz = 10.0;
    const double serverBandwidthMhz = 10.0;
    const double carrierMhz = 14.25;
    const int sourceBin = static_cast<int>(
        ((carrierMhz - serverLowMhz) / serverBandwidthMhz)
        * wideServerRow.size());
    wideServerRow[sourceBin] = -45.0f;
    const QVector<float> dssMappedTrace = mapRowToTrace(
        wideServerRow, 768,
        15.0, serverBandwidthMhz,
        14.0, 1.0,
        kMinDbm);
    if (dssMappedTrace.isEmpty()) {
        return fail("3D-width Kiwi trace should map into the visible trace");
    }
    const int expectedViewportBin = static_cast<int>(
        ((carrierMhz - 13.5) / 1.0) * dssMappedTrace.size());
    int strongestBin = 0;
    for (int i = 1; i < dssMappedTrace.size(); ++i) {
        if (dssMappedTrace[i] > dssMappedTrace[strongestBin]) {
            strongestBin = i;
        }
    }
    if (std::abs(strongestBin - expectedViewportBin) > 2) {
        return fail("3D-width Kiwi trace should align to the viewport frequency frame");
    }

    QVector<float> adaptingTrace(128, -80.0f);
    TraceFloorState adaptingFloor{-100.0f, true};
    stabilizeTraceFloor(adaptingTrace, adaptingFloor, true, kMinDbm, kMaxDbm);
    if (!nearlyEqual(adaptingFloor.floorDbm, -99.4f)
        || !nearlyEqual(floorOf(adaptingTrace), -99.4f)) {
        return fail("full-coverage upward floor adaptation should be gradual");
    }

    const QVector<float> bins{0.0f, 10.0f, 20.0f, 30.0f};
    const float averaged =
        averagedBinSample(bins, 0.5, 2.5, 1.5, kMinDbm);
    if (!nearlyEqual(averaged, 10.0f)) {
        return fail("averaged bin sampler should weight the covered source span");
    }

    return 0;
}
