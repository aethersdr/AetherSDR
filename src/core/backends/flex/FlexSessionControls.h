#pragma once

#include "core/backends/SessionControlRequest.h"

namespace AetherSDR::FlexSessionControls {
QString encode(const SessionControlRequest& request);
SessionControlReply decode(SessionControlField field, const RadioControlReply& reply);
RadioDelta infoMetadata(const QString& body);
} // namespace AetherSDR::FlexSessionControls
