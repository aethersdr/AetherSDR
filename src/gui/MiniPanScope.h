#pragma once

// MiniPanScope — the slim K4-style spectrum trace at the heart of the mini-pan.
//
// A deliberately bare tuning-aid render: dark field, a translucent passband band,
// a centre hairline, a filled FFT trace, and ±span corner labels. NO overlay menu,
// dBm strip, waterfall or time axis — everything SpectrumWidget carries that a
// tuning aid does not want (which is why the mini-pan does not reuse it; see
// docs/minipan-implementation.md §4).
//
// The public API below is frozen: PR2 feeds it live bins via updateSpectrum() and
// drives span/passband from the followed slice without changing this surface.

#include <QWidget>
#include <QVector>

namespace AetherSDR {

class MiniPanScope : public QWidget {
    Q_OBJECT
public:
    explicit MiniPanScope(QWidget* parent = nullptr);

    // Latest dBm bins spanning the full narrow view; resampled across the width.
    void updateSpectrum(const QVector<float>& binsDbm);
    // Vertical dBm scale (top = max, bottom = min).
    void setDbmRange(float minDbm, float maxDbm);
    // Total visible span in kHz (e.g. 10.0 for a ±5 kHz view).
    void setSpanKHz(double kHz);
    // Passband band as Hz offsets from centre (e.g. USB 100..2800). lo>=hi hides it.
    void setPassbandHz(int lowHz, int highHz);

protected:
    void paintEvent(QPaintEvent* e) override;

private:
    // Auto-scale headroom: park the noise floor ~75% down (kHeadroomDb of the
    // kHeadroomDb+kFloorMarginDb window sits above it) with room for signals above.
    static constexpr float kHeadroomDb    = 45.0f;
    static constexpr float kFloorMarginDb = 15.0f;

    QVector<float> m_bins;
    float  m_minDbm{-130.0f};   // fixed fallback until the first bins arrive
    float  m_maxDbm{-40.0f};
    float  m_noiseFloorDbm{-110.0f};
    bool   m_noiseFloorValid{false};
    double m_spanKHz{10.0};
    int    m_pbLoHz{0};
    int    m_pbHiHz{0};
};

} // namespace AetherSDR
