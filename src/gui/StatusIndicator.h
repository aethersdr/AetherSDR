#pragma once

#include <QObject>

class QWidget;

namespace AetherSDR {

// Makes a click-handled status-bar indicator (a QLabel, or a small container
// of labels) operable without a mouse, per docs/a11y.md's interactive-QLabel
// rule (#6257): Tab reaches it, Return/Enter/Space activate it, assistive
// technology sees a Button with a Press action, and a 2 px canon-cyan
// underline marks keyboard focus. The indicator keeps its own styling.
class StatusIndicator : public QObject {
    Q_OBJECT
public:
    // Attaches to `widget` (once; a second call returns the same helper).
    static StatusIndicator* attach(QWidget* widget);
    // The helper attached to `widget`, or nullptr.
    static StatusIndicator* of(const QWidget* widget);

    QWidget* widget() const { return m_widget; }
    // Runs the indicator's action if it is enabled (keys and the Press action).
    void activate();

signals:
    void activated();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    explicit StatusIndicator(QWidget* widget);
    void placeFocusBar();

    QWidget* m_widget;
    QWidget* m_focusBar;
};

}  // namespace AetherSDR
