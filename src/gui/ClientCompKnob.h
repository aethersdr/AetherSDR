#pragma once

#include <QWidget>
#include <QString>
#include <functional>

class QLineEdit;

namespace MasterSDR {

// Shared rotary knob for the compressor editor.  Click-drag vertical to
// change the value, wheel for fine adjustment, double-click to reset to
// the configured default.  Value mapping is user-supplied so the knob
// can be linear (Makeup), exponential (Ratio/Attack/Release), or
// anything else a caller needs.
//
// Value semantics:
//   - Internal state is "normalized" in [0, 1].
//   - setValueFunc/toValueFunc convert between the normalized 0..1 and
//     the caller-facing physical unit.
//   - valueLabelFunc formats the caller-facing value for display.
//   - valueChanged(float) emits the physical value whenever the user
//     moves the knob.
class ClientCompKnob : public QWidget {
    Q_OBJECT

public:
    explicit ClientCompKnob(QWidget* parent = nullptr);

    // Caller-facing label drawn above the knob (e.g. "Ratio", "Attack").
    void setLabel(const QString& text);

    // Physical value range (inclusive).  Only used to clamp external
    // setValue() calls; the actual normalised range is always 0..1.
    void setRange(float minPhysical, float maxPhysical);

    // Default used by double-click reset.
    void setDefault(float physical);

    // Conversion between the 0..1 knob position and the physical value
    // the caller cares about.  Callers install these once after
    // construction; omit them and the knob is linear over [0, 1].
    using ValueMap = std::function<float(float)>;
    void setValueFromNorm(ValueMap fromNorm);
    void setNormFromValue(ValueMap toNorm);

    // Text format for the value label.  Receives the physical value.
    using LabelFormat = std::function<QString(float)>;
    void setLabelFormat(LabelFormat fmt);

    // Center-label mode — draws the parameter label INSIDE the knob
    // ring instead of above it.  Used by the PUDU applet/editor to
    // fit 6 knobs on one row without the label row above eating
    // vertical space.  The value readout still lives below.
    void setCenterLabelMode(bool on);
    bool isCenterLabelMode() const { return m_centerLabel; }

    // Inline value editor — defaults ON for the channel-strip / editor
    // contexts where the knob is large enough (76+ px) for the
    // QLineEdit to fit cleanly.  Applet tiles in the applet bar are
    // ~38 px wide and the inline editor clips visually, so they
    // disable it and fall back to painted value text.
    void setInlineEditEnabled(bool on);
    bool isInlineEditEnabled() const { return m_inlineEdit; }

    // Programmatic setter — e.g. loadClientCompSettings on startup.
    void setValue(float physical);
    float value() const { return m_physical; }

signals:
    void valueChanged(float physical);

protected:
    void paintEvent(QPaintEvent* ev) override;
    void resizeEvent(QResizeEvent* ev) override;
    void mousePressEvent(QMouseEvent* ev) override;
    void mouseMoveEvent(QMouseEvent* ev) override;
    void mouseReleaseEvent(QMouseEvent* ev) override;
    void mouseDoubleClickEvent(QMouseEvent* ev) override;
    void wheelEvent(QWheelEvent* ev) override;
    bool eventFilter(QObject* obj, QEvent* ev) override;

private:
    void applyNorm(float norm);
    QString formatValue() const;
    QRect valueRect() const;          // bottom-area rect for the value text
    void layoutValueEditor();          // position m_valueEdit over valueRect
    void commitValueEdit();            // parse text → setValue → emit
    void refreshValueEditDisplay();    // when not focused: show formatted value

    QString     m_label;
    float       m_minPhys{0.0f};
    float       m_maxPhys{1.0f};
    float       m_defaultPhys{0.0f};
    float       m_norm{0.0f};
    float       m_physical{0.0f};
    ValueMap    m_fromNorm;
    ValueMap    m_toNorm;
    LabelFormat m_fmt;

    bool        m_dragging{false};
    int         m_dragStartY{0};
    float       m_dragStartNorm{0.0f};
    bool        m_centerLabel{false};
    bool        m_inlineEdit{true};

    // Inline value editor — child QLineEdit positioned over the painted
    // value text.  Click to focus, type a number, Enter (or focus-out)
    // commits.  Closes #2655.
    QLineEdit*  m_valueEdit{nullptr};
};

} // namespace MasterSDR
