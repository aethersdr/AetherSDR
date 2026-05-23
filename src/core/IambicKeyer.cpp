#include "IambicKeyer.h"

#include <algorithm>

namespace MasterSDR {

namespace {

// Element timing follows the PARIS standard:
//   1 dit = 1200 / WPM milliseconds
//   1 dah = 3 dits
//   inter-element gap = 1 dit
//
// We don't apply weighting or ratio knobs in this MVP; the radio's CW
// engine handles those for the on-air signal, and the sidetone matches
// the basic 3:1 ratio that most operators expect at the dit level.
constexpr int kMinWpm = 5;
constexpr int kMaxWpm = 60;
constexpr int kDitsPerDah = 3;

} // namespace

IambicKeyer::IambicKeyer() = default;

IambicKeyer::~IambicKeyer()
{
    stop();
}

void IambicKeyer::setOnKeyDownChange(KeyDownCallback cb)
{
    m_onKeyDownChange = std::move(cb);
}

void IambicKeyer::setOnPaddleEvent(PaddleEventCallback cb)
{
    m_onPaddleEvent = std::move(cb);
}

void IambicKeyer::start()
{
    if (m_running.exchange(true, std::memory_order_acq_rel))
        return;
    m_stopRequested.store(false, std::memory_order_release);
    m_thread = std::thread(&IambicKeyer::workerLoop, this);
}

void IambicKeyer::stop()
{
    if (!m_running.exchange(false, std::memory_order_acq_rel))
        return;
    m_stopRequested.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(m_mu);
        m_paddleStateDirty = true;
    }
    m_cv.notify_all();
    if (m_thread.joinable())
        m_thread.join();
    if (m_lastEmittedKeyDown) emitKeyDown(false);
    if (m_lastEmittedDit || m_lastEmittedDah) emitPaddleEvent(false, false);
}

void IambicKeyer::setMode(Mode m) noexcept
{
    m_mode.store(static_cast<int>(m), std::memory_order_relaxed);
}

void IambicKeyer::setWpm(int wpm) noexcept
{
    m_wpm.store(std::clamp(wpm, kMinWpm, kMaxWpm), std::memory_order_relaxed);
}

void IambicKeyer::setSwapPaddles(bool swap) noexcept
{
    m_swap.store(swap, std::memory_order_relaxed);
}

void IambicKeyer::setPaddleState(bool dit, bool dah) noexcept
{
    const bool swap = m_swap.load(std::memory_order_relaxed);
    const bool d = swap ? dah : dit;
    const bool h = swap ? dit : dah;

    {
        std::lock_guard<std::mutex> lk(m_mu);
        if (m_ditPressed == d && m_dahPressed == h) return;
        m_ditPressed = d;
        m_dahPressed = h;
        m_paddleStateDirty = true;
    }
    m_cv.notify_all();
}

void IambicKeyer::reset() noexcept
{
    std::lock_guard<std::mutex> lk(m_mu);
    m_ditMemory = false;
    m_dahMemory = false;
    m_paddleStateDirty = true;
    m_cv.notify_all();
}

int IambicKeyer::unitMs() const noexcept
{
    const int wpm = std::clamp(m_wpm.load(std::memory_order_relaxed), kMinWpm, kMaxWpm);
    return 1200 / wpm;
}

IambicKeyer::Element IambicKeyer::nextElementChoice(bool ditWanted,
                                                    bool dahWanted,
                                                    Element justSent) const noexcept
{
    if (ditWanted && dahWanted)
        return justSent == Element::Dit ? Element::Dah : Element::Dit;
    if (ditWanted) return Element::Dit;
    if (dahWanted) return Element::Dah;
    return justSent == Element::Dit ? Element::Dah : Element::Dit;
}

void IambicKeyer::emitKeyDown(bool down)
{
    if (down == m_lastEmittedKeyDown) return;
    m_lastEmittedKeyDown = down;
    if (m_onKeyDownChange) m_onKeyDownChange(down);
}

void IambicKeyer::emitPaddleEvent(bool dit, bool dah)
{
    if (dit == m_lastEmittedDit && dah == m_lastEmittedDah) return;
    m_lastEmittedDit = dit;
    m_lastEmittedDah = dah;
    if (m_onPaddleEvent) m_onPaddleEvent(dit, dah);
}

