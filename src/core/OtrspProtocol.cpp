#include "OtrspProtocol.h"
#include "LogManager.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"
#include "models/TransmitModel.h"

#include <utility>

namespace AetherSDR {

OtrspProtocol::OtrspProtocol(RadioModel* model, int radio1Slice, int radio2Slice)
    : m_model(model)
    , m_radio1Slice(radio1Slice)
    , m_radio2Slice(radio2Slice)
{}

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
                response += processCommand(command);
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
            qCWarning(lcCat) << "OTRSP: focus unavailable for query" << command;
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
        target->setTxSlice(true);
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
