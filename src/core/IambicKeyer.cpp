#include "IambicKeyer.h"

#include <algorithm>

namespace AetherSDR {

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

std::chrono::nanoseconds IambicKeyer::unitNs() const noexcept
{
    const int wpm = std::clamp(m_wpm.load(std::memory_order_relaxed), kMinWpm, kMaxWpm);
    // Nanosecond unit math: integer 1200/wpm ms truncates (52 ms vs
    // 52.17 ms at 23 WPM — a permanent 0.3% speed error); dividing in
    // ns leaves sub-ppm error at any legal WPM (#4809).
    return std::chrono::nanoseconds(1'200'000'000LL / wpm);
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

        // Mode B memory latch — one definition for the three sites that
        // must agree (element-start snapshot + the two in-wait live
        // checks).  Caller holds m_mu.
        const auto latchOppositeLocked = [this](Element cur) {
            if (cur == Element::Dit && m_dahPressed) m_dahMemory = true;
            if (cur == Element::Dah && m_ditPressed) m_ditMemory = true;
        };

        // Element edges advance on an absolute grid anchored here, when
        // the active phase begins — the CwxLocalKeyer pattern from #3644.
        // Each deadline is grid + duration and the grid advances by the
        // nominal duration, so thread wake latency and callback cost eat
        // into the following wait instead of stretching every element.
        // The old per-element `now() + duration` anchoring made element
        // length `nominal + wake latency + callback cost` with a fresh,
        // strictly positive error term each element (#4809: measured
        // +4 ms mean on the reporter's machine, one-sided).
        auto grid = std::chrono::steady_clock::now();

        while (!m_stopRequested.load(std::memory_order_acquire)
               && (wantDit || wantDah
                   || m_ditMemory.load() || m_dahMemory.load())) {

            // Pick next element.
            Element next;
            if (firstInSqueeze) {
                next = wantDit ? Element::Dit : Element::Dah;
                firstInSqueeze = false;
            } else if (m_ditMemory.load() || m_dahMemory.load()) {
                // Memory bits force the opposite element.
                next = m_ditMemory.load() ? Element::Dit : Element::Dah;
                m_ditMemory.store(false);
                m_dahMemory.store(false);
            } else {
                next = nextElementChoice(wantDit, wantDah, lastSent);
            }
            lastSent = next;

            const std::chrono::nanoseconds unit = unitNs();
            const std::chrono::nanoseconds onDuration =
                (next == Element::Dit) ? unit : unit * kDitsPerDah;
            const std::chrono::nanoseconds offDuration = unit;
            const Mode currentMode =
                static_cast<Mode>(m_mode.load(std::memory_order_relaxed));

            // Mode B: latch the opposite paddle's state at the moment the
            // element begins.  The live checks in the on/gap wait loops below
            // only observe paddle state when the condition variable wakes, and
            // setPaddleState() stores the new values before notifying — so a
            // simultaneous dual release (routine when the serial poll collapses
            // both edges into one ~10 ms tick, #4032) wakes the loop with the
            // held state already gone and the memory never latches, silently
            // degrading Mode B to Mode A.  Snapshotting up front closes that
            // race; the live checks stay, they still catch a genuine
            // mid-element opposite-paddle tap that a start snapshot would miss.
            if (currentMode == Mode::IambicB) {
                std::lock_guard<std::mutex> lk(m_mu);
                latchOppositeLocked(next);
            }

            // ── Element on ─────────────────────────────────────────────
            // Deadline armed BEFORE the key-down callback, so the
            // callback's cost (sidetone gate flip, per-edge trace log,
            // queued radio post) cannot push the edge out.
            // Catch-up limiter: a stall longer than one element leaves every
            // following deadline already past, so both wait loops fall
            // straight through and the worker emits zero-length elements —
            // and zero-length `cw key` edges on air — until the grid catches
            // up.  Re-anchor instead.  Ordinary wake latency is orders of
            // magnitude inside one element, so the self-correcting property
            // this whole change exists for is untouched; only a stall that
            // already lost an element stops trying to win the time back.
            const auto markNow = std::chrono::steady_clock::now();
            if (grid + onDuration < markNow) grid = markNow;
            const auto onDeadline = grid + onDuration;
            emitKeyDown(true);
            {
                std::unique_lock<std::mutex> lk(m_mu);
                while (std::chrono::steady_clock::now() < onDeadline
                       && !m_stopRequested.load(std::memory_order_acquire)) {
                    m_cv.wait_until(lk, onDeadline);
                    // Mode B: latch memory when opposite paddle is held
                    // mid-element.
                    if (currentMode == Mode::IambicB)
                        latchOppositeLocked(next);
                }
            }
            grid = onDeadline;

            // ── Inter-element gap ──────────────────────────────────────
            // Same limiter for the gap: without it the element that absorbed
            // the stall is followed by a zero-length space.
            const auto gapNow = std::chrono::steady_clock::now();
            if (grid + offDuration < gapNow) grid = gapNow;
            const auto offDeadline = grid + offDuration;
            emitKeyDown(false);
            if (m_stopRequested.load(std::memory_order_acquire)) break;
            {
                std::unique_lock<std::mutex> lk(m_mu);
                while (std::chrono::steady_clock::now() < offDeadline
                       && !m_stopRequested.load(std::memory_order_acquire)) {
                    m_cv.wait_until(lk, offDeadline);
                    if (currentMode == Mode::IambicB)
                        latchOppositeLocked(next);
                }
            }
            grid = offDeadline;

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

} // namespace AetherSDR
