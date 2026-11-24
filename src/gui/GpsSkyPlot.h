#pragma once

#include "core/GpsdSkyClient.h"

#include <QList>
#include <QWidget>

namespace AetherSDR {

// AetherGPS's sky plot: the radio's view of the sky from a gpsd SKY report.
// Zenith at the center, the horizon at the rim, north up and east right,
// with rings at 30 and 60 degrees elevation. Each satellite is a dot in its
// constellation's color: filled when used in the fix, a ring when heard but
// not used, and faint when listed but not heard. Heard satellites are
// labeled with their PRN, as in AetherGPS's table.
class GpsSkyPlot : public QWidget {
    Q_OBJECT

public:
    explicit GpsSkyPlot(QWidget* parent = nullptr);

    void setSatellites(const QList<GpsSatellite>& satellites);
    const QList<GpsSatellite>& satellites() const { return m_satellites; }

    // Where a satellite is drawn inside `plot` (the circle's bounding
    // square). Exposed for tests: the projection is the plot's contract.
    static QPointF project(const QRectF& plot, double elevationDeg, double azimuthDeg);
    // The theme token for a constellation's color.
    static QString colorToken(gpsdsky::Constellation c);

    QSize sizeHint() const override { return {300, 300}; }
    QSize minimumSizeHint() const override { return {200, 200}; }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override { return width; }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QList<GpsSatellite> m_satellites;
};

}  // namespace AetherSDR
