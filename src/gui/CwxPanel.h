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
    // the active slice's mode in MainWindow. Esc (CW-abort) fires whether
    // the panel is visible or not; the F1-F12 macro fires are additionally
    // gated on panel visibility at fire time so a hidden keyer can't
    // transmit stored macros. (#2582, #3514)
    void setShortcutsEnabled(bool enabled);

    // Inspection accessors for the in-flight bubble — used by tests to
    // verify the macro-history + ESC-strikeout behavior wired in #3146
    // without re-walking the history container ourselves.
    CwxBubble* pendingBubble() const { return m_pendingBubble; }
    int        historyBubbleCount() const;

    // Test seam: fire the F(index+1) macro exactly as its ApplicationShortcut
    // does, including the visibility/mode guards, so the #3514 regression
    // test can exercise the guard without Qt shortcut dispatch (which won't
    // route to a hidden widget headlessly). (0-based: 0=F1, 11=F12)
    void fireMacroForTest(int index) { fireMacro(index); }

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;

private slots:
    void onCharSent(int index);
    void onSpeedChanged(int wpm);
    void onTransmissionCancelled();

private:
    void fireMacro(int index);   // #3514: guarded F1-F12 macro fire (0-based)
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
    // slice's mode, staying mutually exclusive with DvkPanel's F1-F12 set
    // to avoid Qt shortcut ambiguity (#2464, #2582). The macro fires are
    // additionally gated on panel visibility at fire time (#3514).
    QVector<QShortcut*> m_shortcuts;
};

} // namespace AetherSDR
