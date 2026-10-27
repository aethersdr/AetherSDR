#pragma once

#include <QObject>
#include <QString>
#include <QVector>
#include <QList>
#include <atomic>
#include <functional>
#include <memory>
#include "core/backends/CwTextRequest.h"
#include "core/backends/ReceiveCommand.h"

namespace AetherSDR {

class CwxModel : public QObject {
    Q_OBJECT
public:
    explicit CwxModel(QObject* parent = nullptr);
    ~CwxModel() override;

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

    // Read-only TX availability for text keying (#5422). RadioModel wires this
    // to TransmitModel::admitsCwxSend() so no text is queued while TUNE is
    // active (the radio would key it at TUNE power). Unset = always available.
    // This predicate never acquires a coordinator operation.
    using SendAvailability = std::function<bool()>;
    void setSendAvailability(SendAvailability availability) { m_sendAvailability = std::move(availability); }
    // Ask before committing anything (history bubble, editor clear, speed
    // changes): true when a send would be admitted right now. (#5422)
    bool canSend() const { return !m_sendAvailability || m_sendAvailability(); }

    // Actual-send operation fence, separate from a UI's read-only can-send
    // predicate. Checking whether a button is enabled must never acquire TX.
    using TransmissionPermit = std::function<bool()>;
    // Capture on the model thread; the returned cancellation check may be
    // read at a transport writer. It conveys batch validity, not TX authority,
    // and never dereferences this QObject from a worker thread.
    TransmissionPermit queuedTransmissionPermit() const;
    using TransmissionAdmission = std::function<TransmissionPermit()>;
    void setTransmissionAdmission(TransmissionAdmission admission) { m_transmissionAdmission = std::move(admission); }
    // Optional neutral backend dispatch. False rejects the batch before the
    // sidetone notification; the radio-specific command path stays separate.
    using TextSender = std::function<bool(const QString&, int)>;
    void setTextSender(TextSender sender) { m_textSender = std::move(sender); }
    using RequestSender = std::function<ReceiveDispatch(const CwTextRequest&, int, int, TransmissionPermit)>;
    void setRequestSender(RequestSender sender) { m_requestSender = std::move(sender); }
    // Explicit synchronous route for an engine producer. Every command keeps
    // the request captured by that controller; no temporary ambient identity
    // is installed around UI signals or a nested event loop.
    struct TransmissionRoute {
        TransmissionAdmission admit;
        TextSender text;
        RequestSender command; // nChars < 0: no drain-index reply
        std::function<void(int, bool)> dispatched;
    };

    // Actions
    void send(const QString& text);      // Send mode: full string
    void send(const QString& text, const TransmissionRoute& route);
    void sendChar(const QString& ch);    // Live mode: single char
    void sendChar(const QString& ch, const TransmissionRoute& route);
    void sendMacro(int idx);             // 1-based (1=F1, 12=F12)
    void sendMacro(int idx, const TransmissionRoute& route);
    void saveMacro(int idx, const QString& text); // 0-based
    void erase(int numChars);
    void clearBuffer();
    // Abort any in-flight queue-drain watch and bump the epoch so replies for
    // the discarded batch are rejected. Called on ESC/clear and on disconnect
    // (via RadioModel::onDisconnected) so a stale m_cwxEndIndex can't wedge the
    // monotonic guard across a reconnect. (#3949)
    void resetDrainWatch();
    // An unknown-length macro appended to a batch invalidates its end index,
    // but must not cancel text that is still queued for transport delivery.
    void abandonDrainWatch();
    void setSpeed(int wpm);
    // Adopt a radio-authoritative speed without emitting a command back to the
    // radio. Used by non-Flex keyers after their connect-time readback.
    void adoptSpeed(int wpm);
    void setSpeedModifiersEnabled(bool enabled) { m_speedModifiersEnabled = enabled; }
    void setDelay(int ms);
    void setSpeedStep(int step);
    void setQsk(bool on);
    void setLive(bool on);

    // Status parsing (from radio)
    void applyStatus(const QMap<QString, QString>& kvs);
    // A queued reply names the first character, not the last. An old batch's
    // response cannot re-arm a replacement watch; handoff is not radio idle.
    void handleSendReply(const CwTextReply& reply, int epoch, int nChars);

    // Parses text for leading +/- speed modifiers on words.
    // A +/- run at word-start (after space or string-start) immediately
    // followed by a non-space, non-modifier character counts as a modifier.
    // Standalone +/- not followed by word chars are prosigns/hyphens and
    // pass through unchanged.  Speed resets to baseWpm after each modified
    // word.  Returns a single segment at baseWpm when no modifiers found.
    static QVector<SpeedSegment> expandSpeedModifiers(
        const QString& text, int baseWpm, int step);

signals:
    // Intent notifications only. The synchronous sender owns actual dispatch
    // and its receipt; a signal observer must never send the request again.
    void controlRequested(const AetherSDR::CwTextRequest& request);
    void replyControlRequested(const AetherSDR::CwTextRequest& request, int epoch, int nChars);
    void speedChanged(int wpm);
    void requestRejected(const QString& reason);
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
    // All synchronous sends in this batch have been handed off. This is NOT
    // radio-idle evidence. untrackedMacro has no client-side drain index.
    void transmissionDispatched(int epoch, bool untrackedMacro);
    void queueEmpty();                   // radio CWX buffer drained — TX teardown required

private:
    TransmissionPermit admitTransmission(const TransmissionRoute& route = {});
    void emitExpandedSend(const QVector<SpeedSegment>& segs, const TransmissionPermit& permit,
                          const TransmissionRoute& route = {});
    bool notifyTransmission(const QString& text, int wpm, const TransmissionPermit& permit,
                            const TransmissionRoute& route = {});
    ReceiveDispatch dispatchRequest(const CwTextRequest& request, int epoch, int nChars,
                                    const TransmissionRoute& route = {});
    bool dispatchSpeed(int wpm, CwTextRequest::Origin origin, int epoch,
                       const TransmissionRoute& route);
    void retireSpeedIntent();
    SendAvailability m_sendAvailability;
    TransmissionAdmission m_transmissionAdmission;
    TextSender m_textSender;
    RequestSender m_requestSender;
    bool m_clearing{false}; // synchronous abort notifications cannot replace the queue being cleared
    std::shared_ptr<std::atomic<bool>> m_queueValid{std::make_shared<std::atomic<bool>>(true)};

    int     m_speed{20};
    int     m_delay{5};
    int     m_speedStep{3};
    // Bounded expected transient values, not a count that consumes unrelated
    // radio updates. Refusal removes its ticket; edits and session reset retire
    // both queued speed writes and pending echo suppression.
    QList<std::shared_ptr<const int>> m_pendingWpmEchoes;
    std::shared_ptr<std::atomic<bool>> m_speedIntentValid{std::make_shared<std::atomic<bool>>(true)};
    bool    m_qsk{false};
    bool    m_live{false};
    bool    m_speedModifiersEnabled{true};
    int     m_sentIndex{-1};
    quint64 m_nextBlock{1};
    int     m_cwxEndIndex{-1};   // radio_index to watch for; -1 = not tracking
    int     m_drainEpoch{0};     // bumped on ESC/clear/disconnect (#3949)
    QString m_macros[12];
};

} // namespace AetherSDR
