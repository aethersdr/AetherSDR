#pragma once

#include "AnalogMeterFaceTheme.h"
#include "CrossNeedleMeterGeometry.h"
#include "CrossNeedleRangeSelector.h"
#include "MeterSmoother.h"

#include <QElapsedTimer>
#include <QPixmap>
#include <QTimer>
#include <QWidget>

class QPainter;
class QKeyEvent;

namespace AetherSDR {

// Deterministic two-movement Forward/Reflected power and SWR meter.
//
// All construction geometry is loaded from the versioned JSON face resource;
// this class only renders it and maps the existing Forward-power/SWR feed onto
// the two independent physical movements. On receive both movements park at
// their zero stops: this is a TX power/SWR instrument, not an RX S-meter.
class CrossNeedleMeterWidget : public QWidget {
    Q_OBJECT

  public:
    enum class FaceTheme {
        ClassicWarm,
        DarkRoomUplight,
        GraphiteDark,
    };
    Q_ENUM(FaceTheme)

    explicit CrossNeedleMeterWidget(QWidget *parent = nullptr);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override;

    double forwardWatts() const { return m_forwardWatts; }
    double reflectedWatts() const { return m_reflectedWatts; }
    double swr() const { return m_swr; }
    double rangeMultiplier() const { return m_rangeMultiplier; }
    double rangeCeilingMultiplier() const { return m_rangeSelector.ceilingMultiplier(); }
    bool rangeHoldPending() const { return m_rangeSelector.holdPending(); }
    bool rangeHoldTimerActive() const { return m_rangeHoldTimer.isActive(); }
    bool isTransmitting() const { return m_transmitting; }
    bool reflectedPowerMeasured() const { return m_reflectedPowerMeasured; }
    bool rangeLegendVisible() const { return m_rangeLegendVisible; }
    FaceTheme faceTheme() const { return m_faceTheme; }
    QString faceThemeId() const;
    const CrossNeedleMeterGeometry &geometry() const { return m_geometry; }
    QString accessibleValueText() const;

  public slots:
    void setTxPowers(float forwardWatts, float reflectedWatts);
    void setTxMeters(float forwardWatts, float swr);
    void setTransmitting(bool transmitting);
    void setPowerScale(int maxWatts, bool amplifierActive);
    void setFaceTheme(FaceTheme theme);
    void setRangeLegendVisible(bool visible);

    // Test fixture used only by the in-process automation bridge. AppletPanel
    // exposes it only when AETHER_AUTOMATION is set; production UI cannot call
    // this path.
    void setAutomationFixture(float forwardWatts, float swr);
    void setAutomationPowerFixture(float forwardWatts, float reflectedWatts);
    void clearAutomationFixture();

  protected:
    void paintEvent(QPaintEvent *event) override;
    void changeEvent(QEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

  private slots:
    void advanceNeedles();
    void publishAccessibleValue();
    void completeRangeHold();

  private:
    QRectF fittedDesignRect() const;
    void applyDesignTransform(QPainter &painter) const;
    void rebuildStaticLayer();
    void drawFaceBackground(QPainter &painter, const QRectF &face) const;
    void drawFace(QPainter &painter) const;
    void drawScale(QPainter &painter, const CrossNeedleMeterGeometry::Scale &scale,
                   const CrossNeedleMeterGeometry::Title &title) const;
    void drawSwrGuides(QPainter &painter) const;
    void drawNeedles(QPainter &painter) const;
    void drawLowerMask(QPainter &painter) const;
    void drawCenteredText(QPainter &painter, const QPointF &center, const QString &text) const;
    void drawCenteredMultilineText(QPainter &painter, const QPointF &center,
                                   const QString &text) const;
    AnalogMeterFaceTheme analogFaceTheme() const;
    void drawRotatedText(QPainter &painter, const QPointF &center, const QString &text,
                         double degrees) const;
    void updateTargets(bool snap);
    // Feeds the live forward power to the range switch while transmitting.
    // Returns true when the active range changed.
    bool observeRange();
    void scheduleRangeHold();
    // Adopts the selector's range: reprints the face and snaps the needles.
    void applyRangeChange();
    void scheduleAccessibleValue();
    void publishAutomationProperties();

    CrossNeedleMeterGeometry m_geometry;
    AnalogMeterFaceThemeCatalog m_faceThemes;
    MeterSmoother m_forwardSmoother;
    MeterSmoother m_reflectedSmoother;
    QTimer m_animationTimer;
    QElapsedTimer m_animationClock;
    QTimer m_accessibilityTimer;
    CrossNeedleRangeSelector m_rangeSelector;
    QElapsedTimer m_rangeClock;
    // Completes a pending down-range when no further meter sample arrives
    // (a steady carrier on a radio that reports only changes, or a fixture).
    QTimer m_rangeHoldTimer;

    QPixmap m_staticLayer;
    QSize m_cacheSize;
    qreal m_cacheDpr{0.0};
    bool m_cacheValid{false};

    double m_forwardWatts{0.0};
    double m_reflectedWatts{0.0};
    double m_swr{1.0};
    double m_rangeMultiplier{10.0};
    bool m_transmitting{false};
    bool m_reflectedPowerMeasured{false};
    bool m_rangeLegendVisible{true};
    bool m_automationFixture{false};
    FaceTheme m_faceTheme{FaceTheme::DarkRoomUplight};
};

} // namespace AetherSDR