void IambicKeyer::workerLoop()
{
    Element lastSent = Element::Dah;   // first paddle press emits whatever's wanted
    bool firstInSqueeze = true;        // resets each time we re-enter the active phase

    while (!m_stopRequested.load(std::memory_order_acquire)) {
        // ── Idle wait — block until paddle pressed ─────────────────────
        bool dit, dah;
        {
            std::unique_lock<std::mutex> lk(m_mu);
            m_cv.wait(lk, [this]() {
                return m_paddleStateDirty
                    || m_stopRequested.load(std::memory_order_acquire);
            });
            m_paddleStateDirty = false;
            dit = m_ditPressed;
            dah = m_dahPressed;
        }
        // Always forward latest paddle state to the radio (even on
        // release events — the radio's iambic engine needs to see them).
        emitPaddleEvent(dit, dah);

        if (m_stopRequested.load(std::memory_order_acquire)) break;
        if (!dit && !dah) {
            firstInSqueeze = true;
            continue;
        }

        // ── Active phase: produce elements while paddle pressed ────────
        bool wantDit = dit, wantDah = dah;
        firstInSqueeze = true;

        while (!m_stopRequested.load(std::memory_order_acquire)
               && (wantDit || wantDah || m_ditMemory || m_dahMemory)) {

            // Pick next element.
            Element next;
            if (firstInSqueeze) {
                next = wantDit ? Element::Dit : Element::Dah;
                firstInSqueeze = false;
            } else if (m_ditMemory || m_dahMemory) {
                // Memory bits force the opposite element.
                next = m_ditMemory ? Element::Dit : Element::Dah;
                {
                    std::lock_guard<std::mutex> lk(m_mu);
                    m_ditMemory = false;
                    m_dahMemory = false;
                }
            } else {
                next = nextElementChoice(wantDit, wantDah, lastSent);
            }
            lastSent = next;

            const int unit = unitMs();
            const int onDuration = (next == Element::Dit) ? unit : unit * kDitsPerDah;
            const int offDuration = unit;
            const Mode currentMode =
                static_cast<Mode>(m_mode.load(std::memory_order_relaxed));

            // ── Element on ─────────────────────────────────────────────
            emitKeyDown(true);
            const auto onDeadline =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(onDuration);
            {
                std::unique_lock<std::mutex> lk(m_mu);
                while (std::chrono::steady_clock::now() < onDeadline
                       && !m_stopRequested.load(std::memory_order_acquire)) {
                    m_cv.wait_until(lk, onDeadline);
                    // Mode B: latch memory when opposite paddle is held
                    // mid-element.
                    if (currentMode == Mode::IambicB) {
                        if (next == Element::Dit && m_dahPressed) m_dahMemory = true;
                        if (next == Element::Dah && m_ditPressed) m_ditMemory = true;
                    }
                }
            }
            emitKeyDown(false);
            if (m_stopRequested.load(std::memory_order_acquire)) break;

            // ── Inter-element gap ──────────────────────────────────────
            const auto offDeadline =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(offDuration);
            {
                std::unique_lock<std::mutex> lk(m_mu);
                while (std::chrono::steady_clock::now() < offDeadline
                       && !m_stopRequested.load(std::memory_order_acquire)) {
                    m_cv.wait_until(lk, offDeadline);
                    if (currentMode == Mode::IambicB) {
                        if (next == Element::Dit && m_dahPressed) m_dahMemory = true;
                        if (next == Element::Dah && m_ditPressed) m_ditMemory = true;
                    }
                }
            }

            // Re-read paddle state for the next iteration's decision.
            // Always forward to the radio so it sees release events.
            {
                std::lock_guard<std::mutex> lk(m_mu);
                wantDit = m_ditPressed;
                wantDah = m_dahPressed;
            }
            emitPaddleEvent(wantDit, wantDah);
        }

        // Active phase ended; loop back to idle wait.
    }

    emitKeyDown(false);
    emitPaddleEvent(false, false);
}

} // namespace MasterSDR
