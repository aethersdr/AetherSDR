#pragma once

#include <QColor>
#include <QWidget>

class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;

namespace AetherSDR {

// Internal sub-widgets — paint-and-drag for hue / saturation+value /
// alpha.  All three emit "value changed" signals so the composite picker
// can stay in sync without polling.
class SVSquare : public QWidget {
    Q_OBJECT
public:
    explicit SVSquare(QWidget* parent = nullptr);
    void setHue(int h);            // 0–359
    void setSV(int s, int v);      // 0–255 each
    int saturation() const { return m_s; }
    int value()      const { return m_v; }

signals:
    void svPicked(int s, int v);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void resizeEvent(QResizeEvent*) override;

private:
    void pickFromMouse(const QPoint& p);
    void rebuildCache();

    int m_h{180};
    int m_s{255};
    int m_v{255};
    QImage m_cache;  // pre-rendered SV plane at current hue
};

class HueStrip : public QWidget {
    Q_OBJECT
public:
    explicit HueStrip(QWidget* parent = nullptr);
    void setHue(int h);    // 0–359
    int  hue() const { return m_h; }

signals:
    void huePicked(int h);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;

private:
    void pickFromMouse(const QPoint& p);
    int m_h{180};
};

class AlphaSlider : public QWidget {
    Q_OBJECT
public:
    explicit AlphaSlider(QWidget* parent = nullptr);
    void setAlpha(int a);                 // 0–255
    void setBaseColor(const QColor& c);   // R/G/B at alpha 255
    int  alpha() const { return m_a; }

signals:
    void alphaPicked(int a);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;

private:
    void pickFromMouse(const QPoint& p);
    int    m_a{255};
    QColor m_base{0, 180, 216};
};


// Composite colour picker — SV square + hue strip + alpha slider + hex
// + RGB inputs + a live swatch.  All controls stay in sync; the widget
// emits colourChanged() whenever the operator touches anything.
//
// Used inline by TokenEditorWidget to edit both scalar colour tokens
// and individual gradient stop colours without spawning a separate
// QColorDialog window.  Roughly 280×260 px; resize-friendly.
class CompactColorPicker : public QWidget {
    Q_OBJECT
public:
    explicit CompactColorPicker(QWidget* parent = nullptr);

    QColor color() const { return m_color; }
    void   setColor(const QColor& c);

signals:
    void colorChanged(const QColor& c);

private slots:
    void onSVPicked(int s, int v);
    void onHuePicked(int h);
    void onAlphaPicked(int a);
    void onHexEdited();
    void onRgbEdited();
    void onEyedropperClicked();

private:
    void rebuildFromColor();   // sync sub-widgets from m_color
    void emitChange();         // emit colorChanged + refresh swatch

    QColor      m_color{0, 180, 216, 255};
    SVSquare*   m_sv{nullptr};
    HueStrip*   m_hue{nullptr};
    AlphaSlider* m_alpha{nullptr};
    QLineEdit*   m_hex{nullptr};
    QPushButton* m_eyedropper{nullptr};
    QSpinBox*    m_r{nullptr};
    QSpinBox*    m_g{nullptr};
    QSpinBox*    m_b{nullptr};
    QLabel*      m_swatch{nullptr};
    bool         m_updating{false};   // re-entrancy guard during sync
    bool         m_pickingScreen{false};  // grabMouse/grabKeyboard active

protected:
    bool event(QEvent* ev) override;  // intercepts events during screen pick
};

} // namespace AetherSDR
