#include "CwxLocalKeyer.h"

#include <QHash>

namespace AetherSDR {

namespace {

// Standard ITU Morse table.  Lowercase keys; we uppercase incoming text
// before lookup.  Punctuation mirrors what FlexRadio's CWX accepts on
// the wire; if we encounter an unknown character we just emit a word
// gap so the message stays time-aligned with the radio.
const QHash<QChar, QString>& morseTable()
{
    static const QHash<QChar, QString> t = {
        {'A', ".-"},    {'B', "-..."},  {'C', "-.-."},  {'D', "-.."},
        {'E', "."},     {'F', "..-."},  {'G', "--."},   {'H', "...."},
        {'I', ".."},    {'J', ".---"},  {'K', "-.-"},   {'L', ".-.."},
        {'M', "--"},    {'N', "-."},    {'O', "---"},   {'P', ".--."},
        {'Q', "--.-"},  {'R', ".-."},   {'S', "..."},   {'T', "-"},
        {'U', "..-"},   {'V', "...-"},  {'W', ".--"},   {'X', "-..-"},
        {'Y', "-.--"},  {'Z', "--.."},
        {'0', "-----"}, {'1', ".----"}, {'2', "..---"}, {'3', "...--"},
        {'4', "....-"}, {'5', "....."}, {'6', "-...."}, {'7', "--..."},
        {'8', "---.."}, {'9', "----."},
        {'.', ".-.-.-"},{',', "--..--"},{'?', "..--.."},{'\'',".----."},
        {'!', "-.-.--"},{'/', "-..-."}, {'(', "-.--."}, {')', "-.--.-"},
        {'&', ".-..."}, {':', "---..."},{';', "-.-.-."},{'=', "-...-"},
        {'+', ".-.-."}, {'-', "-....-"},{'_', "..--.-"},{'"', ".-..-."},
        {'$', "...-..-"},{'@', ".--.-."},
    };
    return t;
}

} // namespace

CwxLocalKeyer::CwxLocalKeyer() : CwxLocalKeyer(true) {}

CwxLocalKeyer::CwxLocalKeyer(bool spawnWorker) : m_spawnWorker(spawnWorker) {}

CwxLocalKeyer::~CwxLocalKeyer()
{
    m_shutdown.store(true, std::memory_order_release);
    m_abort.store(true, std::memory_order_release);
    m_cv.notify_all();
    if (m_thread.joinable())
        m_thread.join();
    keyUpIfDown();
}

void CwxLocalKeyer::setOnKeyDownChange(KeyDownCallback cb)
{
    m_onKeyDownChange = std::move(cb);
}

void CwxLocalKeyer::start(const QString& text, int wpm)
{
    if (text.isEmpty() || wpm <= 0) return;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        m_queue.enqueue({text, wpm});
    }
    if (m_spawnWorker) {
        // Spawn the worker lazily on first use, then keep it alive for the
        // session (it parks on the condition_variable between sends).
        if (!m_workerStarted.exchange(true, std::memory_order_acq_rel))
            m_thread = std::thread(&CwxLocalKeyer::workerLoop, this);
        m_cv.notify_all();
    } else if (!m_running) {
        // Synchronous test path: emit the first edge now; the test ticks the
        // rest via onTick().
        scheduleNext();
    }
}

void CwxLocalKeyer::stop()
{
    {
        std::lock_guard<std::mutex> lk(m_mu);
        m_queue.clear();
    }
    m_abort.store(true, std::memory_order_release);
    if (m_spawnWorker) {
        m_cv.notify_all();
    } else {
        // Synchronous test path: abort immediately.
        keyUpIfDown();
        m_elements.clear();
        m_running = false;
        resetEpoch();
        m_abort.store(false, std::memory_order_release);
    }
}

void CwxLocalKeyer::encode(const QString& text, int wpm)
{
    // Standard CW timing: 1 unit = 1.2 / WPM seconds.  At 20 WPM →
    // 60 ms/unit; matches the FlexRadio keyer's output within ±1 ms.
    m_unitMs = qMax(1, static_cast<int>(1200.0 / wpm));

    const auto& tbl = morseTable();
    bool firstChar = true;
    for (QChar ch : text.toUpper()) {
        if (ch == ' ') {
            // Word gap = 7 units total = WordGap (4u, added here) +
            // CharGap (3u, added by the next letter via the !firstChar
            // branch below).  Critically, do NOT reset firstChar to true:
            // doing so would suppress that next CharGap and leave only
            // 4u of silence, which falls inside ggmorse's 3u
            // inter-character classification window and erases word
            // boundaries on the TX decode panel (#2417).  4u + 3u = 7u
            // also matches standard CW timing for the local sidetone
            // listener.
            m_elements.enqueue(Element::WordGap);
            continue;
        }
        const auto it = tbl.find(ch);
        if (it == tbl.end()) continue;
        if (!firstChar)
            m_elements.enqueue(Element::CharGap);
        firstChar = false;
        const QString& pattern = it.value();
        for (int i = 0; i < pattern.size(); ++i) {
            if (i > 0)
                m_elements.enqueue(Element::ElementGap);
            m_elements.enqueue(pattern[i] == '.' ? Element::Dit : Element::Dah);
        }
    }
}

