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

    // Queues text at the TransmitModel CW speed. False, with no edge, when
    // RadioModel::hostCwTextRefusal() refuses or no producer is available.
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
};

} // namespace AetherSDR
