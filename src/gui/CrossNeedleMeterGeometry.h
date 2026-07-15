#pragma once

#include <QColor>
#include <QPainterPath>
#include <QPointF>
#include <QString>
#include <QStringList>
#include <QVector>

class QIODevice;

namespace AetherSDR {

// Editable geometry for the PWR applet's cross-needle power/SWR face.
//
// The shipping values live in resources/meterfaces/cross-needle-v12.json.
// This class validates that resource and owns the mechanical mappings used by
// both the painter and tests: non-linear scale interpolation, concealed-pivot
// needle rays, constant-SWR curves, and their intersection.
class CrossNeedleMeterGeometry {
  public:
    struct Frame {
        double outerInset{6.0};
        double outerRadius{23.0};
        double faceInset{26.0};
        double faceRadius{15.0};
        double faceOutlineWidth{5.0};
        QColor bezel{23, 28, 33};
        QColor bezelEdge{71, 75, 77};
        QColor face{247, 239, 221};
    };

    // Layered, code-drawn material treatment for the illuminated meter card.
    // Coordinates are in the same 1500 x 1000 design space as the mechanics.
    struct FaceGradient {
        QColor top{232, 216, 183};
        QColor middle{250, 242, 221};
        QColor bottom{222, 205, 172};
        double middleStop{0.50};
        QPointF glowCenter{750.0, 400.0};
        double glowRadius{900.0};
        QColor glowInner{255, 250, 235, 205};
        QColor glowOuter{255, 250, 235, 0};
        QPointF vignetteCenter{750.0, 440.0};
        double vignetteRadius{930.0};
        double vignetteClearStop{0.62};
        QColor vignetteEdge{106, 97, 76, 65};
    };

    // Selectable dark-room treatment: a low ambient card, a broad lamp halo,
    // a concentrated bottom-center hotspot, a tighter paper-diffusion bloom,
    // and a symmetric edge vignette. Keeping every stop in the JSON resource
    // makes the photographic concept reproducible with QPainter and adjustable
    // without touching mechanics.
    struct UplightGradient {
        QColor top{142, 110, 73};
        QColor middle{174, 126, 70};
        QColor bottom{169, 119, 65};
        double middleStop{0.60};
        QPointF haloCenter{750.0, 920.0};
        double haloRadius{930.0};
        QColor haloInner{255, 181, 83, 180};
        QColor haloMiddle{242, 160, 72, 170};
        double haloMiddleStop{0.52};
        QColor haloShoulder{218, 143, 68, 40};
        double haloShoulderStop{0.72};
        QColor haloOuter{211, 137, 69, 0};
        QPointF hotspotCenter{750.0, 930.0};
        double hotspotRadius{470.0};
        QColor hotspotInner{255, 245, 110, 220};
        QColor hotspotMiddle{255, 195, 65, 145};
        double hotspotMiddleStop{0.48};
        QColor hotspotOuter{255, 146, 44, 0};
        QPointF bloomCenter{750.0, 940.0};
        double bloomRadius{330.0};
        QColor bloomInner{255, 246, 95, 225};
        QColor bloomMiddle{255, 215, 65, 90};
        double bloomMiddleStop{0.52};
        QColor bloomOuter{255, 220, 130, 0};
        double paperGrainOpacity{0.100};
        QColor scaleSeparator{117, 75, 48, 150};
        QPointF vignetteCenter{750.0, 650.0};
        double vignetteRadius{890.0};
        double vignetteClearStop{0.34};
        QColor vignetteEdge{9, 10, 12, 120};
    };

