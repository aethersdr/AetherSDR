#pragma once

// MiniPanScope — bare tuning-aid trace: passband band, carrier hairline, filled
// FFT, ±span labels; none of SpectrumWidget's overlays
// (docs/minipan-implementation.md §3). Centred on the passband centre
// (MiniPan::passbandCenterOffsetHz). MainWindow pushes the source pan's FFT
// colours and dBm window each frame so it mirrors the main pan; only the
// passband, hairline and labels use theme tokens.

#include <QWidget>
#include <QVector>
#include <QColor>
#include <QString>

namespace AetherSDR {

class MiniPanScope : public QWidget {
    Q_OBJECT
public:
    explicit MiniPanScope(QWidget* parent = nullptr);

    // Latest dBm bins spanning the full narrow view; resampled across the width.
    void updateSpectrum(const QVector<float>& binsDbm);
    // Vertical dBm scale (top = max, bottom = min). Mirrored from the source
    // pan, so it carries BOTH the operator's FFT Scale (the dBm strip) and the
    // FFT Floor slider — the floor control works by moving this window.
    void setDbmRange(float minDbm, float maxDbm);
    // Total visible span in kHz (e.g. 10.0 for a ±5 kHz view).
    void setSpanKHz(double kHz);
    // VFO (carrier) frequency: the readout drawn in the top row between the
    // ±span labels, and the frequency the hairline marks. 0 renders the
    // placeholder. Drawn here rather than in a QLabel above the scope so the
    // trace gets the full height of the tile.
    //
    // NOT the centre of the view — that is the passband centre, which this and
    // setPassbandHz() together determine.
    void setVfoMhz(double mhz);
    // The readout exactly as drawn — for tests and automation, which have no
    // QLabel to read now.
    QString readoutText() const;
    // Passband as Hz offsets from the CARRIER (e.g. USB 100..2800), exactly as
    // SliceModel reports them. lo>=hi hides the band and falls the view back to
    // carrier-centred. Its midpoint is what the view centres on, so the band
    // itself always renders symmetrically about the middle of the widget.
    void setPassbandHz(int lowHz, int highHz);
    // Mirror the source pan's FFT trace appearance (FFT Line / FFT Fill).
    // An invalid line or fill colour falls back to the theme's spectrum token,
    // which is what an un-mirrored scope (no source widget) renders with.
    void setTraceAppearance(const QColor& lineColor, const QColor& fillColor,
                            float fillAlpha, float lineWidth);
    // Mirror Heat Map: colour the trace by intensity (the shared ramp in
    // gui/FftHeatMap.h) instead of the flat FFT Line/Fill colours.
    void setHeatMap(bool on);
    // Mirror Grid: the horizontal dB rules.
    void setShowGrid(bool on);

protected:
    void paintEvent(QPaintEvent* e) override;

private:
    QVector<float> m_bins;
    double m_vfoMhz{0.0};
    float  m_minDbm{-130.0f};
    float  m_maxDbm{-40.0f};
    double m_spanKHz{10.0};
    int    m_pbLoHz{0};
    int    m_pbHiHz{0};

    // Invalid until MainWindow mirrors a source pan.
    QColor m_lineColor;
    QColor m_fillColor;
    float  m_fillAlpha{0.70f};
    float  m_lineWidth{1.2f};
    bool   m_heatMap{false};
    bool   m_showGrid{true};
};

} // namespace AetherSDR
