#pragma once

#include <QWidget>
#include <QPushButton>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QTimer>
#include <QVector>

class QFrame;
class QShortcut;

namespace AetherSDR {
class VoiceKeyer;

class DvkPanel : public QWidget {
    Q_OBJECT
public:
    // The panel drives whichever keyer it is given — the radio DVK or the
    // client-side keyer — through the VoiceKeyer interface only.
    explicit DvkPanel(VoiceKeyer* keyer, QWidget* parent = nullptr);
    // The panel's themed stylesheet, before token resolution.
    static QString styleTemplate();
    // The operator's word for a dvk command verb ("playback_start" → "Play").
    static QString verbLabel(const QString& verb);
    int selectedSlot() const;

    // Bind a different keyer. Every connection is re-made and the whole
    // panel is redrawn from the new keyer, so no state from the old one
    // survives the swap. MainWindow defers this while a keyer is busy.
    void setKeyer(VoiceKeyer* keyer);
    VoiceKeyer* keyer() const { return m_model; }

    // Enable/disable the F1-F12 and Esc ApplicationShortcuts. Driven by
    // the active slice's mode in MainWindow so the keys fire regardless
    // of panel visibility, while staying mutually exclusive with CwxPanel
    // to avoid Qt shortcut ambiguity. (#2582)
    void setShortcutsEnabled(bool enabled);

    // Show one line in the panel's status area. For notices the keyer itself
    // does not raise — a source change waiting on the operation in flight.
    void showNotice(const QString& text, bool error = false);

    // A recording is holding the radio's VOX off. Shown in the recording
    // status, because a radio setting changed that the operator did not change.
    void setVoxHeldOff(bool held);

    // A pending source change, waiting for the operation in flight to end.
    // Carried as a suffix on the status line rather than written once: the
    // elapsed timer rewrites that line every 100 ms during an operation, so a
    // one-shot message would flash and vanish. Cleared when the swap lands.
    void setPendingSourceNotice(const QString& text);

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;

private slots:
    void onStatusChanged(int status, int id);
    void onRecordingChanged(int id);
    void onElapsedTick();

private:
    VoiceKeyer* m_model;
    QVector<QFrame*> m_rowFrames;
    QVector<QPushButton*> m_fkeyBtns;
    QVector<QLabel*> m_nameLabels;
    QVector<QLabel*> m_durLabels;
    QVector<QProgressBar*> m_progressBars;
    QPushButton* m_recBtn;
    QPushButton* m_stopBtn;
    QPushButton* m_playBtn;
    QPushButton* m_prevBtn;
    // The title carries the keyer's source ("(Radio)" / "(Local)") so the
    // operator can always see which keyer the panel drives.
    QLabel* m_titleLabel{nullptr};
    QLabel* m_statusLabel;
    QLabel* m_statusDot{nullptr};
    int m_selectedSlot{1};
    bool m_voxHeldOff{false};
    QString m_pendingSourceNotice;
    QLineEdit* m_renameEdit{nullptr};
    int m_renameSlot{-1};

    // Elapsed timer state
    QTimer* m_elapsedTimer{nullptr};
    int m_elapsedMs{0};
    int m_timerSlotId{-1};
    int m_timerStatus{0};  // VoiceKeyer::Status cast to int

    // F1-F12 + ESC shortcuts — ApplicationShortcut on window(), enabled
    // by MainWindow based on the active slice's mode (mutually exclusive
    // with CwxPanel's set) so they fire regardless of panel visibility
    // (#2464, #2582).
    QVector<QShortcut*> m_shortcuts;


    // Connect the bound keyer's signals, and drop the previous keyer's.
    void connectKeyer();
    // Redraw every slot, the transport and the status from the keyer.
    void refreshFromKeyer();
    void selectSlot(int id);
    // Row and F-key names carry the slot's name and length for screen readers.
    void updateSlotAccessibility(int id, const QString& name, int durationMs);
    // Sets the status text and announces it; the elapsed-time tick does not.
    // `error` shows it in the danger colour (the text always says "failed").
    void announceStatus(const QString& text, bool error = false);
    // What the status line carries beyond the operation itself: a held VOX, a
    // pending source change.
    QString statusSuffix() const;
    void refreshTransport();
    void togglePlayback(int id);
    void stopActiveOperation();
    void showContextMenu(int id, const QPoint& globalPos);
    void startRename(int id);
    void commitRename();
    void cancelRename();
    QString formatDuration(int ms);
    int durationForSlot(int id) const;
    // The slot a stop should target: whatever is running, or the selected
    // slot when the keyer has not yet echoed a start. The interface's stop
    // calls name a slot, because a client-side keyer's operations are
    // per-slot rather than one global transport.
    int activeOrSelected() const;
};

} // namespace AetherSDR
