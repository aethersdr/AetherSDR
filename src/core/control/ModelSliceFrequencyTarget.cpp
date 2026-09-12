#include "SliceFrequencyTarget.h"
#include "ReceiveControlGuard.h"

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
        : m_radio(radio), m_guard(radio, connection)
    {}

    bool available() const override
    {
        if (thread() != QThread::currentThread() || !m_guard.ready()) {
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
    std::optional<ProtocolError> checkSlice(int sliceId) const
    {
        if (thread() != QThread::currentThread()) {
            return refusal("request.conflict", "radio connection is not ready");
        }
        if (const std::optional<ProtocolError> error = m_guard.checkSlice(sliceId)) {
            return error;
        }
        SliceModel* slice = m_radio->slice(sliceId);
        const RadioCapabilities caps = m_radio->backendCapabilities();
        if (!validCoverage(caps) || !slice->frequencyReportedKnown()) {
            return refusal("capability.unavailable", "frequency coverage or observation unavailable");
        }
        // TX-slice designation alone is not keying: this receive intent may
        // retune it while confirmed idle. Any lease/inhibit policy coupling
        // belongs to the Stage 4 arbiter before TX-capable daemon enablement.
        return m_guard.checkTransmit(caps);
    }

    QPointer<RadioModel> m_radio;
    ReceiveControlGuard m_guard;
};

} // namespace

std::unique_ptr<SliceFrequencyTarget> makeModelSliceFrequencyTarget(
    RadioModel* radio, RadioConnectionTarget* connection)
{
    if (!ReceiveControlGuard::canBind(radio, connection)) {
        return {};
    }
    return std::make_unique<ModelSliceFrequencyTarget>(radio, connection);
}

} // namespace AetherSDR::control
