#pragma once

#include "core/TxCoordinator.h"

#include <QPointer>
#include <QString>

#include <memory>

namespace AetherSDR {

class CwxLocalKeyer;
class RadioModel;

// CW text keyed by this host for a radio with no text buffer
// (RadioModel::hasHostCwTextKeyer). CwxLocalKeyer times the elements on its
// worker; every edge goes through RadioModel::queueProducerCwKeyEdge, so each
// element passes the same per-edge transmit gates as a paddle. The producer is
// this keyer's own, so stop() fences and releases only its elements. A
// key-down refused at admission, with the TX slice out of CW, or with break-in
// off and no MOX/PTT held, ends the message.
// Construct anywhere; every other member function runs on the model's thread.
class HostCwTextKeyer {
public:
    HostCwTextKeyer(RadioModel* model, QString source);
    ~HostCwTextKeyer();

    HostCwTextKeyer(const HostCwTextKeyer&) = delete;
    HostCwTextKeyer& operator=(const HostCwTextKeyer&) = delete;

    // Bounds on host-keyed text, from Hamlib: rigctld reads one command
    // argument into MAXARGSZ = 511 characters (tests/rigctl_parse.c), and its
    // own send_morse queue is HAMLIB_FIFO_SIZE = 1024 (src/fifo.h). No
    // rigctld client sends more in one line, and Hamlib never holds more.
    static constexpr int kMaxMessageChars = 511;
    static constexpr int kMaxPendingChars = 1024;
    // Whether `chars` more may be queued: at most kMaxMessageChars in one
    // message, and at most kMaxPendingChars with the text already pending
    // (queued, playing, or noted as handed to a queued send()).
    bool admitsLength(int chars) const;
    // Text handed to a send() still waiting in the event queue; send() takes
    // it off again, so a pipelined burst counts before it is delivered.
    void noteQueued(int chars);

    // Queues text at the TransmitModel CW speed. False, with no edge, when
    // RadioModel::hostCwTextRefusal() refuses, the text is past the bounds
    // above, or no producer is available.
    bool send(const QString& text);
    // Drops pending text, fences queued elements and keys up now.
    void stop();

private:
    QPointer<RadioModel> m_model;   // cleared before a dying model drops queued work
    QString m_source;
    TxCoordinator::Producer m_producer;
    TxCoordinator::Request m_input;
    std::unique_ptr<CwxLocalKeyer> m_keyer;
    std::shared_ptr<char> m_run;   // this run's identity for refusal callbacks
    int m_queuedChars{0};          // noteQueued() text not yet through send()
};

} // namespace AetherSDR
