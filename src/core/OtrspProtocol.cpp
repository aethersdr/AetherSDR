#include "OtrspProtocol.h"
#include "LogManager.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"
#include "models/TransmitModel.h"

#include <utility>
#include <initializer_list>

namespace AetherSDR {

OtrspProtocol::OtrspProtocol(RadioModel* model, int radio1Slice, int radio2Slice)
    : m_model(model)
    , m_radio1Slice(radio1Slice)
    , m_radio2Slice(radio2Slice)
{
    m_txDeadline.setSingleShot(true);
    m_txDeadline.setTimerType(Qt::PreciseTimer);
    m_txDeadline.setInterval(250);
    connect(&m_txDeadline, &QTimer::timeout, this, [this] {
        qCWarning(lcCat) << "OTRSP: TX focus confirmation timed out";
        finishTx(m_txRevision);
    });
    if (model) {
        connect(model, &RadioModel::connectionStateChanged, this, [this](bool connected) {
            if (!connected) { cancelPending(); }
        });
        connect(model, &RadioModel::backendRebuilt, this, &OtrspProtocol::cancelPending);
        connect(model, &RadioModel::sliceRemoved, this, [this](int id) {
            if (id == m_radio1Slice || id == m_radio2Slice) { cancelPending(); }
        });
        connect(model, &QObject::destroyed, this, &OtrspProtocol::cancelPending);
    }
}

QByteArray OtrspProtocol::processData(const QByteArray& data)
{
    QByteArray response;
    for (char ch : data) {
        if (m_closed) {
            break;
        }
        if (ch == '\r') {
            const QByteArray command = std::exchange(m_buffer, {});
            const bool discarding = std::exchange(m_discarding, false);
            if (!discarding && !command.isEmpty()) {
                if (m_queuedBytes + command.size() > 4096) {
                    qCWarning(lcCat) << "OTRSP: closing session with excessive pending commands";
                    close();
                    emit transportClosed();
                    break;
                }
                m_commands.enqueue(command);
                m_queuedBytes += command.size();
                response += drainCommands();
            }
        } else if (ch == '\n' && m_buffer.isEmpty() && !m_discarding) {
            // Accept CRLF clients without treating LF as another command.
            continue;
        } else if (!m_discarding) {
            // OTRSP limits commands to 16 bytes, including the terminating CR.
            if (m_buffer.size() >= 15 || ch < ' ' || ch > '~') {
                qCWarning(lcCat) << "OTRSP: discarding malformed or oversized command";
                m_buffer.clear();
                m_discarding = true;
            } else {
                m_buffer += ch;
            }
        }
    }
    return response;
}

void OtrspProtocol::close()
{
    m_closed = true;
    m_buffer.clear();
    m_commands.clear();
    m_queuedBytes = 0;
    m_waitingForTx = false;
    m_txDeadline.stop();
    for (const auto& connection : std::exchange(m_txConnections, {})) {
        disconnect(connection);
    }
    m_txTarget.clear();
}

QByteArray OtrspProtocol::drainCommands()
{
    if (m_processing) { return {}; }
    m_processing = true;
    QByteArray response;
    while (!m_closed && !m_waitingForTx && !m_commands.isEmpty()) {
        const QByteArray command = m_commands.dequeue();
        m_queuedBytes -= command.size();
        response += processCommand(command);
    }
    m_processing = false;
    return response;
}

void OtrspProtocol::finishTx(quint64 revision)
{
    // Queued setters may stop the transport and release its last protocol owner.
    const auto keepAlive = weak_from_this().lock();
    if (!m_waitingForTx || revision != m_txRevision) { return; }
    m_waitingForTx = false;
    m_txDeadline.stop();
    for (const auto& connection : std::exchange(m_txConnections, {})) {
        disconnect(connection);
    }
    m_txTarget.clear();
    if (!m_processing) {
        const QByteArray response = drainCommands();
        if (!response.isEmpty()) { emit responseReady(response); }
    }
}

void OtrspProtocol::observeTx()
{
    if (m_waitingForTx && m_txTarget && m_txTarget == radioSlice(m_txNumber)
        && transmitFocus() == QByteArray("TX") + char('0' + m_txNumber) + '\r') {
        finishTx(m_txRevision);
    }
}

void OtrspProtocol::cancelPending()
{
    if (!m_waitingForTx) { return; }
    qCWarning(lcCat) << "OTRSP: cancelling pending commands after radio/slice change";
    close();
    emit transportClosed();
}

SliceModel* OtrspProtocol::radioSlice(int number) const
{
    if (m_closed || !m_model || !m_model->isConnected()
        || m_radio1Slice < 0 || m_radio2Slice < 0 || m_radio1Slice == m_radio2Slice) {
        return nullptr;
    }
    const int id = number == 1 ? m_radio1Slice : m_radio2Slice;
    return m_model->sliceMayBelongToUs(id) ? m_model->slice(id) : nullptr;
}

QByteArray OtrspProtocol::receiveFocus() const
{
    const SliceModel* first = radioSlice(1);
    const SliceModel* second = radioSlice(2);
    if (!first || !second) {
        return {};
    }
    if (!first->audioMute() && second->audioMute() && first->audioPan() == 50) {
        return QByteArrayLiteral("RX1\r");
    }
    if (first->audioMute() && !second->audioMute() && second->audioPan() == 50) {
        return QByteArrayLiteral("RX2\r");
    }
    if (!first->audioMute() && !second->audioMute()
        && first->isActive() != second->isActive()) {
        const QByteArray focus = first->isActive() ? QByteArrayLiteral("RX1")
                                                 : QByteArrayLiteral("RX2");
        if (first->audioPan() == 0 && second->audioPan() == 100) {
            return focus + "S\r";
        }
        if (first->audioPan() == 100 && second->audioPan() == 0) {
            return focus + "R\r";
        }
    }
    return {};
}

QByteArray OtrspProtocol::transmitFocus() const
{
    const SliceModel* first = radioSlice(1);
    const SliceModel* second = radioSlice(2);
    if (first && first->isTxSlice() && (!second || !second->isTxSlice())) {
        return QByteArrayLiteral("TX1\r");
    }
    if (second && second->isTxSlice() && (!first || !first->isTxSlice())) {
        return QByteArrayLiteral("TX2\r");
    }
    return {};
}

QByteArray OtrspProtocol::processCommand(const QByteArray& command)
{
    if (command == "?NAME") {
        return QByteArrayLiteral("NAMEAetherSDR\r");
    }
    if (command.startsWith('?')) {
        QByteArray response;
        if (command == "?RX") {
            response = receiveFocus();
        } else if (command == "?TX") {
            response = transmitFocus();
        }
        if (!response.isEmpty()) {
            return response;
        }
        if (command == "?RX" || command == "?TX") {
            qCDebug(lcCat) << "OTRSP: focus unavailable for query" << command;
        }
        // Unknown queries (including the bare '?' ping) are echoed by OTRSP.
        return command + '\r';
    }

    const bool tx = command == "TX1" || command == "TX2";
    const bool rx = command == "RX1" || command == "RX2"
        || command == "RX1S" || command == "RX2S"
        || command == "RX1R" || command == "RX2R";
    if (!tx && !rx) {
        qCDebug(lcCat) << "OTRSP: ignoring unsupported command" << command;
        return {};
    }

    const int number = command.at(2) - '0';
    const QPointer<SliceModel> target = radioSlice(number);
    const QPointer<SliceModel> other = radioSlice(number == 1 ? 2 : 1);
    if (!target || (rx && !other)) {
        qCWarning(lcCat) << "OTRSP: mapped slice unavailable for" << command;
        return {};
    }
    if (tx) {
        if (m_model->transmitModel().isTransmitting() && !target->isTxSlice()) {
            qCWarning(lcCat) << "OTRSP: refusing TX focus change while transmitting";
            return {};
        }
        m_txTarget = target;
        m_txNumber = number;
        m_waitingForTx = true;
        const quint64 revision = ++m_txRevision;
        for (int radio : {1, 2}) {
            if (SliceModel* slice = radioSlice(radio)) {
                m_txConnections.append(connect(slice, &SliceModel::txSliceChanged,
                                               this, &OtrspProtocol::observeTx));
                m_txConnections.append(connect(slice, &QObject::destroyed,
                                               this, &OtrspProtocol::cancelPending));
            }
        }
        m_txDeadline.start();
        // Use the typed request's dispatch/reply result; never claim the requested
        // focus before the radio reports it. Later commands remain in wire order.
        const ReceiveDispatch result = m_model->requestTxSlice(target, true, this,
            [this, revision](const RadioControlReply& reply) {
                if (revision != m_txRevision || !m_waitingForTx) { return; }
                if (reply.kind == RadioControlReply::Kind::Cancelled) {
                    cancelPending();
                } else if (reply.kind != RadioControlReply::Kind::Accepted) {
                    qCWarning(lcCat) << "OTRSP: TX focus request failed:" << reply.message;
                    finishTx(revision);
                } else {
                    observeTx();
                }
            });
        if (result == ReceiveDispatch::Unsupported) {
            finishTx(revision);
        } else {
            observeTx();
        }
        return {};
    }

    // Model notifications can synchronously close or replace a mapped slice.
    const auto stillMapped = [this, target, other, number] {
        if (!target || !other || radioSlice(number) != target.data()
            || radioSlice(number == 1 ? 2 : 1) != other.data()) {
            qCWarning(lcCat) << "OTRSP: slice mapping changed during RX focus request";
            return false;
        }
        return true;
    };
    const bool stereo = command.size() == 4;
    if (stereo) {
        const bool reverse = command.at(3) == 'R';
        const QPointer<SliceModel> first = number == 1 ? target : other;
        const QPointer<SliceModel> second = number == 2 ? target : other;
        first->setAudioPan(reverse ? 100 : 0);
        if (!stillMapped()) { return {}; }
        second->setAudioPan(reverse ? 0 : 100);
        if (!stillMapped()) { return {}; }
        first->setAudioMute(false);
        if (!stillMapped()) { return {}; }
        second->setAudioMute(false);
    } else {
        other->setAudioMute(true);
        if (!stillMapped()) { return {}; }
        target->setAudioPan(50);
        if (!stillMapped()) { return {}; }
        target->setAudioMute(false);
    }
    if (!stillMapped()) { return {}; }
    target->setActive(true);
    return {};
}

} // namespace AetherSDR
