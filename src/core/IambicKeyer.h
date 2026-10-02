#pragma once

#include "TxCoordinator.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace AetherSDR {

// Software iambic keyer (Modes A and B) for a paddle wired to the PC. Paddle
// timing lives here so every backend sees the same completed element edges: Flex
// forwards them to NetCW, a host-modulating backend (HL2) shapes IQ, and the
// local sidetone gate follows with sub-5 ms latency.
// Threading: a dedicated worker waits on a condition_variable until absolute
// steady_clock deadlines (QTimer jitter is too high for CW); setPaddleState()
// may be called from any thread and wakes it.
// Callbacks, set before start():
//   - onKeyDownChange(down, when): `when` is the edge's SCHEDULED grid instant,
//     not wake time (0-5 ms late on macOS, #4890), so consumers placing edges in
//     time keep the rhythm. Called on the worker: the receiver MUST be lock-free
//     (e.g. CwSidetoneGenerator::setKeyDown).
//   - onPaddleEvent(dit, dah): raw paddle transitions, diagnostics only.
//   - onRoutedKeyDownChange(down, when, request): an element derived from the
//     raw input for the engine queue; never supplies a new producer/session.
class IambicKeyer {
public:
    enum class Mode : int {
        IambicA = 0,   // squeeze: alternates dit/dah, stops at element boundary on release
        IambicB = 1,   // squeeze: like A, plus one extra element if released during second-to-last element
    };

    using KeyDownCallback    = std::function<void(bool down,
                                   std::chrono::steady_clock::time_point when)>;
    using PaddleEventCallback = std::function<void(bool dit, bool dah)>;
    using RoutedKeyDownCallback = std::function<void(bool, std::chrono::steady_clock::time_point,
                                                     const TxCoordinator::Request&)>;

    IambicKeyer();
    ~IambicKeyer();

    IambicKeyer(const IambicKeyer&) = delete;
    IambicKeyer& operator=(const IambicKeyer&) = delete;

    // Install output callbacks before start().  Both are called on the
    // worker thread; receivers must hop to their own thread if needed.
    void setOnKeyDownChange(KeyDownCallback cb);
    void setOnPaddleEvent(PaddleEventCallback cb);
    void setOnRoutedKeyDownChange(RoutedKeyDownCallback cb);

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
    void setPaddleState(bool dit, bool dah, const TxCoordinator::Request& input) noexcept;

    // Hard reset: stop the current element, key up, clear memory bits.
    // Used when WPM/mode changes invalidate the current element timing.
    void reset() noexcept;

private:
    enum class Element : int { Dit = 1, Dah = 2 };

    void workerLoop();
    void setPaddleInput(bool dit, bool dah, const TxCoordinator::Request* input) noexcept;
    std::chrono::nanoseconds unitNs() const noexcept;  // 1.2e9 ns / WPM, clamped
    Element nextElementChoice(bool ditWanted, bool dahWanted, Element justSent) const noexcept;
    void emitKeyDown(bool down, std::chrono::steady_clock::time_point when);
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
    TxCoordinator::Request  m_input; // original raw input, under m_mu
    TxCoordinator::Request  m_elementInput; // worker-owned through matching key-up

    // Iambic mode B "memory" bits — set when the opposite paddle is
    // pressed mid-element, cleared when consumed.  Atomic because the
    // worker's between-element checks read them without holding m_mu
    // while the latch sites store under it (#4809 review find).
    std::atomic<bool>       m_ditMemory{false};
    std::atomic<bool>       m_dahMemory{false};

    std::atomic<int>        m_mode{static_cast<int>(Mode::IambicB)};
    std::atomic<int>        m_wpm{20};
    std::atomic<bool>       m_swap{false};

    KeyDownCallback         m_onKeyDownChange;
    PaddleEventCallback     m_onPaddleEvent;
    RoutedKeyDownCallback   m_onRoutedKeyDownChange;
    bool                    m_lastEmittedKeyDown{false};
    bool                    m_lastEmittedDit{false};
    bool                    m_lastEmittedDah{false};
};

} // namespace AetherSDR
