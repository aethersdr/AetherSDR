#pragma once

#include <QString>

#include <functional>
#include <vector>

namespace AetherSDR {

// ============================================================================
// SmartMTR meter model
// ============================================================================
//
// The SmartMtrWidget is render-only and radio-agnostic. The parent pushes a
// MeterInput describing WHAT to show (kind + value + range); each kind owns a
// static MeterConfig (scale markers + a value->position mapping) that tells the
// widget how to draw it.
//
// To add a new kind: add a MeterKind value and one MeterConfig entry in the
// registry (SmartMtrConfig.cpp). No widget changes.
// ============================================================================

// The measurement the control currently shows. Extend here for new kinds.
enum class MeterKind { Signal, MicLevel };

// Tick footprint and emphasis (see SmartMtrUnits / SmartMtrColors).
enum class MarkerSize { Small, Large };
enum class MarkerColor { Normal, High };

// Label emphasis: Strong = full size, regular weight; Normal = slightly smaller
// and lighter weight.
enum class LabelStyle { Normal, Strong };

// Pushed by the parent each update.
struct MeterInput {
    MeterKind kind = MeterKind::Signal;
    bool hasValue = false; // false -> indicator parks at the scale minimum
    double value = 0.0;    // meaning is per kind (signal: dBm, mic: dB, ...)
    double min = 0.0;
    double max = 1.0;

    // Externally-measured peak (same units as value), for kinds whose peak is a
    // separate radio-sourced stat rather than a locally-derived window max. Mic
    // uses it (the radio's MICPEAK meter drives the peak marker); signal leaves
    // hasPeak false and the widget falls back to its sliding-window envelope.
    bool hasPeak = false;
    double peak = 0.0;
};

// One static scale tick, authored per kind. position is in hole-local UNITS
// (SmartMtrUnits::kScaleMin..kScaleMax); ticks outside that band are not drawn.
struct ScaleMarker {
    double position = 0.0;
    MarkerSize size = MarkerSize::Small;
    MarkerColor color = MarkerColor::Normal;
    QString label; // empty -> no label
    LabelStyle labelStyle = LabelStyle::Normal;
    // Horizontal shift of the label from the marker centre, in UNITS (+ right,
    // - left). Lets a multi-digit label straddle the tick as desired (e.g. the
    // tick falling between the digits of "+20").
    double labelOffset = 0.0;
};

// Static per-kind configuration.
struct MeterConfig {
    std::vector<ScaleMarker> markers;
    // Maps a value within [min,max] to a hole-local unit position. Unclamped:
    // callers range-check (markers) or clamp (indicator) as needed.
    std::function<double(double value, double min, double max)> valueToPosition;
};

// Registry lookup for a kind's static configuration.
const MeterConfig& meterConfig(MeterKind kind);

// Clamped indicator position for an input: handles the null value (-> scale
// minimum) and clamps the mapped position to the scale band.
double indicatorPosition(const MeterInput& in);

} // namespace AetherSDR