void CwxLocalKeyer::scheduleNext()
{
    if (m_elements.isEmpty()) {
        Pending p;
        bool havePending = false;
        {
            std::lock_guard<std::mutex> lk(m_mu);
            if (!m_queue.isEmpty()) {
                p = m_queue.dequeue();
                havePending = true;
            }
        }
        if (!havePending) {
            m_running = false;
            resetEpoch();
            keyUpIfDown();
            return;
        }
        // In live mode each keystroke arrives as a separate Pending entry.
        // encode() always starts with firstChar=true so it never prepends a
        // CharGap for the transition between entries. The last element of any
        // encoded character is always Dit or Dah, leaving m_currentlyDown=true
        // here; without an explicit gap the key stays down continuously and
        // adjacent characters merge into undecodable noise (#2473).
        if (m_currentlyDown)
            m_elements.enqueue(Element::CharGap);
        encode(p.text, p.wpm);
        if (m_elements.isEmpty()) {
            scheduleNext();
            return;
        }
    }

    m_running = true;
    const Element e = m_elements.dequeue();
    int durationMs = m_unitMs;
    bool keyDownNext = false;
    switch (e) {
    case Element::Dit:        durationMs = m_unitMs;     keyDownNext = true;  break;
    case Element::Dah:        durationMs = m_unitMs * 3; keyDownNext = true;  break;
    case Element::ElementGap: durationMs = m_unitMs;     keyDownNext = false; break;
    case Element::CharGap:    durationMs = m_unitMs * 3; keyDownNext = false; break;
    case Element::WordGap:    durationMs = m_unitMs * 4; keyDownNext = false; break;
    }
    if (keyDownNext != m_currentlyDown) {
        m_currentlyDown = keyDownNext;
        emitKeyDown(keyDownNext);
    }
    // Drift-correct against an absolute clock: the next edge should land at
    // m_nextEdgeMs from the start of the run, not durationMs from now, so a
    // late wake is followed by a correspondingly shorter wait and the slip
    // does not accumulate (#2980/#3202).
    if (!m_epochValid)
        startEpoch();
    m_nextEdgeMs += durationMs;
    const qint64 wait = qMax<qint64>(1, m_nextEdgeMs - elapsedMs());
    armTimer(static_cast<int>(wait));
}

void CwxLocalKeyer::onTick()
{
    scheduleNext();
}

qint64 CwxLocalKeyer::elapsedMs() const
{
    if (!m_epochValid) return 0;
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - m_epoch)
        .count();
}

void CwxLocalKeyer::armTimer(int waitMs)
{
    // Production: the worker thread sleeps to the absolute steady_clock
    // deadline (m_epoch + m_nextEdgeMs); this records the equivalent relative
    // wait for diagnostics and for the drift-correction unit test, which
    // overrides this seam.
    m_nextWaitMs = waitMs;
}

void CwxLocalKeyer::resetEpoch()
{
    m_epochValid = false;
    m_nextEdgeMs = 0;
}

void CwxLocalKeyer::startEpoch()
{
    m_epoch = std::chrono::steady_clock::now();
    m_epochValid = true;
    m_nextEdgeMs = 0;
}

void CwxLocalKeyer::emitKeyDown(bool down)
{
    if (down == m_lastEmittedKeyDown) return;
    m_lastEmittedKeyDown = down;
    if (m_onKeyDownChange) m_onKeyDownChange(down);
}

void CwxLocalKeyer::keyUpIfDown()
{
    m_currentlyDown = false;
    emitKeyDown(false);
}

void CwxLocalKeyer::workerLoop()
{
    for (;;) {
        // ── Idle wait — park until there is text to send (or shutdown) ──
        {
            std::unique_lock<std::mutex> lk(m_mu);
            m_cv.wait(lk, [this] {
                return m_shutdown.load(std::memory_order_acquire)
                    || !m_queue.isEmpty();
            });
            if (m_shutdown.load(std::memory_order_acquire))
                break;
        }
        // Fresh send run — clear any stale abort latched while idle.
        m_abort.store(false, std::memory_order_release);

        for (;;) {
            scheduleNext();          // advance one edge; pulls m_queue, emits gate
            if (!m_running)
                break;               // fully drained → back to idle wait

            // Interruptible sleep to the element's absolute steady_clock
            // deadline.  No QTimer / event loop, so panadapter paint and
            // VITA-49 bursts can't coalesce the edge (#3623).
            const auto deadline = m_epoch + std::chrono::milliseconds(m_nextEdgeMs);
            {
                std::unique_lock<std::mutex> lk(m_mu);
                m_cv.wait_until(lk, deadline, [this] {
                    return m_abort.load(std::memory_order_acquire)
                        || m_shutdown.load(std::memory_order_acquire);
                });
            }
            if (m_abort.load(std::memory_order_acquire)
                || m_shutdown.load(std::memory_order_acquire)) {
                keyUpIfDown();
                m_elements.clear();
                resetEpoch();
                m_running = false;
                break;
            }
        }
        if (m_shutdown.load(std::memory_order_acquire))
            break;
    }
    keyUpIfDown();
}

} // namespace AetherSDR
