#pragma once

// What the bridge should call the connection, as a pure decision.
//
// `radioSnapshot()` exposes `connected` as a bare bool, so *opening the DSP* and
// *idle* are the same JSON. A headless caller that issues `connect ip` therefore
// sees `connected: false` both while the connect is working and when nothing is
// happening at all, and cannot tell them apart (#5413, item 3).
//
// The two inputs are the only ones that matter, and neither is a timer:
//
//   `connected`      — the link is up. Whatever else is true, this wins: the
//                      DSP-setup signals are not ordered against connected() and
//                      a late dspSetupFinished must never demote a live radio.
//   `dspSetupInFlight` — a DSP build has begun and has not reported finishing.
//
// Pure, so the transitions are testable without a radio, a socket, an event loop
// or a GUI — the layer #5358 asks for. The strings are fixed here rather than at
// the call site because they are protocol: a caller matching on "connecting"
// must not have to care which file spelled it.

namespace AetherSDR {

enum class ConnectState {
    Idle,        // nothing in progress
    Connecting,  // a DSP build is running; the connect has not completed
    Connected,   // the link is up
};

inline ConnectState connectStateFor(bool connected, bool dspSetupInFlight)
{
    // Connected is checked FIRST and deliberately. dspSetupFinished and the
    // backend's connected() are separate signals with no guaranteed order, so a
    // finished-after-connected delivery would otherwise report "connecting" for
    // a radio that is already streaming.
    if (connected) {
        return ConnectState::Connected;
    }
    if (dspSetupInFlight) {
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
