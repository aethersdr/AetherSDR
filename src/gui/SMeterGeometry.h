#pragma once

#include <QPointF>
#include <QRectF>
#include <QSize>
#include <QSizeF>
#include <QString>
#include <QVector>

#include <optional>

class QIODevice;

namespace AetherSDR {

// Versioned, editable construction for the standard S-meter face.
//
// The shipping values live in resources/meterfaces/s-meter-v1.json. Unlike
// the fixed-aspect cross-needle face, this meter intentionally keeps its
// existing responsive width/height formulas so applet and detached-window
// resizing remain pixel-for-pixel compatible with the original painter.
class SMeterGeometry {
public:
    struct Tick {
        double value{0.0};
        QString label;
    };

    struct Sizing {
        QSize preferred{280, 140};
        QSize minimum{200, 100};
        // Outside this supported width/height range, the meter face is
        // centered in a bounded viewport instead of stretching its movement
        // geometry until the pivot leaves the printed arcs.
        double minimumAspectRatio{1.75};
        double maximumAspectRatio{4.0};
    };

    struct Arc {
        double centerXWidthFactor{0.5};
        double radiusWidthFactor{0.85};
        // arcCenterY = radius + height * centerYHeightFactor
        double centerYHeightFactor{0.35};
        double startDegrees{55.0};
        double endDegrees{125.0};
        double innerGapPixels{6.0};
        double lineWidthPixels{3.0};
    };

    struct TickStyle {
        double startOffsetPixels{2.0};
        double endOffsetPixels{14.0};
        double labelOffsetPixels{26.0};
        double lineWidthPixels{1.5};
        int fontMinimumPixels{10};
        double fontHeightFactor{0.1};
        bool bold{true};
    };

    struct RxScale {
        double minimumDbm{-127.0};
        double s9Dbm{-73.0};
        double maximumDbm{-13.0};
        double dbPerSUnit{6.0};
        double s9Fraction{0.6};
        QVector<Tick> ticks;
    };

    struct StaticScale {
        double minimum{0.0};
        double maximum{1.0};
        double warningStart{0.0};
        bool hasWarning{false};
        QVector<Tick> ticks;
    };

    struct PowerTickPolicy {
        double minimumScaleWatts{0.0};
        int tickStepWatts{10};
        int labelStepWatts{40};
    };

    struct Needle {
        double pivotYBelowWidgetPixels{6.0};
        double tipExtensionPixels{14.0};
        double lineWidthPixels{2.0};
        double shadowWidthPixels{3.0};
        QPointF shadowOffset{1.0, 1.0};
    };

    struct Pivot {
        double minimumRadiusPixels{13.5};
        // The width-derived radius is capped by the preferred face aspect
        // ratio so a wide, short floating window cannot inflate the mask.
        double radiusWidthFactor{0.0975};
        double glowRadiusFactor{3.4};
        double glowMiddleFactor{0.45};
        int glowCenterAlpha{80};
        int glowMiddleAlpha{28};
        double rimWidthPixels{1.0};
    };

    struct PeakMarker {
        double radiusInsetPixels{2.0};
        double lengthPixels{6.0};
        double halfWidthPixels{3.0};
        double minimumLeadDb{1.0};
    };

    struct PeakHold {
        double innerRadiusOffsetPixels{-4.0};
        double outerRadiusOffsetPixels{10.0};
        double lineWidthPixels{2.0};
        double visibleAboveMinimumDb{1.0};
    };

    struct Readout {
        int sourceFontMinimumPixels{9};
        double sourceFontHeightDivisor{14.0};
        int valueFontMinimumPixels{13};
        double valueFontHeightDivisor{8.0};
        int topExtraPixels{4};
        int sideMarginPixels{6};
    };

    struct Layout {
        QRectF viewport;
        double centerX{0.0};
        double radius{0.0};
        double centerY{0.0};
        double needlePivotY{0.0};
        double innerRadius{0.0};
        double pivotRadius{0.0};
        int tickFontPixels{0};
        int sourceFontPixels{0};
        int valueFontPixels{0};
    };

    struct MovementRay {
        QPointF pivot;
        QPointF scalePoint;
        QPointF direction;
    };

    static SMeterGeometry loadResource(QString* error = nullptr);
    static SMeterGeometry load(QIODevice& device, QString* error = nullptr);
    static SMeterGeometry fallback();

    bool isValid(QString* error = nullptr) const;
    Layout layoutFor(const QSizeF& size) const;
    double fractionToRadians(double fraction) const;
    double rxFraction(double dbm) const;
    double scaleFraction(const StaticScale& scale, double value) const;
    MovementRay movementRayFor(const QSizeF& size, double fraction) const;
    std::optional<QPointF> movementRayCircleIntersection(
        const QSizeF& size, double fraction, double radius) const;
    QPointF needleTip(const QSizeF& size, double fraction) const;
    const PowerTickPolicy& powerTickPolicy(double maximumWatts) const;

    int formatVersion{0};
    int designVersion{0};
    Sizing sizing;
    Arc arc;
    TickStyle tickStyle;
    RxScale rxScale;
    StaticScale swrScale;
    StaticScale levelScale;
    StaticScale compressionScale;
    QVector<PowerTickPolicy> powerTickPolicies;
    Needle needle;
    Pivot pivot;
    PeakMarker peakMarker;
    PeakHold peakHold;
    Readout readout;
};

} // namespace AetherSDR
