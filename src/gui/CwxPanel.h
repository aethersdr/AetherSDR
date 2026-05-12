#pragma once

#include <QWidget>
#include <QVector>
#include <functional>

class QPushButton;
class QTextEdit;
class QSpinBox;
class QLabel;
class QLineEdit;
class QStackedWidget;
class QScrollArea;
class QVBoxLayout;
class QShortcut;

namespace AetherSDR {

class CwxModel;

class CwxPanel : public QWidget {
    Q_OBJECT
public:
    explicit CwxPanel(CwxModel* model, QWidget* parent = nullptr);

    void setModel(CwxModel* model);

    // Optional providers used to guard the global F1-F12 / ESC shortcuts
    // so they don't fire in modes/states where they'd be surprising (#1552).
    //  - modeProvider returns the active slice's mode ("CW", "CWL", ...)
    //  - transmittingProvider returns true when the radio is actively TXing
    // When unset, the shortcuts fire unconditionally (legacy behavior).
    void setActiveModeProvider(std::function<QString()> provider) {
        m_activeModeProvider = std::move(provider);
    }
    void setTransmittingProvider(std::function<bool()> provider) {
        m_transmittingProvider = std::move(provider);
    }

    // Enable/disable the F1-F12 and Esc ApplicationShortcuts. Driven by
    // the active slice's mode in MainWindow, so the keys fire whether
    // the panel is visible or not. (#2582)
    void setShortcutsEnabled(bool enabled);

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;

private slots:
    void onCharSent(int index);
    void onSpeedChanged(int wpm);

private:
    void buildSendView();
    void buildSetupView();
    void showSendView();
    void showSetupView();
    void sendBuffer();
    void onKeyPress(const QString& text);

    CwxModel*       m_model{nullptr};

    QStackedWidget* m_stack{nullptr};

    // Send/Live view
    QWidget*        m_sendPage{nullptr};
    QScrollArea*    m_historyScroll{nullptr};
    QWidget*        m_historyContainer{nullptr};
    QVBoxLayout*    m_historyLayout{nullptr};
    QTextEdit*      m_textEdit{nullptr};     // input area at bottom
    int             m_sendStartIndex{0};     // cumulative index offset for highlighting

    // Setup view
    QWidget*        m_setupPage{nullptr};
    QTextEdit*      m_macroEdits[12]{};
    QSpinBox*       m_delaySpin{nullptr};
    QPushButton*    m_qskBtn{nullptr};

    // Bottom bar
    QPushButton*    m_sendBtn{nullptr};
    QPushButton*    m_liveBtn{nullptr};
    QPushButton*    m_setupBtn{nullptr};
    QSpinBox*       m_speedSpin{nullptr};

    std::function<QString()> m_activeModeProvider;
    std::function<bool()>    m_transmittingProvider;

    // F1-F12 + ESC shortcuts — enabled by MainWindow based on the active
    // slice's mode so they fire regardless of panel visibility, while
    // staying mutually exclusive with DvkPanel's F1-F12 set to avoid Qt
    // shortcut ambiguity (#2464, #2582).
    QVector<QShortcut*> m_shortcuts;
};

} // namespace AetherSDR
