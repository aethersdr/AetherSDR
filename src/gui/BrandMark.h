#pragma once

#include <QPixmap>
#include <QWidget>

class QPainter;

namespace AetherSDR {

// Brand cluster for the unified title bar: the circular logo mark followed by
// the "AetherSDR" wordmark, with "SDR" filled by the brand gradient.
//
// Painter-driven rather than two QLabels because the gradient fill has to be
// clipped to the glyph outlines of the second word only — a stylesheet can
// colour a whole label but cannot split one run of text across two brushes.
//
// The mark is decorative and the wordmark is the app's own name, so the widget
// exposes a single accessible name and takes no focus; the window title carries
// the same string for screen readers that read the window rather than its
// children.
class BrandMark : public QWidget {
    Q_OBJECT

public:
    explicit BrandMark(QWidget* parent = nullptr);

    // Diameter of the circular logo mark in device-independent pixels.
    static constexpr int kLogoDiameter = 22;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override { return sizeHint(); }

    // Introspection for the automation bridge (`titlebar` model).
    QString wordmarkText() const;
    bool    hasLogo() const { return !m_logo.isNull(); }

    // Paints the wordmark ("Aether" in color.brand.wordmark, "SDR" filled
    // with color.brand.gradient) in `font`, starting at `x` on `baseline`.
    // The gradient spans the "SDR" run across `height`, the painted widget's
    // height. `scope` resolves the theme tokens. Shared so every place that
    // shows the wordmark draws it the same way.
    static void paintWordmark(QPainter& p, const QWidget* scope, const QFont& font,
                              qreal x, qreal baseline, qreal height);
    static qreal wordmarkWidth(const QFont& font);

protected:
    void paintEvent(QPaintEvent* ev) override;

private:
    QFont wordmarkFont() const;
    void  rebuildLogo();

    QPixmap m_logo;      // pre-scaled, circular-clipped, DPR-aware
    qreal   m_logoDpr{0.0};
};

} // namespace AetherSDR
