#pragma once

#include <QWidget>
#include <QImage>

#include <functional>
#include <vector>

namespace AetherSDR {

// Self-rendered mini-waterfall for the WebSDR panel. A QImage ring buffer
// scrolled one row per message (newest at the top, below a frequency scale).
// The dead SDR guard bands at the edges (sent as black by the WebSDR) are
// detected from per-column luminance and cropped so the useful spectrum fills
// the width. Overlays the listening marker + passband. Clicks report an
// absolute kHz (crop-aware). Plain QWidget — no QRhi; QPainter only.
//
// Q_OBJECT so QAccessible can identify it by class name (see the factory in
// the .cpp) — required because it overrides paintEvent (a11y rules).
class WebSdrWaterfallView : public QWidget {
    Q_OBJECT
public:
    explicit WebSdrWaterfallView(QWidget* parent = nullptr);

    void setOnClick(std::function<void(double)> cb) { m_onClick = std::move(cb); }
    void setGeometryInfo(double centerKHz, double srKHz);
    void setTuning(double freqKHz, double loKHz, double hiKHz);
    void addRow(const QImage& row);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* e) override;

private:
    void   recomputeCrop();
    double freqAtCol(int c) const;
    double freqLeft() const;
    double freqRight() const;
    double xOf(double freqKHz) const;

    QImage m_img;
    std::function<void(double)> m_onClick;
    std::vector<double> m_colAvg;
    int    m_cropL{0};
    int    m_cropR{0};
    int    m_rowsSinceCrop{0};
    double m_centerKHz{0.0};
    double m_srKHz{0.0};
    double m_tunedKHz{0.0};
    double m_loKHz{0.0};
    double m_hiKHz{0.0};
    bool   m_haveTuning{false};
};

} // namespace AetherSDR
