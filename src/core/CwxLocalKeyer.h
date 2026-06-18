#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

#include <QQueue>
#include <QString>
#include <QtGlobal>

namespace AetherSDR {

// Local Morse keyer — generates dit/dah timing events from CWX text so the
// AetherSDR sidetone path can play a tone matching what the radio is
// transmitting.  Independent of the radio's own keyer; both use the same
// configured WPM, so they stay in sync within ±1 element on typical hardware.
// If they drift, sidetone is informational only — the radio produces the
// actual on-air CW.
//
// Threading
// ─────────
// The element schedule runs on a dedicated worker thread.  Each edge is timed
// to an absolute std::chrono::steady_clock deadline (drift-corrected against
// the run epoch) waited on an interruptible condition_variable — NOT a QTimer.
// QTimer fires on whichever event loop owns it, and under panadapter paint +
// VITA-49 burst handling that loop coalesces the firing, landing the edge a
// block or two late and clipping individual CW elements (#3623).  IambicKeyer
// abandoned QTimer for exactly this reason ("QTimer's jitter is too high for
// CW"); this keyer now mirrors that pattern.
//
// Output
// ──────
// onKeyDownChange(bool down) flips the sidetone gate.  It is called directly
// from the worker thread, so the receiver MUST be lock-free (e.g.
// CwSidetoneGenerator::setKeyDown, which is std::atomic).
class CwxLocalKeyer {
public:
    using KeyDownCallback = std::function<void(bool down)>;

    CwxLocalKeyer();
    ~CwxLocalKeyer();

    CwxLocalKeyer(const CwxLocalKeyer&) = delete;
    CwxLocalKeyer& operator=(const CwxLocalKeyer&) = delete;

    // Install before start().  Called on the worker thread — the receiver must
    // be lock-free.
    void setOnKeyDownChange(KeyDownCallback cb);

    // Append a transmission segment.  Spawns the worker thread on first use; if
    // a segment is already playing, the new text is queued and plays when the
    // current one finishes.
    void start(const QString& text, int wpm);

    // Cancel any pending text and key-up immediately.  Used by
    // CwxModel::clearBuffer / erase so the operator's "stop sending" maps to
    // instant local silence.  The worker stays alive for the next send.
    void stop();

    // Not thread-safe: reads worker-owned schedule state without a lock, so it
    // is for the synchronous drift test / diagnostics only and must not be
    // called from another thread while the worker is running (it would race
    // scheduleNext()). Route through m_mu if ever promoted to a live
    // cross-thread query. (#3623 review)
    bool isIdle() const { return m_elements.isEmpty() && !m_running; }

protected:
    // Test seam: a subclass constructed with spawnWorker=false drives the
    // schedule synchronously (no real thread) by calling onTick() itself and
    // injecting elapsedMs(), so the #3271 drift-correction coverage stays
    // deterministic.
    explicit CwxLocalKeyer(bool spawnWorker);
    void onTick();
    virtual qint64 elapsedMs() const;       // ms since the run epoch
    virtual void armTimer(int waitMs);       // record the next (drift-corrected) wait
    qint64 nextEdgeMsForTest() const { return m_nextEdgeMs; }
    bool elapsedValidForTest() const { return m_epochValid; }

private:
    enum class Element : char { Dit, Dah, ElementGap, CharGap, WordGap };

    struct Pending { QString text; int wpm; };

    void encode(const QString& text, int wpm);
    void scheduleNext();
    void resetEpoch();
    void startEpoch();
    void emitKeyDown(bool down);
    void keyUpIfDown();
    void workerLoop();

    // Schedule state — touched only by the thread that runs scheduleNext()
    // (the worker in production, or the test thread when spawnWorker=false).
    QQueue<Element> m_elements;
    int           m_unitMs{60};   // 60 ms = 20 WPM
    bool          m_running{false};
    bool          m_currentlyDown{false};
    // Drift-corrected scheduling: an absolute target edge time vs an elapsed
    // reference, so a late wake is followed by a correspondingly shorter wait.
    // Without this, scheduler jitter would push each successive edge later and
    // accumulate audible stutter / wrong word spacing (#2980/#3202).
    std::chrono::steady_clock::time_point m_epoch{};
    bool          m_epochValid{false};
    qint64        m_nextEdgeMs{0};
    int           m_nextWaitMs{0};   // last wait armTimer() recorded

    KeyDownCallback m_onKeyDownChange;
    bool            m_lastEmittedKeyDown{false};

    // Cross-thread control.  m_queue is guarded by m_mu; the flags are atomic.
    const bool              m_spawnWorker{true};
    std::thread             m_thread;
    mutable std::mutex      m_mu;
    std::condition_variable m_cv;
    QQueue<Pending>         m_queue;
    std::atomic<bool>       m_workerStarted{false};
    std::atomic<bool>       m_abort{false};
    std::atomic<bool>       m_shutdown{false};
};

} // namespace AetherSDR
