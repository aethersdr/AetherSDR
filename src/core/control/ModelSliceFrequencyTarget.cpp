#include "SliceFrequencyTarget.h"

#include "RadioConnectionTarget.h"
#include "core/backends/IRadioBackend.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QPointer>
#include <QThread>

#include <cmath>

namespace AetherSDR::control {
namespace {

ProtocolError refusal(const char* code, const char* message)
{
    return {QString::fromLatin1(code), QString::fromLatin1(message), {}, false};
}

bool validCoverage(const RadioCapabilities& caps)
{
    const SliceFrequencyControl& control = caps.sliceFrequencyControl;
    if (control.authority == SliceFrequencyControl::Authority::Unknown
        || control.minimumHz <= 0 || control.maximumHz < control.minimumHz
        || control.maximumHz > SliceModel::kMaximumReportedFrequencyHz) {
        return false;
    }
    for (const DeclaredBandRange& band : caps.declaredBandRanges) {
        if (!std::isfinite(band.lowHz) || !std::isfinite(band.highHz)
            || band.lowHz <= 0 || band.highHz < band.lowHz) {
            return false;
        }
    }
    return true;
}

class ModelSliceFrequencyTarget final : public SliceFrequencyTarget {
public:
    ModelSliceFrequencyTarget(RadioModel* radio, RadioConnectionTarget* connection)
        : m_radio(radio), m_connection(connection)
    {
        // A command edge or construction default is NOT an idle observation.
        connect(radio, &RadioModel::radioTransmitConfirmed, this, [this](bool tx) {
            m_confirmedIdle = !tx;
        });
        connect(radio, &RadioModel::radioTransmittingChanged, this, [this](bool tx) {
            if (tx) {
                m_confirmedIdle = false;
            }
        });
        // A locally initiated keying interval can precede radio readback.
        // Do not reuse an idle observation from before that interval ends.
        const auto invalidateOnActive = [this](bool active) {
            if (active) {
                m_confirmedIdle = false;
            }
        };
        TransmitModel* transmit = &radio->transmitModel();
        connect(transmit, &TransmitModel::transmittingChanged, this, invalidateOnActive);
        connect(transmit, &TransmitModel::moxChanged, this, invalidateOnActive);
        connect(transmit, &TransmitModel::tuneChanged, this, invalidateOnActive);
        connect(radio, &RadioModel::connectionStateChanged, this, [this](bool connected) {
            if (!connected) {
                m_confirmedIdle = false;
            }
        });
        connect(radio, &RadioModel::backendRebuilt, this, [this] {
            m_confirmedIdle = false;
        });
        connect(connection, &RadioConnectionTarget::stateChanged, this, [this] {
            if (!m_connection || m_connection->state() != RadioConnectionTarget::State::Connected) {
                m_confirmedIdle = false;
            }
        });
    }

    bool available() const override
    {
        if (!ready()) {
            return false;
        }
        for (SliceModel* slice : m_radio->slices()) {
            if (slice && !checkSlice(slice->sliceId())) {
                return true;
            }
        }
        return false;
    }

    std::optional<ProtocolError> setFrequency(int sliceId, qint64 hz) override
    {
        if (const std::optional<ProtocolError> error = checkSlice(sliceId)) {
            return error;
        }
        const RadioCapabilities caps = m_radio->backendCapabilities();
        const SliceFrequencyControl& control = caps.sliceFrequencyControl;
        bool inRange = hz >= control.minimumHz && hz <= control.maximumHz;
        if (inRange && !caps.declaredBandRanges.isEmpty()) {
            inRange = false;
            for (const DeclaredBandRange& band : caps.declaredBandRanges) {
                if (hz >= band.lowHz && hz <= band.highHz) {
                    inRange = true;
                    break;
                }
            }
        }
        if (!inRange) {
            return refusal("request.out_of_range", "hz is outside the supported receive coverage");
        }
        // Do not call the optimistic desktop/CAT setters. No event-loop
        // yielding or saved pointer crosses validation and this typed dispatch.
        m_radio->backend()->setSliceFrequency(sliceId, static_cast<double>(hz));
        return std::nullopt;
    }

private:
    bool ready() const
    {
        return thread() == QThread::currentThread()
            && m_radio && m_radio->thread() == thread()
            && m_connection && m_connection->thread() == thread()
            && m_connection->state() == RadioConnectionTarget::State::Connected
            && m_radio->isConnected() && m_radio->backend()
            && m_radio->backend()->thread() == thread();
    }

    std::optional<ProtocolError> checkSlice(int sliceId) const
    {
        if (!ready()) {
            return refusal("request.conflict", "radio connection is not ready");
        }
        SliceModel* slice = m_radio->slice(sliceId);
        if (!slice || !m_radio->isSlotOurs(sliceId) || m_radio->isSlotForeign(sliceId)) {
            return refusal("resource.not_found", "owned slice unavailable");
        }
        if (slice->isLocked()) {
            return refusal("request.conflict", "slice is locked");
        }
        const RadioCapabilities caps = m_radio->backendCapabilities();
        if (!validCoverage(caps) || !slice->frequencyReportedKnown()) {
            return refusal("capability.unavailable", "frequency coverage or observation unavailable");
        }
        // TX-slice designation alone is not keying: this receive intent may
        // retune it while confirmed idle. Any lease/inhibit policy coupling
        // belongs to the Stage 4 arbiter before TX-capable daemon enablement.
        if (caps.canTransmit
            && (!m_confirmedIdle || m_radio->isRadioTransmitting()
                || m_radio->transmitModel().isTransmitting()
                || m_radio->transmitModel().isMox()
                || m_radio->transmitModel().isTuning())) {
            return refusal("request.conflict", "transmitter is active or idle state is unconfirmed");
        }
        return std::nullopt;
    }

    QPointer<RadioModel> m_radio;
    QPointer<RadioConnectionTarget> m_connection;
    bool m_confirmedIdle{false};
};

} // namespace

std::unique_ptr<SliceFrequencyTarget> makeModelSliceFrequencyTarget(
    RadioModel* radio, RadioConnectionTarget* connection)
{
    if (!radio || !connection || radio->thread() != QThread::currentThread()
        || connection->thread() != QThread::currentThread()
        || radio->isConnected() || radio->isConnectAttemptInFlight()
        || connection->state() != RadioConnectionTarget::State::Idle) {
        return {};
    }
    return std::make_unique<ModelSliceFrequencyTarget>(radio, connection);
}

} // namespace AetherSDR::control
