#pragma once

#include "ControlProtocolCodec.h"
#include "RadioConnectionTarget.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QPointer>
#include <QThread>

namespace AetherSDR::control {

// Shared conservative receive admission, NOT the Stage-4 TX arbiter. A false
// command edge is never evidence that a transmitter is idle. This object is
// bound before connecting and never survives its radio/connection dependencies.
class ReceiveControlGuard final : public QObject {
public:
    ReceiveControlGuard(RadioModel* radio, RadioConnectionTarget* connection)
        : m_radio(radio), m_connection(connection)
    {
        connect(radio, &RadioModel::radioTransmitConfirmed, this,
                [this](bool transmitting) { m_confirmedIdle = !transmitting; });
        const auto invalidateOnActive = [this](bool active) {
            if (active) {
                m_confirmedIdle = false;
            }
        };
        connect(radio, &RadioModel::radioTransmittingChanged, this, invalidateOnActive);
        TransmitModel* transmit = &radio->transmitModel();
        connect(transmit, &TransmitModel::transmittingChanged, this, invalidateOnActive);
        connect(transmit, &TransmitModel::moxChanged, this, invalidateOnActive);
        connect(transmit, &TransmitModel::tuneChanged, this, invalidateOnActive);
        connect(radio, &RadioModel::connectionStateChanged, this, [this](bool connected) {
            if (!connected) {
                m_confirmedIdle = false;
            }
        });
        connect(radio, &RadioModel::backendRebuilt, this, [this] { m_confirmedIdle = false; });
        connect(connection, &RadioConnectionTarget::stateChanged, this, [this] {
            if (!m_connection || m_connection->state() != RadioConnectionTarget::State::Connected) {
                m_confirmedIdle = false;
            }
        });
    }

    [[nodiscard]] bool ready() const
    {
        return thread() == QThread::currentThread()
            && m_radio && m_radio->thread() == thread()
            && m_connection && m_connection->thread() == thread()
            && m_connection->state() == RadioConnectionTarget::State::Connected
            && m_radio->isConnected() && m_radio->backend()
            && m_radio->backend()->thread() == thread();
    }

    [[nodiscard]] std::optional<ProtocolError> checkSlice(int id) const
    {
        if (!ready()) {
            return refusal("request.conflict", "radio connection is not ready");
        }
        SliceModel* slice = m_radio->slice(id);
        if (!slice || !m_radio->isSlotOurs(id) || m_radio->isSlotForeign(id)) {
            return refusal("resource.not_found", "owned slice unavailable");
        }
        if (slice->isLocked()) {
            return refusal("request.conflict", "slice is locked");
        }
        return {};
    }

    [[nodiscard]] std::optional<ProtocolError> checkTransmit(const RadioCapabilities& caps) const
    {
        if (!ready()) {
            return refusal("request.conflict", "radio connection is not ready");
        }
        if (caps.canTransmit
            && (!m_confirmedIdle || m_radio->isRadioTransmitting()
                || m_radio->transmitModel().isTransmitting()
                || m_radio->transmitModel().isMox()
                || m_radio->transmitModel().isTuning())) {
            return refusal("request.conflict", "transmitter is active or idle state is unconfirmed");
        }
        return {};
    }

    [[nodiscard]] static bool canBind(RadioModel* radio, RadioConnectionTarget* connection)
    {
        return radio && connection && radio->thread() == QThread::currentThread()
            && connection->thread() == QThread::currentThread()
            && !radio->isConnected() && !radio->isConnectAttemptInFlight()
            && connection->state() == RadioConnectionTarget::State::Idle;
    }

    [[nodiscard]] static ProtocolError refusal(const char* code, const char* message)
    {
        return {QString::fromLatin1(code), QString::fromLatin1(message), {}, false};
    }

private:
    QPointer<RadioModel> m_radio;
    QPointer<RadioConnectionTarget> m_connection;
    bool m_confirmedIdle{false};
};

} // namespace AetherSDR::control