    // True dark-face treatment based on the approved ImageGen concept. The
    // mechanics remain shared; this block contains only reproducible card,
    // illumination, printed-ink, mask, and needle material choices.
    struct DarkTheme {
        QColor top{20, 22, 23};
        QColor middle{27, 28, 28};
        QColor bottom{25, 24, 23};
        double middleStop{0.58};
        QPointF ambientCenter{750.0, 410.0};
        double ambientRadius{900.0};
        QColor ambientInner{68, 66, 61, 55};
        QColor ambientOuter{40, 40, 38, 0};
        QPointF glowCenter{750.0, 940.0};
        double glowRadius{720.0};
        QColor glowInner{190, 92, 35, 100};
        QColor glowMiddle{112, 59, 34, 38};
        double glowMiddleStop{0.58};
        QColor glowOuter{80, 43, 30, 0};
        QPointF vignetteCenter{750.0, 500.0};
        double vignetteRadius{940.0};
        double vignetteClearStop{0.42};
        QColor vignetteEdge{0, 0, 0, 120};
        double paperGrainOpacity{0.220};

        QColor scaleOuter{202, 188, 159};
        QColor scaleSeparator{126, 95, 73, 225};
        QColor scaleCalibration{119, 71, 59, 235};
        QColor scaleInner{59, 84, 105};
        QColor majorTick{205, 191, 162};
        QColor minorTick{151, 139, 117, 230};
        QColor text{205, 190, 158};
        QColor rangeText{179, 162, 132};
        QColor swrGuide{139, 77, 58, 225};
        QColor swrLabel{205, 190, 158};

        QColor needle{202, 197, 183};
        QColor needleEdge{82, 78, 69, 220};
        QColor needleHighlight{240, 234, 216};
        QColor needleShadow{0, 0, 0, 105};

        QColor maskFill{20, 23, 26};
        QColor maskEdge{64, 69, 74};
        QColor maskText{219, 207, 181};
    };

    struct ScaleStyle {
        // Code-drawn vintage calibration ribbon. Each layer is an editable
        // concentric stroke, so the approved mechanics remain independent of
        // the face treatment and can be reproduced without bitmap artwork.
        double ribbonInset{16.0};
        double ribbonWidth{26.0};
        QColor ribbon{224, 212, 187, 225};
        double outerWidth{7.0};
        QColor outer{30, 36, 38};
        double separatorInset{6.5};
        double separatorWidth{3.5};
        QColor separator{246, 237, 216};
        double calibrationInset{11.0};
        double calibrationWidth{3.0};
        QColor calibration{126, 62, 54, 235};
        double innerInset{29.0};
        double innerWidth{4.5};
        QColor inner{48, 65, 91};
        double majorTickWidth{5.0};
        QColor majorTick{35, 42, 46};
        double minorTickWidth{2.5};
        QColor minorTick{55, 72, 91, 230};
        QColor text{35, 42, 46};
    };

    struct Typography {
        int scaleNumberPixels{42};
        int sideTitlePixels{50};
        int rangePixels{26};
        int unitPixels{40};
        int swrNumberPixels{36};
        int maskLabelPixels{46};
    };

    struct Scale {
        QPointF center;
        double radius{1.0};
        double startRadians{0.0};
        double endRadians{0.0};
        QVector<double> values;
        QVector<double> anglesRadians;
        // Derived monotone-cubic tangents. They preserve every calibrated
        // tick while avoiding visible elbows between adjacent intervals.
        QVector<double> angleTangents;
        QStringList labels;
        int minorSubdivisions{1};
        double labelOffset{34.0};
    };

    struct Title {
        QString text;
        QPointF center;
        // Authored with the same sign convention as the V12 proof generator.
        double rotationDegrees{0.0};
    };

    struct ScaleOverlap {
        // The physical face prints the reflected graph behind Forward. A very
        // short mask at their crossing makes that layer order legible.
        bool reflectedBehindForward{true};
        double reflectedGapCenterRadians{-1.1956693253530366};
        double reflectedGapHalfSpanRadians{0.012};
        double reflectedGapCenterRadiusInset{13.5};
        double reflectedGapWidth{45.0};
    };

    struct SwrGuide {
        QString label;
        QString displayLabel;
        // V12 source-registered construction: a restrained quadratic from
        // the traced upper endpoint to the y=880 datum, then a C1 Hermite
        // continuation behind the lower mask. These fields intentionally
        // preserve the physical face instead of recomputing a different fan
        // from the movement calibration.
        double swr{1.0};
        QPointF visibleUpper;
        QPointF registeredDatum;
        QPointF hiddenLower;
        double quadraticA{0.0};
        QPointF labelCenter;
    };

