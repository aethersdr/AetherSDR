#pragma once

#include "core/txlinearity/TxLinearityTypes.h"

#include <QWidget>

class QPaintEvent;

namespace AetherSDR {

// The marked TX spectrum — the core of the linearity analyzer's display.
//
// A raw spectrum picture is what SmartSDR already gives an operator. What it
// does not give is which peak is which: this view labels f1 and f2, marks every
// computed IMD product with its order and its level in dBc, draws the measured
// instrument noise floor as a line, and shades the shoulder and ACPR
// integration bands so the operator can see exactly what was integrated rather
// than trusting an offset label in a table. A marked spectrum beats a raw
// picture, and it is the difference between a display and an instrument.
//
// Deliberately a plain QWidget with a paintEvent, not a QRhiWidget like
// SpectrumWidget. That one is a display path drawing tens of thousands of bins
// at frame rate and needs the GPU; this draws one static averaged trace when a
// measurement completes. Reaching for the GPU path here would inherit its
// context-loss and readback machinery for a plot that repaints a few times a
// minute.
class TxLinearitySpectrumView : public QWidget {
    Q_OBJECT

public:
    explicit TxLinearitySpectrumView(QWidget* parent = nullptr);

    // Show a completed measurement. An invalid result clears the plot and
    // displays the refusal reason, because a stale trace next to a failed
    // measurement is how an operator ends up reading last run's numbers.
    void setResult(const txlin::LinearityResult& result);
    void clear();

    // Overlay a second, dimmed trace for A/B comparison (SmartSignal off vs
    // on). Phase 2 drives this; the view supports it now so the Phase 2 change
    // is a call site rather than a rewrite of the paint code.
    void setReferenceResult(const txlin::LinearityResult& result);
    void clearReferenceResult();

    // Vertical span, dBFS. Defaults cover a full-scale tone down to below any
    // realistic instrument floor.
    void setDbRange(double topDb, double bottomDb);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    // Baseband Hz -> x pixel, dBFS -> y pixel, over the plot rect.
    [[nodiscard]] double freqToX(double hz, const QRectF& plot) const;
    [[nodiscard]] double dbToY(double db, const QRectF& plot) const;

    void drawGrid(QPainter& painter, const QRectF& plot) const;
    void drawTrace(QPainter& painter, const QRectF& plot,
                   const txlin::LinearityResult& result, bool dimmed) const;
    void drawBands(QPainter& painter, const QRectF& plot) const;
    void drawMarkers(QPainter& painter, const QRectF& plot) const;
    void updateAccessibleSummary();

    txlin::LinearityResult m_result;
    txlin::LinearityResult m_reference;
    bool m_hasResult = false;
    bool m_hasReference = false;

    double m_topDb = 0.0;
    double m_bottomDb = -140.0;
    // Horizontal span, Hz either side of the two-tone centre. Recomputed from
    // the result so the products always fit with margin, rather than being a
    // fixed span the operator has to zoom.
    double m_spanHz = 20000.0;
    double m_centreHz = 0.0;
};

}  // namespace AetherSDR
