#pragma once

#include "RtlReceiverRegistry.h"
#include "Nrsc5FmDecoder.h"
#include <memory>
#include <string>

namespace AetherSDR::rtl {
// Called by the registry preparation executor; destruction and worker joins
// remain on that executor after acquisition acknowledges bank retirement.
std::unique_ptr<RtlReceiverRegistry::Receiver> prepareHdFmReceiver(
    const RtlReceiverRegistry::ReceiverSpec& spec, std::string& error,
    std::unique_ptr<Nrsc5FmDecoder::Pipe> pipe = {},
    std::optional<WdspChannel::Reservation> reservation = {});
} // namespace AetherSDR::rtl