    struct SwrStyle {
        QColor guide{161, 74, 58, 230};
        double guideWidth{3.2};
        QColor label{45, 45, 42};
    };

    struct NeedleStyle {
        QColor line{18, 22, 25};
        double lineWidth{6.0};
        QColor edge{2, 4, 5, 210};
        double edgeWidth{2.0};
        double edgeOffset{1.4};
        QColor highlight{132, 132, 124, 175};
        double highlightWidth{1.6};
        double highlightOffset{1.2};
        QColor shadow{0, 0, 0, 80};
        double shadowWidth{9.0};
        QPointF shadowOffset{4.0, 5.0};
        QColor softShadow{0, 0, 0, 28};
        double softShadowWidth{14.0};
        QPointF softShadowOffset{7.0, 9.0};
    };

    struct Mask {
        QVector<QPointF> boundary;
        double bottomY{974.0};
        QColor fill{34, 40, 47};
        QColor edge{87, 99, 110};
        QColor text{234, 238, 241};
        QString label{QStringLiteral("SWR")};
        QPointF labelCenter{750.0, 928.0};
    };

    struct Validation {
        double activeForwardWatts{100.0};
        double activeSwr{1.5};
        double activeReflectedWatts{4.0};
        double rangeMultiplier{10.0};
        QPointF intersection{971.7280475161609, 835.2917160356153};
        QString guide{QStringLiteral("1.5")};
        double maximumGuideError{2.0};
    };

    static CrossNeedleMeterGeometry loadResource(QString *error = nullptr);
    static CrossNeedleMeterGeometry load(QIODevice &device, QString *error = nullptr);
    static CrossNeedleMeterGeometry fallback();

    bool isValid(QString *error = nullptr) const;

    double canvasWidth{1500.0};
    double canvasHeight{1000.0};
    int formatVersion{0};
    int designVersion{0};
    Frame frame;
    FaceGradient faceGradient;
    UplightGradient uplightGradient;
    DarkTheme darkTheme;
    ScaleStyle scaleStyle;
    Typography typography;
    Scale forwardScale;
    Scale reflectedScale;
    Title forwardTitle;
    Title reflectedTitle;
    QPointF forwardUnitCenter{435.0, 110.0};
    QPointF reflectedUnitCenter{1065.0, 110.0};
    QString rangeLabel;
    QPointF rangeLabelCenter{1245.0, 82.0};
    QVector<double> rangeMultipliers{1.0, 10.0, 100.0};
    ScaleOverlap scaleOverlap;
    SwrStyle swrStyle;
    QVector<SwrGuide> swrGuides;
    NeedleStyle needleStyle;
    Mask mask;
    Validation validation;

    double forwardAngle(double forwardWatts, double rangeMultiplier) const;
    double reflectedAngle(double reflectedWatts, double rangeMultiplier) const;
    QPointF forwardTip(double forwardWatts, double rangeMultiplier) const;
    QPointF reflectedTip(double reflectedWatts, double rangeMultiplier) const;
    QPointF needleIntersection(double forwardWatts, double reflectedWatts,
                               double rangeMultiplier) const;

    QPainterPath swrGuidePath(const SwrGuide &guide) const;
    QPointF swrGuideLabelCenter(const SwrGuide &guide) const;
    double distanceToGuide(const QPointF &point, const SwrGuide &guide) const;
    QString nearestGuideLabel(const QPointF &point, double *distance = nullptr) const;

    static double reflectedPowerWatts(double forwardWatts, double swr);
    static double swrFromPowers(double forwardWatts, double reflectedWatts);
    static double rangeMultiplierFor(int maxWatts, bool amplifierActive);

  private:
    static double interpolate(const Scale &scale, double value);
    static QPointF pointOnScale(const Scale &scale, double angleRadians);
    static QPointF lineIntersection(const QPointF &firstOrigin, const QPointF &firstTip,
                                    const QPointF &secondOrigin, const QPointF &secondTip);
};

} // namespace AetherSDR
