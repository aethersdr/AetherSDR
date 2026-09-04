// The bridge's connect state, as transitions.
//
// Before this, `radioSnapshot()` carried only a `connected` bool, so a caller
// that issued `connect ip` and got `connected: false` could not tell a connect
// that was working from one that would never finish — the two are the same JSON
// (#5413, item 3). These assert the third value's transitions without a radio,
// a socket or an event loop.

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
          "a DSP build in flight is connecting — the state that did not exist");
    check(connectStateFor(true, false) == ConnectState::Connected,
          "a live link is connected");

    // ---- CONNECTED WINS, and this is the ordering guard ---------------------
    //
    // dspSetupFinished and the backend's connected() are separate signals with
    // no guaranteed order. If a finished arrives after connected — or is lost
    // entirely — the radio must not be reported as still connecting.
    check(connectStateFor(true, true) == ConnectState::Connected,
          "connected beats an in-flight build, whatever order the signals land");

    // ---- A full connect, in order ------------------------------------------
    {
        ConnectState s = connectStateFor(false, false);
        check(s == ConnectState::Idle, "sequence: starts idle");
        s = connectStateFor(false, true);          // dspSetupProgress
        check(s == ConnectState::Connecting, "sequence: DSP begins -> connecting");
        s = connectStateFor(true, false);          // connected(), setup done
        check(s == ConnectState::Connected, "sequence: link up -> connected");
        s = connectStateFor(false, false);         // disconnected()
        check(s == ConnectState::Idle, "sequence: disconnect -> idle");
    }

    // ---- A connect that FAILS returns to idle, not to connecting ------------
    //
    // The case the issue is about: the build ends without a link. If the state
    // stuck at "connecting" a caller would wait forever on a connect that had
    // already given up.
    {
        ConnectState s = connectStateFor(false, true);
        check(s == ConnectState::Connecting, "failed connect: building");
        s = connectStateFor(false, false);         // dspSetupFinished, no link
        check(s == ConnectState::Idle,
              "failed connect: build ended with no link -> idle, NOT stuck connecting");
    }

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
