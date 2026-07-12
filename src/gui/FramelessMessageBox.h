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

    FramelessWindowTitleBar* m_titleBar{nullptr};
    QMargins m_originalMargins;
};

} // namespace AetherSDR
