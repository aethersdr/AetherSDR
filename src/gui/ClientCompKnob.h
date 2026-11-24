#pragma once

#include <QWidget>
#include <QString>
#include <QTimer>
#include <functional>

class QLineEdit;
class QKeyEvent;
class QFocusEvent;

namespace AetherSDR {

// Rotary knob for client DSP: vertical drag, wheel or arrow-key adjustment,
// double-click resets to default. State is normalized [0, 1];
// setValueFunc/toValueFunc map to the physical unit (linear, exponential,
// ...), valueLabelFunc formats it, and valueChanged(float) emits the physical
// value.
class ClientCompKnob : public QWidget {
    Q_OBJECT

public:
    explicit ClientCompKnob(QWidget* parent = nullptr);

    // Caller-facing label drawn above the knob (e.g. "Ratio", "Attack").
    void setLabel(const QString& text);

    // Spoken name when the painted label is ambiguous out of context, e.g.
    // "Body mix" for a "Mix" knob under a "Body" heading. Names the knob and
    // its inline value editor; setLabel() keeps it once set.
    void setAccessibleLabel(const QString& name);

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

    // Discrete controls (e.g. compressor Phase stages): arrow keys and the
    // accessible increase/decrease actions move by this many physical units.
    // 0 (the default) steps by 1% of travel. The mouse wheel steps the same
    // way, and Shift gives no finer step on a discrete knob.
    void setPhysicalStep(float step);

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
    bool event(QEvent* ev) override;
    bool eventFilter(QObject* obj, QEvent* ev) override;
    void keyPressEvent(QKeyEvent* ev) override;
    void focusInEvent(QFocusEvent* ev) override;
    void focusOutEvent(QFocusEvent* ev) override;

private:
    friend class ClientCompKnobAccessible;
    void applyNorm(float norm);
    void setValueFromUser(float physical);
    void stepBy(int direction, bool fine);
    bool valueEditorHasFocus() const;
    void pageBy(int direction);
    static QString continuousKeyDescription();
    void applyAccessibleLabel(const QString& name);
    void scheduleAccessibleAnnouncement();
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
    QTimer      m_accessibilityTimer;
    QString     m_accessibleLabel;
    float       m_physicalStep{0.0f};
    QString     m_lastAccessibleText;
};

} // namespace AetherSDR
