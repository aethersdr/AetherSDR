#pragma once

#include "core/backends/icom/CivCodec.h"
#include "core/backends/icom/IcomModels.h"

#include <cstdint>
#include <optional>

namespace AetherSDR::icom {

enum class ConnectIdentityResult : std::uint8_t {
    Identified,
    Rejected,
    TimedOut,
};

enum class ConnectPowerAction : std::uint8_t {
    Continue,
    RetrySession,
    Wake,
    Stop,
};

enum class PostWakeStallAction : std::uint8_t {
    ReportError,
    ReconnectQuietly,
};

// connectRadio() is reached only after the operator selected a radio, supplied
// its credentials and requested Connect. For a hardware-verified model that is
// explicit permission to wake: both FA and silence are observed IC-9700
// standby shapes. Presence of powerOn still means this exact model/transport
// pair survived the hardware sequence; command-table similarity is not
// evidence. The caller supplies whether the hardware-bounded wake budget has
// been exhausted; IC-9700 cold RS-BA1 recovery needs at most two fresh-session
// attempts on the tested radio.
[[nodiscard]] inline ConnectPowerAction connectPowerAction(
    const IcomModel* model, ConnectIdentityResult result,
    bool sessionRetryAttempted, bool wakeBudgetExhausted)
{
    if (result == ConnectIdentityResult::Identified) {
        return ConnectPowerAction::Continue;
    }
    if (!model || !profileFor(*model).powerOn) {
        return ConnectPowerAction::Stop;
    }
    // A newly opened RS-BA1 CI-V pipe can be slow even while the radio is fully
    // awake. Preserve the original connect path by proving silence on one fresh
    // session before interpreting a timeout as standby. An explicit FA is a
    // real negative reply, not startup latency, and may proceed directly.
    if (result == ConnectIdentityResult::TimedOut && !sessionRetryAttempted) {
        return ConnectPowerAction::RetrySession;
    }
    if (!wakeBudgetExhausted) {
        return ConnectPowerAction::Wake;
    }
    return ConnectPowerAction::Stop;
}

// The IC-9700 can answer during early boot and then briefly lose CI-V while
// the RS-BA1 transport stays authenticated. One silent fresh session is part
// of wake convergence; a second stall is a real fault and must remain visible.
[[nodiscard]] inline PostWakeStallAction postWakeStallAction(
    bool wakeRecently, bool quietReconnectIssued)
{
    return wakeRecently && !quietReconnectIssued
        ? PostWakeStallAction::ReconnectQuietly
        : PostWakeStallAction::ReportError;
}

}  // namespace AetherSDR::icom
