#pragma once

#include "ControlProtocolCodec.h"

#include <QObject>

#include <memory>
#include <optional>

namespace AetherSDR {
class RadioModel;

namespace control {
class RadioConnectionTarget;

// Trusted, owning-thread, non-keying intent seam. Implementations revalidate
// live identities and state before dispatch; success is acceptance, not an ACK.
class SliceFrequencyTarget : public QObject {
public:
    using QObject::QObject;
    ~SliceFrequencyTarget() override = default;
    [[nodiscard]] virtual bool available() const = 0;
    [[nodiscard]] virtual std::optional<ProtocolError> setFrequency(
        int sliceId, qint64 hz) = 0;
};

// Both dependencies must outlive the target and remain on its owning thread.
[[nodiscard]] std::unique_ptr<SliceFrequencyTarget> makeModelSliceFrequencyTarget(
    RadioModel* radio, RadioConnectionTarget* connection);

} // namespace control
} // namespace AetherSDR
