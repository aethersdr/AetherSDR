#pragma once

#include <QObject>
#include <QString>
#include <QVector>

namespace AetherSDR {

class CwxModel : public QObject {
    Q_OBJECT
public:
    explicit CwxModel(QObject* parent = nullptr);

    // A contiguous run of text to be keyed at a single WPM.
    // expandSpeedModifiers() returns a sequence of these.
    struct SpeedSegment {
        QString text;  // plain text (modifier prefix stripped, space=' ')
        int     wpm;
        bool operator==(const SpeedSegment& o) const {
            return text == o.text && wpm == o.wpm;
        }
    };

    // State
    int   speed()     const { return m_speed; }
    int   delay()     const { return m_delay; }
    int   speedStep() const { return m_speedStep; }
    bool  qskOn()     const { return m_qsk; }
    bool  isLive()    const { return m_live; }
    int   sentIndex() const { return m_sentIndex; }
    // radio_index of the last char in the queued batch we are waiting to drain,
    // or -1 when not tracking. queueEmpty() fires once sentIndex() reaches this.
    // Exposed for the automation bridge's `get cwx` snapshot so the queue-drain
    // watch is observable (#3949).
    int   cwxEndIndex() const { return m_cwxEndIndex; }
    // Generation counter for the drain watch. Bumped by resetDrainWatch() (ESC/
    // clear/disconnect) so a late cwx-send reply for a discarded batch can be
    // recognised as stale and ignored rather than re-arming the watch. (#3949)
    int   drainEpoch()  const { return m_drainEpoch; }
    QString macro(int idx) const;  // 0-based (0=F1, 11=F12)

    // Actions
    void send(const QString& text);      // Send mode: full string
    void sendChar(const QString& ch);    // Live mode: single char
    void sendMacro(int idx);             // 1-based (1=F1, 12=F12)
    void saveMacro(int idx, const QString& text); // 0-based
    void erase(int numChars);
    void clearBuffer();
    // Abort any in-flight queue-drain watch and bump the epoch so replies for
    // the discarded batch are rejected. Called on ESC/clear and on disconnect
    // (via RadioModel::onDisconnected) so a stale m_cwxEndIndex can't wedge the
    // monotonic guard across a reconnect. (#3949)
    void resetDrainWatch();
    void setSpeed(int wpm);
    void setDelay(int ms);
    void setSpeedStep(int step);
    void setQsk(bool on);
    void setLive(bool on);

    // Status parsing (from radio)
    void applyStatus(const QMap<QString, QString>& kvs);
    // Invoked by RadioModel with the reply to the final cwx send command.
    // Body format is "<radio_index>,<block>" per FlexLib CWX.cs:54-83.
    //
    // radio_index is the INSERTION-START (first-char) queue position of the
    // batch, NOT the last char — confirmed on FLEX-6500 fw 4.2.20.41343: a
    // 23-char send into a queue at sent=48 replied radio_index=49 (=48+1), and
    // `cwx sent=` then climbed 48→71 as the radio keyed. So the batch-end index
    // to watch is radio_index + nChars - 1 (49+23-1 = 71, matching the observed
    // final sent=). nChars is the char count of this send (spaces included).
    // The epoch is the drainEpoch() snapshot taken when the command was emitted;
    // a reply whose epoch no longer matches belongs to a batch that was since
    // aborted (ESC/clear/disconnect) and is ignored. (#3949)
    void handleSendReply(int resultCode, const QString& body, int epoch, int nChars);

    // Parses text for leading +/- speed modifiers on words.
    // A +/- run at word-start (after space or string-start) immediately
    // followed by a non-space, non-modifier character counts as a modifier.
    // Standalone +/- not followed by word chars are prosigns/hyphens and
    // pass through unchanged.  Speed resets to baseWpm after each modified
    // word.  Returns a single segment at baseWpm when no modifiers found.
    static QVector<SpeedSegment> expandSpeedModifiers(
        const QString& text, int baseWpm, int step);

signals:
    void commandReady(const QString& cmd);
    // Emitted for the final cwx send of a macro/send block, and for every
    // live-mode char, so RadioModel can capture the radio_index from the reply
    // and know when that block is fully transmitted.  epoch is snapshotted at
    // emit time (stale-batch guard) and nChars is the char count of this send
    // (spaces included) — handleSendReply() watches radio_index + nChars - 1
    // because radio_index is the batch's first-char position, not its last.
    // See handleSendReply(). (#3949)
    void replyCommandReady(const QString& cmd, int epoch, int nChars);
    void speedChanged(int wpm);
    void speedStepChanged(int step);
    void delayChanged(int ms);
    void qskChanged(bool on);
    void charSent(int index);           // character at index was keyed
    void erased(int start, int stop);
    void macroChanged(int idx, const QString& text);  // 0-based
    void liveChanged(bool on);
    // Emitted whenever new text is sent to the radio's keyer — used by
    // CwxLocalKeyer to drive a sidetone-matching local Morse stream.
    // Includes the WPM in effect at send time so playback timing is
    // self-contained even if speed changes mid-transmission.
    void transmissionRequested(const QString& text, int wpm);
    void transmissionCancelled();        // erase / clearBuffer / interrupt
    void queueEmpty();                   // radio CWX buffer drained — TX teardown required

private:
    void emitExpandedSend(const QVector<SpeedSegment>& segs);

    int     m_speed{20};
    int     m_delay{5};
    int     m_speedStep{3};
    bool    m_qsk{false};
    bool    m_live{false};
    int     m_sentIndex{-1};
    int     m_nextBlock{1};
    int     m_cwxEndIndex{-1};   // radio_index to watch for; -1 = not tracking
    int     m_drainEpoch{0};     // bumped on ESC/clear/disconnect (#3949)
    QString m_macros[12];
};

} // namespace AetherSDR
