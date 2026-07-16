#pragma once

#include <QColor>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QVector>

class QIODevice;
class QPainter;
class QPainterPath;

namespace AetherSDR {

// Shared physical face materials used by both analog meter widgets. The
// standard S-meter adds AetherDefault; the independent PWR meter uses the
// three physical treatments only.
enum class AnalogMeterFaceTheme {
    AetherDefault,
    ClassicWarm,
    DarkRoomUplight,
    GraphiteDark,
};

QString analogMeterFaceThemeId(AnalogMeterFaceTheme theme);
AnalogMeterFaceTheme analogMeterFaceThemeFromId(
    const QString& id, AnalogMeterFaceTheme fallback);

class AnalogMeterFaceThemeCatalog {
public:
    struct FaceGradient {
        QColor top;
        QColor middle;
        QColor bottom;
        double middleStop{0.5};
        QPointF glowCenter;
        double glowRadius{1.0};
        QColor glowInner;
        QColor glowOuter;
        QPointF vignetteCenter;
        double vignetteRadius{1.0};
        double vignetteClearStop{0.0};
        QColor vignetteEdge;
        double paperGrainOpacity{0.0};

        bool operator==(const FaceGradient&) const = default;
    };

    struct UplightGradient {
        QColor top;
        QColor middle;
        QColor bottom;
        double middleStop{0.5};
        QPointF haloCenter;
        double haloRadius{1.0};
        QColor haloInner;
        QColor haloMiddle;
        double haloMiddleStop{0.0};
        QColor haloShoulder;
        double haloShoulderStop{0.0};
        QColor haloOuter;
        QPointF hotspotCenter;
        double hotspotRadius{1.0};
        QColor hotspotInner;
        QColor hotspotMiddle;
        double hotspotMiddleStop{0.0};
        QColor hotspotOuter;
        QPointF bloomCenter;
        double bloomRadius{1.0};
        QColor bloomInner;
        QColor bloomMiddle;
        double bloomMiddleStop{0.0};
        QColor bloomOuter;
        QPointF vignetteCenter;
        double vignetteRadius{1.0};
        double vignetteClearStop{0.0};
        QColor vignetteEdge;
        double paperGrainOpacity{0.0};

        bool operator==(const UplightGradient&) const = default;
    };

    struct DarkGradient {
        QColor top;
        QColor middle;
        QColor bottom;
        double middleStop{0.5};
        QPointF ambientCenter;
        double ambientRadius{1.0};
        QColor ambientInner;
        QColor ambientOuter;
        QPointF glowCenter;
        double glowRadius{1.0};
        QColor glowInner;
        QColor glowMiddle;
        double glowMiddleStop{0.0};
        QColor glowOuter;
        QPointF vignetteCenter;
        double vignetteRadius{1.0};
        double vignetteClearStop{0.0};
        QColor vignetteEdge;
        double paperGrainOpacity{0.0};

        bool operator==(const DarkGradient&) const = default;
    };

    struct Palette {
        QColor ribbon;
        QColor scaleOuter;
        QColor scaleSeparator;
        QColor scaleCalibration;
        QColor scaleInner;
        QColor majorTick;
        QColor minorTick;
        QColor text;
        QColor secondaryText;
        QColor swrGuide;
        QColor swrLabel;
        QColor needle;
        QColor needleEdge;
        QColor needleHighlight;
        QColor needleShadow;
        QColor needleSoftShadow;
        QColor maskFill;
        QColor maskEdge;
        QColor maskText;

        bool operator==(const Palette&) const = default;
    };

    static AnalogMeterFaceThemeCatalog loadResource(QString* error = nullptr);
    static AnalogMeterFaceThemeCatalog load(QIODevice& device, QString* error = nullptr);
    static AnalogMeterFaceThemeCatalog fallback();

    bool isValid(QString* error = nullptr) const;
    const Palette& palette(AnalogMeterFaceTheme theme) const;
    void drawBackground(QPainter& painter, const QRectF& face,
                        AnalogMeterFaceTheme theme) const;
    QPainterPath lowerMaskPath(const QRectF& face) const;
    QVector<QPointF> lowerMaskBoundary(const QRectF& face) const;

    // Keep the resource and compiled fallback mechanically comparable. A
    // defaulted C++20 equality operator automatically includes every field
    // added to the catalog or one of its nested material records.
    bool operator==(const AnalogMeterFaceThemeCatalog&) const = default;

    int formatVersion{0};
    QRectF referenceFace;
    QVector<QPointF> normalizedMaskBoundary;
    double normalizedMaskBottom{1.0};
    FaceGradient classicGradient;
    UplightGradient uplightGradient;
    DarkGradient darkGradient;
    Palette classicPalette;
    Palette uplightPalette;
    Palette darkPalette;
};

} // namespace AetherSDR
