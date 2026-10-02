#pragma once

// Bridge connection state as a pure decision (#5413): `connected` alone can't
// distinguish a working connect from nothing happening.
// The attempt input is RadioModel::isConnectAttemptInFlight() (the whole attempt,
// set at the connectToRadio() request edge, cleared on land/fail/abandon, #4912),
// not a backend DSP sub-phase, which reads false mid-connect on HL2 (before the
// first EP6 packet) and can stay true after cancellation.
// Model wiring is covered by tests/connect_state_model_test.cpp. The strings are
// protocol; callers match on them.

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
