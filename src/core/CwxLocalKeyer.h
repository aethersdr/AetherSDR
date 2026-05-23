#pragma once

#include <QObject>
#include <QQueue>
#include <QString>
#include <QTimer>

namespace MasterSDR {

// Local Morse keyer — generates dit/dah timing events from CWX text so the
// MasterSDR sidetone path can play a tone matching what the radio is
// transmitting.  Independent of the radio's own keyer; both use the same
// configured WPM, so they stay in sync within ±1 element on typical
// hardware.  If they drift, sidetone is informational only — the radio
// produces the actual on-air CW.
//
// Usage: enqueue text with start(); the keyer schedules QTimer firings
// for each element transition (dit-on / dit-off / dah-on / dah-off /
// inter-element / inter-character / word gaps) and emits keyStateChanged
// for each on→off or off→on transition.
class CwxLocalKeyer : public QObject {
    Q_OBJECT
public:
    explicit CwxLocalKeyer(QObject* parent = nullptr);

    // Append a transmission segment.  If the keyer is idle this kicks off
    // playback; if it's mid-transmission the new text is queued and plays
    // when the current segment finishes.
    void start(const QString& text, int wpm);

    // Cancel any pending text and key-up immediately.  Used by
    // CwxModel::clearBuffer / erase so the operator's "stop sending" maps
    // to instant silence locally.
    void stop();

    bool isIdle() const { return m_elements.isEmpty() && !m_running; }

signals:
    void keyStateChanged(bool down);

private:
    enum class Element : char { Dit, Dah, ElementGap, CharGap, WordGap };

    struct Pending { QString text; int wpm; };

    void encode(const QString& text, int wpm);
    void scheduleNext();
    void onTick();

    QTimer        m_timer;
    QQueue<Pending> m_queue;
    QQueue<Element> m_elements;
    int           m_unitMs{60};   // 60 ms = 20 WPM
    bool          m_running{false};
    bool          m_currentlyDown{false};
};

} // namespace MasterSDR
