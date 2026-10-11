#pragma once

#include <QObject>

class QWidget;

namespace AetherSDR {

// Makes a click-handled status-bar indicator (a QLabel, or a small container
// of labels) operable without a mouse, per docs/a11y.md's interactive-QLabel
// rule (#6257): Tab reaches it, Return/Enter activate it, assistive technology
// sees a Button with a Press action (checkable for on/off indicators), and a
// 2 px canon-cyan underline marks keyboard focus. The indicator keeps its own
// styling. Space is left alone: MainWindow binds it to PTT (Hold) app-wide.
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

    // On/off indicators report their state to assistive technology; colour
    // alone is not an accessible state (docs/a11y.md).
    void setCheckable(bool checkable);
    bool isCheckable() const { return m_checkable; }
    void setChecked(bool checked);
    bool isChecked() const { return m_checked; }
    // setChecked() on `widget`'s helper, if it has one.
    static void setCheckedFor(QWidget* widget, bool checked);

signals:
    void activated();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    explicit StatusIndicator(QWidget* widget);
    void placeFocusBar();

    QWidget* m_widget;
    QWidget* m_focusBar;
    bool m_checkable{false};
    bool m_checked{false};
};

}  // namespace AetherSDR
