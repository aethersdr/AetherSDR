#pragma once

#include <QWidget>
#include <QString>
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
class QResizeEvent;
class QPaintEvent;

namespace AetherSDR {

class CwxModel;

// Painted history bubble for one outgoing CW message.  Both manual sends
// and F-key macro fires produce one; the panel tracks the most recent
// in-flight bubble so `sent=N` status updates advance its sent count
// and an ESC abort flips it to render the unsent suffix with strikeout
// (#3146).
class CwxBubble : public QWidget {
public:
    CwxBubble(const QString& text, const QString& time, QWidget* parent = nullptr);

    QString text() const { return m_text; }
    int     sentCount() const { return m_sentCount; }
    bool    isAborted() const { return m_aborted; }

    void setSentCount(int n);
    void markAborted();

protected:
    void resizeEvent(QResizeEvent*) override;
    void paintEvent(QPaintEvent*) override;

private:
    void recalcSize();

    QString m_text;
    QString m_time;
    int     m_sentCount{0};
    bool    m_aborted{false};
};

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

    // Inspection accessors for the in-flight bubble — used by tests to
    // verify the macro-history + ESC-strikeout behavior wired in #3146
    // without re-walking the history container ourselves.
    CwxBubble* pendingBubble() const { return m_pendingBubble; }
    int        historyBubbleCount() const;

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;

private slots:
    void onCharSent(int index);
    void onSpeedChanged(int wpm);
    void onTransmissionCancelled();

private:
    void buildSendView();
    void buildSetupView();
    void showSendView();
    void showSetupView();
    void sendBuffer();
    void resendText(const QString& text);
    void clearHistory();
    void appendHistoryBubble(const QString& text);
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

    // In-flight bubble tracking — manual sends and macro fires both
    // populate these.  A single pending pointer is the v1 scope; the
    // queue-based <radio_index>,<block> mapping from CWX.cs:54-83 is
    // out of scope for the contest workflow (one macro at a time). (#3146)
    CwxBubble*      m_pendingBubble{nullptr};
    int             m_pendingStartIndex{-1};
    QString         m_pendingText;

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
