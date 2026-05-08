#pragma once

#include <QPoint>
#include <QString>
#include <QWidget>

class QLabel;
class QPushButton;

namespace AetherSDR {

// Header strip that sits at the top of every ContainerWidget.  Owns
// the title text, float/dock toggle button, close (hide) button,
// and acts as a drag handle for reorder-by-drag.  18 px tall, dark
// gradient, 10 px bold title.
class ContainerTitleBar : public QWidget {
    Q_OBJECT

public:
    static constexpr int kHeight = 18;

    explicit ContainerTitleBar(const QString& title, QWidget* parent = nullptr);

    void setTitle(const QString& title);
    QString title() const;

    // Swap the float-toggle button's icon between "float" and "dock"
    // glyphs.  Called by the owning ContainerWidget whenever its dock
    // mode changes.
    void setFloatingState(bool isFloating);

    // Show/hide the close (hide) button.  Root containers typically
    // don't want a close button (hiding the whole sidebar is done
    // through the menu bar instead).
    void setCloseButtonVisible(bool visible);

    // Sync the pin button's pressed state.  The pin button is only
    // visible while the container is in floating mode (mirrors the
    // float-button visibility in setFloatingState()).
    void setAlwaysOnTopState(bool on);
    bool alwaysOnTopState() const { return m_alwaysOnTop; }

signals:
    void floatToggleClicked();
    void closeClicked();
    void alwaysOnTopToggled(bool on);
    void dragStartRequested(const QPoint& globalPos);

protected:
    void mousePressEvent(QMouseEvent* ev) override;
    void mouseMoveEvent(QMouseEvent* ev) override;

private:
    QLabel*      m_titleLabel{nullptr};
    QPushButton* m_pinBtn{nullptr};
    QPushButton* m_floatBtn{nullptr};
    QPushButton* m_closeBtn{nullptr};
    QPoint       m_pressPos;
    bool         m_pressed{false};
    bool         m_closeAllowed{true};   // false = explicitly disabled (sidebar)
    bool         m_isFloating{false};
    bool         m_alwaysOnTop{false};
};

} // namespace AetherSDR
