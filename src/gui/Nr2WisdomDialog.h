#pragma once

#include "CanonWindow.h"

#include <QElapsedTimer>

class QLabel;
class QProgressBar;
class QPushButton;
class QTimer;

namespace AetherSDR {

// The FFTW Wisdom window NR2 shows while the first-use FFT plans are made: a
// canon dialog with the current step, a progress bar, a "Working..." line with
// the elapsed time, a reassurance line when the planner has gone quiet, and
// Cancel. MainWindow runs the planner on a worker thread and feeds it here.
//
// Modeless and a tool window, so it floats above the main window without a
// taskbar entry or stealing focus while the operator keeps working; on macOS
// it stays visible when the app deactivates. Cancel, the corner ✕, Escape and
// Ctrl+W (⌘W) all request cancellation, once. Cancel leaves the window up,
// showing "Canceling..." until the plan in flight finishes; the other three
// also hide it, as QDialog::reject() does. The owner deletes it after
// showResult().
class Nr2WisdomDialog : public CanonWindow {
    Q_OBJECT

public:
    enum class Outcome { Complete, Canceled, Failed };

    explicit Nr2WisdomDialog(QWidget* parent = nullptr);

    // From the planner, on the GUI thread. Each one counts as a sign of life
    // and hides the reassurance line. Ignored once cancellation is requested.
    void setStepDescription(const QString& description);
    void setProgress(int percent);

    // The planner has finished: stop the activity line and show the outcome.
    void showResult(Outcome outcome);

    bool isCancelRequested() const { return m_cancelRequested; }

signals:
    // Emitted once, the first time the operator cancels.
    void cancelRequested();

private:
    void requestCancel();
    void tickActivity();
    void noteActivity();
    static void announce(QLabel* label);

    QLabel*       m_status{nullptr};
    QProgressBar* m_progress{nullptr};
    QLabel*       m_activity{nullptr};
    QLabel*       m_elapsed{nullptr};
    QLabel*       m_reassurance{nullptr};
    QPushButton*  m_cancel{nullptr};
    QTimer*       m_activityTimer{nullptr};
    QElapsedTimer m_elapsedTimer;
    QElapsedTimer m_lastActivity;
    bool          m_cancelRequested{false};
};

} // namespace AetherSDR
