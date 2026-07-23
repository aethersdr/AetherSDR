#pragma once

#include <QMessageBox>

namespace AetherSDR {

class FramelessWindowTitleBar;

// QMessageBox with the same optional custom chrome as PersistentDialog.
// These prompts are transient, so they intentionally do not persist geometry.
class FramelessMessageBox : public QMessageBox {
    Q_OBJECT

public:
    explicit FramelessMessageBox(QWidget* parent = nullptr);

    void setFramelessMode(bool on);

    static StandardButton information(QWidget* parent, const QString& title,
                                      const QString& text,
                                      StandardButtons buttons = Ok,
                                      StandardButton defaultButton = NoButton);
    static StandardButton warning(QWidget* parent, const QString& title,
                                  const QString& text,
                                  StandardButtons buttons = Ok,
                                  StandardButton defaultButton = NoButton);
    static StandardButton critical(QWidget* parent, const QString& title,
                                   const QString& text,
                                   StandardButtons buttons = Ok,
                                   StandardButton defaultButton = NoButton);
    static StandardButton question(QWidget* parent, const QString& title,
                                   const QString& text,
                                   StandardButtons buttons = StandardButtons(Yes | No),
                                   StandardButton defaultButton = NoButton);

protected:
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    static StandardButton showMessage(Icon icon, QWidget* parent,
                                      const QString& title, const QString& text,
                                      StandardButtons buttons,
                                      StandardButton defaultButton);
    void positionTitleBar();
    // Reserve space for the title-bar overlay by growing the layout's top
    // content margin. Must be re-applied after QMessageBox rebuilds its grid
    // (setIcon/setText/addButton/... all trigger a rebuild that resets margins).
    void applyTitleBarMargins();

    FramelessWindowTitleBar* m_titleBar{nullptr};
    QMargins m_originalMargins;
    bool m_frameless{false};
};

} // namespace AetherSDR
