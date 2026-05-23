#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace MasterSDR {

// Software iambic keyer state machine — drives the local CW sidetone in
// real time when an operator's paddle is wired to the PC instead of the
// radio.  Modes A and B implemented for v1; Ultimatic / Bug / Straight
// follow in a later phase.
//
// Architecture
// ────────────
// The radio has its own iambic engine on the RF side; we feed it raw
// paddle states via RadioModel::sendCwPaddle(dit, dah) and let the radio
// produce the on-air signal.  This class runs an *identical* iambic
// state machine locally for the sole purpose of driving the sidetone
// gate with sub-5 ms latency.  Both engines see the same paddle inputs;
// configured at the same WPM they produce identical Morse timing.
//
// Threading
// ─────────
// The state machine runs on a dedicated worker thread.  Element timing
// uses std::this_thread::sleep_until against std::chrono::steady_clock —
// QTimer's jitter is too high for CW.  Paddle edges are pushed in via
// setPaddleState() from any thread; the worker wakes via a
// condition_variable.
//
// Output
// ──────
// Two callbacks set at construction:
//   - onKeyDownChange(bool down) — flips the sidetone gate.  Called
//     directly from the worker thread; the receiver MUST be lock-free
//     (e.g. CwSidetoneGenerator::setKeyDown which is std::atomic).
//   - onPaddleEvent(bool dit, bool dah) — passes raw paddle states to
//     the caller for forwarding to the radio.  The caller is responsible
//     for hopping to the radio thread (Qt::QueuedConnection or
//     QMetaObject::invokeMethod).
class IambicKeyer {
public:
    enum class Mode : int {
        IambicA = 0,   // squeeze: alternates dit/dah, stops at element boundary on release
        IambicB = 1,   // squeeze: like A, plus one extra element if released during second-to-last element
    };

    using KeyDownCallback    = std::function<void(bool down)>;
    using PaddleEventCallback = std::function<void(bool dit, bool dah)>;

    IambicKeyer();
    ~IambicKeyer();

    IambicKeyer(const IambicKeyer&) = delete;
    IambicKeyer& operator=(const IambicKeyer&) = delete;

    // Install output callbacks before start().  Both are called on the
    // worker thread; receivers must hop to their own thread if needed.
    void setOnKeyDownChange(KeyDownCallback cb);
    void setOnPaddleEvent(PaddleEventCallback cb);

    // Spawn the worker thread.  Idempotent.
    void start();

    // Stop the worker, drain pending elements, ensure key is up.
    // Idempotent and safe to call from the destructor.
    void stop();

    bool isRunning() const noexcept { return m_running.load(std::memory_order_acquire); }

    // Parameter setters — atomic, callable from any thread.
    void setMode(Mode m) noexcept;
    void setWpm(int wpm) noexcept;                  // clamped to [5, 60]
    void setSwapPaddles(bool swap) noexcept;        // swap dit/dah inputs

    Mode mode() const noexcept { return static_cast<Mode>(m_mode.load(std::memory_order_relaxed)); }
    int  wpm() const noexcept  { return m_wpm.load(std::memory_order_relaxed); }
    bool swapPaddles() const noexcept { return m_swap.load(std::memory_order_relaxed); }

    // Paddle edge input.  Call whenever raw paddle state changes —
    // typically from PaddleReader on its own thread.  Both arguments
    // are absolute states (true = pressed), not edges.
    void setPaddleState(bool dit, bool dah) noexcept;

    // Hard reset: stop the current element, key up, clear memory bits.
    // Used when WPM/mode changes invalidate the current element timing.
    void reset() noexcept;

private:
    enum class Element : int { Dit = 1, Dah = 2 };

    void workerLoop();
    int  unitMs() const noexcept;          // 1200 / WPM, clamped
    Element nextElementChoice(bool ditWanted, bool dahWanted, Element justSent) const noexcept;
    void emitKeyDown(bool down);
    void emitPaddleEvent(bool dit, bool dah);

    std::thread             m_thread;
    std::atomic<bool>       m_running{false};
    std::atomic<bool>       m_stopRequested{false};

    std::mutex              m_mu;
    std::condition_variable m_cv;

    // Latched paddle state — written from any thread under m_mu, read
    // by the worker.  An std::atomic pair would be marginally faster
    // but the worker only checks these at element boundaries (every
    // 50 ms at 24 WPM), so locking cost is irrelevant.
    bool                    m_ditPressed{false};
    bool                    m_dahPressed{false};
    bool                    m_paddleStateDirty{false};

    // Iambic mode B "memory" bits — set when the opposite paddle is
    // pressed mid-element, cleared when consumed.
    bool                    m_ditMemory{false};
    bool                    m_dahMemory{false};

    std::atomic<int>        m_mode{static_cast<int>(Mode::IambicB)};
    std::atomic<int>        m_wpm{20};
    std::atomic<bool>       m_swap{false};

    KeyDownCallback         m_onKeyDownChange;
    PaddleEventCallback     m_onPaddleEvent;
    bool                    m_lastEmittedKeyDown{false};
    bool                    m_lastEmittedDit{false};
    bool                    m_lastEmittedDah{false};
};

} // namespace MasterSDR
