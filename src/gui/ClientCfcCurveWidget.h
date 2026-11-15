#pragma once

#include "core/ClientCfc.h"

#include <QWidget>

#include <array>
#include <vector>

namespace AetherSDR {

// The CFC's two curves over a log frequency axis: compression (0..20 dB,
// filled points) and post-EQ (+/-20 dB, hollow points), with the live
// compression drawn behind them. Interactive unless set read-only: drag a
// point to move its band in frequency and that curve in level, double-click
// empty space to add a band, right-click a point to remove its band.
class ClientCfcCurveWidget : public QWidget {
    Q_OBJECT

public:
    enum class Curve { Compression, PostEq };

    explicit ClientCfcCurveWidget(QWidget* parent = nullptr);

    void setBands(const std::vector<ClientCfc::Band>& bands);
    const std::vector<ClientCfc::Band>& bands() const { return m_bands; }
    void setPostEqActive(bool active);
    void setCompression(const std::array<float, ClientCfc::kDisplayPoints>& db);
    void clearCompression();
    void setReadOnly(bool readOnly) { m_readOnly = readOnly; }
    int selectedBand() const { return m_selected; }
    void setSelectedBand(int index);

    QSize sizeHint() const override { return {560, 220}; }
    QSize minimumSizeHint() const override { return {240, 90}; }

signals:
    // The operator changed the bands (drag, add or remove).
    void bandsEdited(const std::vector<AetherSDR::ClientCfc::Band>& bands);
    void selectedBandChanged(int index);

protected:
    void paintEvent(QPaintEvent* ev) override;
    void mousePressEvent(QMouseEvent* ev) override;
    void mouseMoveEvent(QMouseEvent* ev) override;
    void mouseReleaseEvent(QMouseEvent* ev) override;
    void mouseDoubleClickEvent(QMouseEvent* ev) override;
    void contextMenuEvent(QContextMenuEvent* ev) override;

private:
    QRectF plotRect() const;
    double freqToX(double hz) const;
    double xToFreq(double x) const;
    double dbToY(double db) const;
    double yToDb(double y) const;
    QPointF pointFor(int band, Curve curve) const;
    // The band and curve whose point is under `pos`, or -1.
    int hitTest(const QPointF& pos, Curve* curve) const;
    // The curve's value at `hz`, linear between bands as cfcomp interpolates.
    double interpolate(double hz, Curve curve) const;

    std::vector<ClientCfc::Band> m_bands;
    std::array<float, ClientCfc::kDisplayPoints> m_compression{};
    bool m_hasCompression{false};
    bool m_postEqActive{false};
    bool m_readOnly{false};
    int m_selected{-1};
    int m_dragBand{-1};
    Curve m_dragCurve{Curve::Compression};
};

}  // namespace AetherSDR
