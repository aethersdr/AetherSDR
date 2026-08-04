// MainWindow_Recording.cpp — window video recording subsystem (#3351 sibling TU).
//
// Creation, UI/state wiring and the close-while-recording handshake for
// WindowVideoRecorder. Kept out of MainWindow.cpp per the decomposition rule in
// AGENTS.md ("Adding code to MainWindow").
//
// Includes are carried explicitly: the Linux CI image is on Qt 6.8.3 while
// macOS runs 6.11.x, so transitive resolution on the newer Qt proves nothing.

#include "MainWindow.h"
#include "TitleBar.h"
#include "WindowVideoRecorder.h"

#include "core/AudioEngine.h"
#include "models/RadioModel.h"
#include "models/TransmitModel.h"

#include <QDir>
#include <QStatusBar>
#include <QString>
#include <QTimer>

namespace AetherSDR {

namespace {
// Upper bound on how long close() waits for the encoder to finalize. The Qt
// Multimedia path finalizes asynchronously from recorderStateChanged; if that
// state change never arrives the window is already hidden and every subsequent
// close() is ignored, leaving a process only `kill` can end.
constexpr int kRecorderStopWatchdogMs = 5000;
} // namespace

void MainWindow::wireWindowVideoRecorder()
{
    if (m_windowVideoRecorder) {
        return;
    }

    m_windowVideoRecorder = new WindowVideoRecorder(this, this);

    connect(m_windowVideoRecorder, &WindowVideoRecorder::recordingStarted,
            this, [this](const QString& filePath) {
        Q_UNUSED(filePath);
        m_windowRecorderHadError = false;
        m_titleBar->setRecordWindowEnabled(true);
    });
    connect(m_windowVideoRecorder, &WindowVideoRecorder::recordingStopped,
            this, [this](const QString& filePath, int durationSecs) {
        if (!m_windowRecorderHadError) {
            statusBar()->showMessage(
                tr("🎬 Video saved (%1s): %2")
                    .arg(durationSecs)
                    .arg(QDir::toNativeSeparators(filePath)),
                5000);
        }
        m_titleBar->setRecordWindowEnabled(false);
    });
    connect(m_windowVideoRecorder, &WindowVideoRecorder::recordingError,
            this, [this](const QString& message) {
        m_windowRecorderHadError = true;
        statusBar()->showMessage(tr("🎬 Recording error: %1").arg(message), 5000);
        m_titleBar->setRecordWindowEnabled(false);
        // Ensure the encoder session stops; UI unchecked must match capture state.
        if (m_windowVideoRecorder && m_windowVideoRecorder->isRecording()) {
            m_windowVideoRecorder->stopRecording();
        }
    });

    // ── Audio feeds ──────────────────────────────────────────────────────
    // RX rides RadioModel's normalized bus, NOT PanadapterStream, for the same
    // two reasons the QSO recorder was moved off the stream (see
    // wireRxDemodAudioSinks()): a backend without a PanadapterStream (HL2,
    // KiwiSDR) has no such signal at all, and the stream is destroyed by
    // teardownBackend() on every family swap, which would silently kill the tap
    // for the rest of the session. RadioModel outlives the swap.
    connect(&m_radioModel, &RadioModel::rxDemodAudioReady,
            m_windowVideoRecorder, &WindowVideoRecorder::feedRxAudio);
    connect(m_audio, &AudioEngine::txFinalMonitorPcmReady,
            m_windowVideoRecorder, &WindowVideoRecorder::feedTxAudio);
    connect(&m_radioModel.transmitModel(), &TransmitModel::moxChanged,
            m_windowVideoRecorder, &WindowVideoRecorder::onMoxChanged);
    connect(m_audio, &AudioEngine::cwSidetoneRecordPcmReady,
            m_windowVideoRecorder, &WindowVideoRecorder::feedTxAudio);
    connect(m_audio, &AudioEngine::cwRecordingActiveChanged,
            m_windowVideoRecorder, &WindowVideoRecorder::onMoxChanged);

    connect(m_titleBar, &TitleBar::recordWindowToggled,
            this, &MainWindow::onRecordWindowToggled);
}

void MainWindow::onRecordWindowToggled(bool on)
{
    if (!m_windowVideoRecorder) {
        return;
    }
    if (on) {
        // TitleBar already checked the button before this slot runs. Only leave
        // it on when a session was actually armed — refuse during finalize or
        // on immediate open failure would otherwise leave a stuck "REC" state.
        if (!m_windowVideoRecorder->startRecording()) {
            m_titleBar->setRecordWindowEnabled(false);
            if (m_windowVideoRecorder->isSessionActive()) {
                statusBar()->showMessage(
                    tr("🎬 Recording still finalizing — try again in a moment."),
                    3000);
            }
        }
    } else {
        m_windowVideoRecorder->stopRecording();
    }
}

bool MainWindow::deferCloseForWindowRecorder()
{
    // The watchdog below already gave up on this session; never defer again.
    if (m_recorderCloseForced) {
        return false;
    }
    // isSessionActive covers both live recording and stop-in-flight finalize.
    if (!m_windowVideoRecorder || !m_windowVideoRecorder->isSessionActive()) {
        return false;
    }
    if (m_waitingForRecorderToStop) {
        return true;
    }

    m_waitingForRecorderToStop = true;
    hide();

    // Connect BEFORE stopRecording(): the native fallback path emits
    // recordingStopped synchronously inside stop, and a post-stop connect would
    // miss it (macOS hang / zombie process).
    connect(m_windowVideoRecorder, &WindowVideoRecorder::recordingStopped, this, [this]() {
        // Defer so we never nest closeEvent inside stopRecording.
        QTimer::singleShot(0, this, [this]() { close(); });
    }, Qt::SingleShotConnection);

    // Watchdog: recordingStopped is not guaranteed to arrive (wedged encoder,
    // latched backend error). Without it the hidden window can never close and
    // the process can only be killed — the very failure the hide/defer above is
    // meant to avoid, just one step later.
    QTimer::singleShot(kRecorderStopWatchdogMs, this, [this]() {
        if (m_recorderCloseForced || !m_waitingForRecorderToStop) {
            return;
        }
        if (!m_windowVideoRecorder || !m_windowVideoRecorder->isSessionActive()) {
            return;
        }
        qWarning("MainWindow: window recorder did not finalize within %d ms; closing anyway.",
                 kRecorderStopWatchdogMs);
        m_recorderCloseForced = true;
        close();
    });

    m_windowVideoRecorder->stopRecording();
    return true;
}

} // namespace AetherSDR
