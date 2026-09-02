#pragma once

// When an IO-board frequency push may go out, as a pure decision.
//
// WHY THIS IS A POLICY AND NOT AN `if` CHAIN IN THE BACKEND. The encoder that
// builds the five I2C banks is straightforward and was right first time; every
// defect in this feature has been in the SCHEDULING around it — a guard present
// on the trailing edge of a throttle and missing on the leading one, an armed
// timer surviving a disconnect, a band change coalesced as though it were
// ordinary frequency drift. Those are conditions, not arithmetic, and they are
// only testable if they live somewhere a test can reach without a radio.
//
// The thing being scheduled points an amplifier's band relay. That is the
// reason the ordering below is fixed and commented rather than left to whoever
// next edits the call site.

namespace AetherSDR::hl2 {

enum class IoBoardAction {
    // Push now. The caller sends, records the band, and restarts the cooldown.
    Send,
    // Same-band movement inside the cooldown: remember it as pending and let
    // the timer deliver the latest value when the window expires.
    Coalesce,
    // Transmitting. Do nothing at all, and do NOT stash the value: unkey runs
    // the whole path again and recomputes from the TX receiver, which is
    // fresher than anything held here.
    DeferKeyed,
    // Not connected. Do nothing AND discard any pending value.
    DropDisconnected,
};

// The decision, in the order the conditions must be tested.
//
// DISCONNECTED FIRST, because the consequence outlives the session:
// MetisClient::m_oneShot is cleared by neither start() nor stop(), and the
// IO-board frequency — unlike the filter byte — is not re-primed through
// Params. A bank queued while the link is down therefore becomes the FIRST
// thing the next connect transmits, pointing an amplifier at the band the
// previous session ended on.
//
// KEYED SECOND, because switching a band relay under RF burns its contacts.
// The operator normally cannot retune mid-transmission on this radio, but
// connect-time pushes and the automation bridge can both reach the scheduler
// while MOX is up, and the cost of being wrong is someone's hardware.
//
// BAND CHANGE BEATS THE THROTTLE, because the rate limit exists for VFO sweeps
// (~10 events/second against a board that asks for two) and a band crossing is
// not that case: applyBandFilter moves the physical filter relay immediately,
// so deferring the amplifier leaves the two disagreeing for up to the cooldown
// — and keying in that window is exactly the hazard. Rate-limit frequency
// tracking; never rate-limit which band the amplifier is on.
[[nodiscard]] constexpr IoBoardAction ioBoardAction(bool connected,
                                                    bool keyed,
                                                    bool throttleActive,
                                                    bool bandChanged) noexcept
{
    if (!connected)
        return IoBoardAction::DropDisconnected;
    if (keyed)
        return IoBoardAction::DeferKeyed;
    if (throttleActive && !bandChanged)
        return IoBoardAction::Coalesce;
    return IoBoardAction::Send;
}

}  // namespace AetherSDR::hl2
