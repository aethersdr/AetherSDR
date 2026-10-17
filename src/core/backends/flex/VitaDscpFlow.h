#pragma once

#include <optional>

namespace AetherSDR {

inline constexpr int kVitaDscpEf = 46;
inline constexpr int kVitaIpTosEf = kVitaDscpEf << 2;

// qWAVE's voice class defaults to DSCP 56. Start unmarked and request EF
// explicitly; a denied request must not leave a different priority behind.
template<typename Api>
std::optional<typename Api::FlowId> createVitaEfFlow(Api& api)
{
    typename Api::FlowId flow = 0;
    if (!api.addBestEffortFlow(flow)) {
        return std::nullopt;
    }
    if (!api.setDscp(flow, kVitaDscpEf)) {
        api.removeFlow(flow);
        return std::nullopt;
    }
    return flow;
}

} // namespace AetherSDR
