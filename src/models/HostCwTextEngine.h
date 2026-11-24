#pragma once

#include "core/TxCoordinator.h"

#include <QList>
#include <QPointer>
#include <QString>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace AetherSDR {

class CwxLocalKeyer;
class RadioModel;

// The host CW text engine, one per RadioModel on a hostCwKeyingControl backend.
// It plays CwxModel's segments for one sender at a time; each element is its own
// request under the sender's actor, through queueProducerCwKeyEdge's host-text
// gates. Other senders' messages wait here. Model thread only.
class HostCwTextEngine {
public:
    // Hamlib's bounds: one rigctld argument is MAXARGSZ = 511 characters
    // (tests/rigctl_parse.c) and its send_morse queue HAMLIB_FIFO_SIZE = 1024
    // (src/fifo.h). kMaxPendingChars covers the playing and the waiting text.
    static constexpr int kMaxMessageChars = 511;
    static constexpr int kMaxPendingChars = 1024;
    // A run ends when its keyer drains. Its producer is fenced only once its
    // elements' input is finished: on the HL2 the last key-up is on the wire
    // up to its 100 ms lead and 250 ms shift bound later, then its 5 ms fall,
    // and under break-in the input lasts to the hang's end, at most 2000 ms
    // (#7245). The wait counts polls, so a stalled thread only lengthens it.
    static constexpr int kRetirePollMs = 5;
    static constexpr int kRetireWaitMs = 2500;
    // A run, or a segment played into a drained keyer, starts a word space
    // after the previous last key-up.
    static constexpr int kRunSpacingUnits = 7;

    struct Waiting {
        TxCoordinator::Request sender;
        QString text;   // the message, or the stored macro's text with {CALL} resolved
        int macroIndex{0};
        bool live{false};
    };
    // ended(sender, refused): the run for `sender` is over, after its last key-up.
    // refused: a key-down was refused, so the message did not go out whole.
    using Ended = std::function<void(const TxCoordinator::Request&, bool)>;

    HostCwTextEngine(RadioModel* model, Ended ended);
    ~HostCwTextEngine();
    HostCwTextEngine(const HostCwTextEngine&) = delete;
    HostCwTextEngine& operator=(const HostCwTextEngine&) = delete;

    bool admitsLength(int chars) const;
    int pendingChars() const;
    // A run plays from its first segment until its keyer drains.
    bool playing() const { return m_run != nullptr; }
    // A finished run's producer is not fenced yet: its last element may still
    // be on its way to the wire. Nothing waits on it.
    bool retiring() const { return !m_retiring.empty(); }
    bool playingFor(const TxCoordinator::Request& sender) const;
    // `sender` has a playing run or a waiting message.
    bool hasTextFor(const TxCoordinator::Request& sender) const;

    // Keys one segment for `sender`, appended to its run; `done` finishes when
    // the run ends. False when another sender's run plays or no producer is free.
    bool play(const TxCoordinator::Request& sender, const QString& text, int wpm,
              TxCoordinator::Completion done);
    // Queues a message behind the run; the same sender's messages stay together.
    bool hold(Waiting message);
    // The next waiting message that may start now: the front, when idle, or the
    // playing sender's own. Invalid senders are dropped on the way.
    std::optional<Waiting> takeNext();
    void dropWaiting(const TxCoordinator::Request& sender);

    // Ends `sender`'s run and drops its waiting text; keys up before returning.
    // A finished run's last element, already keyed up, is left to finish.
    void stop(const TxCoordinator::Request& sender);
    // Ends the run, fences every finished run's producer and drops every
    // waiting message.
    void stopAll();
    // stopAll(), and every later play() or hold() is refused.
    void close();

private:
    void endRun();
    // A natural drain ends the run at once and retires its producer.
    void drained(const std::weak_ptr<char>& run);
    void refused(const std::weak_ptr<char>& run);
    // Fences `producer` once its input is finished, checked every
    // kRetirePollMs; past kRetireWaitMs it fences it anyway.
    void retire(TxCoordinator::Producer producer);
    void pollRetiring(int id, int waitedMs);
    void fence(TxCoordinator::Producer& producer);

    struct Retiring {
        TxCoordinator::Producer producer;
        int id{0};
    };

    QPointer<RadioModel> m_model;
    Ended m_ended;
    TxCoordinator::Request m_sender;     // the playing sender's own request
    TxCoordinator::Producer m_producer;  // this run's elements, under the sender's actor
    TxCoordinator::Request m_input;
    std::unique_ptr<CwxLocalKeyer> m_keyer;
    std::shared_ptr<char> m_run;         // this run's identity for worker callbacks
    // This run's last key-up (steady_clock ticks, 0 = none), written on the worker.
    std::shared_ptr<std::atomic<std::chrono::steady_clock::rep>> m_runKeyUp;
    std::chrono::steady_clock::time_point m_lastKeyUp{};   // the previous run's
    std::vector<TxCoordinator::Completion> m_done;
    QList<Waiting> m_waiting;
    std::vector<Retiring> m_retiring;    // finished runs' producers, not fenced yet
    int m_nextRetiringId{0};
    std::shared_ptr<char> m_alive{std::make_shared<char>()};   // for the retire polls
    bool m_closed{false};
};

} // namespace AetherSDR
