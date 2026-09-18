// The bridge's connect state, as transitions.
//
// Before this, `radioSnapshot()` carried only a `connected` bool, so a caller
// that issued `connect ip` and got `connected: false` could not tell a connect
// that was working from one that would never finish — the two are the same JSON
// (#5413, item 3). These assert the third value's transitions without a radio,
// a socket or an event loop.
//
// WHAT THIS FILE CANNOT PROVE, stated here because a reviewer proved it: a pure
// function's test cannot tell whether the model passes it the right arguments.
// The first version of this field read a DSP-only flag, and mutating the
// production wiring to leave that flag false left every check below green. The
// production mapping — RadioModel::connectState() and the bridge snapshot — is
// covered in tests/connect_state_model_test.cpp through injected model state
// and the production cancellation/error handlers.

#include "models/ConnectStatePolicy.h"

#include <cstdio>
#include <cstring>

using AetherSDR::ConnectState;
using AetherSDR::connectStateFor;
using AetherSDR::connectStateName;

namespace {

int g_failures = 0;

void check(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", what);
    if (!ok) {
        ++g_failures;
    }
}

bool named(ConnectState s, const char* expect)
{
    return std::strcmp(connectStateName(s), expect) == 0;
}

}  // namespace

int main()
{
    // ---- The three states --------------------------------------------------
    check(connectStateFor(false, false) == ConnectState::Idle,
          "nothing happening is idle");
    check(connectStateFor(false, true) == ConnectState::Connecting,
          "an attempt in flight is connecting — the state that did not exist");
    check(connectStateFor(true, false) == ConnectState::Connected,
          "a live link is connected");

    // ---- CONNECTED WINS, and this is the ordering guard ---------------------
    //
    // onConnected() clears the attempt flag, but nothing orders that against
    // the backend's own connected() edge. A live radio must not be reported as
    // still connecting because one bookkeeping clear has not run yet.
    check(connectStateFor(true, true) == ConnectState::Connected,
          "connected beats an in-flight attempt, whatever order the edges land");

    // ---- A full connect, in order ------------------------------------------
    {
        ConnectState s = connectStateFor(false, false);
        check(s == ConnectState::Idle, "sequence: starts idle");
        s = connectStateFor(false, true);          // connectToRadio()
        check(s == ConnectState::Connecting, "sequence: request -> connecting");
        s = connectStateFor(true, false);          // link up, attempt landed
        check(s == ConnectState::Connected, "sequence: link up -> connected");
        s = connectStateFor(false, false);         // disconnected()
        check(s == ConnectState::Idle, "sequence: disconnect -> idle");
    }

    // ---- A connect that FAILS returns to idle, not to connecting ------------
    //
    // The case the issue is about: the attempt ends without a link. If the
    // state stuck at "connecting" a caller would wait forever on a connect that
    // had already given up.
    {
        ConnectState s = connectStateFor(false, true);
        check(s == ConnectState::Connecting, "failed connect: in flight");
        s = connectStateFor(false, false);         // attempt cleared, no link
        check(s == ConnectState::Idle,
              "failed connect: attempt ended with no link -> idle, NOT stuck connecting");
    }

    // ---- The window a DSP-only flag reported as `idle` -----------------------
    //
    // On a successful HL2 connect the DSP build finishes and Metis starts, but
    // connected() waits for the first EP6 packet. A DSP-only input is false
    // across that whole interval while the attempt is very much alive; the
    // attempt input is not.
    check(connectStateFor(/*connected=*/false, /*attemptInFlight=*/true)
              == ConnectState::Connecting,
          "the post-DSP, pre-first-packet window is connecting, not idle");

    // ---- The wire strings are protocol --------------------------------------
    check(named(ConnectState::Idle, "idle"), "idle spells \"idle\"");
    check(named(ConnectState::Connecting, "connecting"), "connecting spells \"connecting\"");
    check(named(ConnectState::Connected, "connected"), "connected spells \"connected\"");

    if (g_failures == 0) {
        std::printf("\nALL PASS\n");
        return 0;
    }
    std::printf("\nFAILURES PRESENT\n");
    return 1;
}
