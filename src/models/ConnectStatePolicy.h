#pragma once

// What the bridge should call the connection, as a pure decision.
//
// `radioSnapshot()` exposes `connected` as a bare bool, so *a connect that is
// working* and *nothing happening at all* are the same JSON. A headless caller
// that issues `connect ip` sees `connected: false` for both and cannot tell
// them apart (#5413, item 3).
//
// THE SECOND INPUT IS THE WHOLE ATTEMPT, NOT THE DSP SUB-PHASE, and the first
// version of this file got that wrong. It read Hl2Backend's
// dspSetupProgress/dspSetupFinished into a flag of its own, which broke in both
// directions:
//
//   * FALSE `idle` DURING A LIVE CONNECT. On a successful HL2 connect,
//     finishDspSetup() starts Metis and emits dspSetupFinished() immediately,
//     while connected() cannot happen until the first EP6 packet raises
//     linkUp(). Across that real, watchdog-covered interval isConnected() is
//     false and the DSP flag is false, so the field said `idle` about an
//     attempt that was still running — exactly the confusion it exists to end.
//
//   * STALE `connecting` AFTER CANCELLATION. disconnectFromRadio() clears the
//     attempt immediately, but a DSP build cannot be cancelled, so the DSP flag
//     stayed true until the I/O thread returned. And with no link ever up,
//     MetisClient::stop() need not emit linkDown(), so onDisconnected() — the
//     claimed reset — was not guaranteed to run at all. A backend swap could
//     destroy the signal source with the flag still set.
//
// RadioModel already owns the lifecycle that answers this correctly:
// m_connectAttemptActive, behind isConnectAttemptInFlight(). It is set at the
// request edge in connectToRadio() and cleared when the attempt lands, fails,
// or is abandoned (#4912) — one lifecycle, maintained in one place, rather than
// a second one owned by a display field. `attemptInFlight` is that.
//
// Pure, so the transitions are testable without a radio, a socket, an event loop
// or a GUI — the layer #5358 asks for. But a pure function cannot prove the
// model passes it the right arguments: the production mapping is covered
// through RadioModel and the bridge snapshot in
// tests/automation_connect_wait_phase_test.cpp, which is where #4912's
// lifecycle is already exercised.
//
// The strings are fixed here rather than at the call site because they are
// protocol: a caller matching on "connecting" must not have to care which file
// spelled it.

namespace AetherSDR {

enum class ConnectState {
    Idle,        // nothing in progress
    Connecting,  // an attempt is in flight and has not completed
    Connected,   // the link is up
};

inline ConnectState connectStateFor(bool connected, bool attemptInFlight)
{
    // Connected is checked FIRST and deliberately. The attempt flag is cleared
    // by onConnected(), but nothing orders that against the backend's own
    // connected() edge, and a live radio must never be reported as still
    // connecting because one bookkeeping clear has not run yet.
    if (connected) {
        return ConnectState::Connected;
    }
    if (attemptInFlight) {
        return ConnectState::Connecting;
    }
    return ConnectState::Idle;
}

// PROTOCOL STRINGS. Existing scripts read the `connected` bool and are
// untouched; these are the new third value beside it.
inline const char* connectStateName(ConnectState s)
{
    switch (s) {
    case ConnectState::Connected:  return "connected";
    case ConnectState::Connecting: return "connecting";
    case ConnectState::Idle:       break;
    }
    return "idle";
}

}  // namespace AetherSDR
