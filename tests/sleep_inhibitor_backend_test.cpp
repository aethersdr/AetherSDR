// SleepInhibitor's Linux backend order (#6192): the desktop's own suspend
// inhibitor (GNOME, then KDE) before logind's sleep block, falling through
// when a backend is absent or refuses, and never a screensaver inhibit, so
// the screen still locks while a radio is connected.
#include "core/SleepInhibitor.h"

#include <cstdio>
#include <set>
#include <vector>

using AetherSDR::LinuxSleepBackend;
using AetherSDR::acquireFirstLinuxSleepBackend;

namespace {
int g_failures = 0;

// Runs the order against a desktop where only `accepting` backends take the
// inhibitor; checks which one won and which were tried.
void expect(const std::set<LinuxSleepBackend>& accepting, LinuxSleepBackend want,
            std::vector<LinuxSleepBackend> wantTried, const char* what)
{
    std::vector<LinuxSleepBackend> tried;
    const LinuxSleepBackend got = acquireFirstLinuxSleepBackend([&](LinuxSleepBackend b) {
        tried.push_back(b);
        return accepting.count(b) != 0;
    });
    if (got != want || tried != wantTried) {
        std::fprintf(stderr, "FAIL: %s: got %d after %zu tries, want %d after %zu\n",
                     what, int(got), tried.size(), int(want), wantTried.size());
        ++g_failures;
    }
}
} // namespace

int main()
{
    using B = LinuxSleepBackend;
    expect({B::GnomeSession, B::Logind}, B::GnomeSession, {B::GnomeSession},
           "GNOME: desktop inhibitor, logind never probed");
    expect({B::KdePowerManagement, B::Logind}, B::KdePowerManagement,
           {B::GnomeSession, B::KdePowerManagement}, "KDE desktop");
    expect({B::Logind}, B::Logind, {B::GnomeSession, B::KdePowerManagement, B::Logind},
           "Hyprland: logind only");
    expect({}, B::None, {B::GnomeSession, B::KdePowerManagement, B::Logind}, "nothing available");

    // Releasing an inhibitor that was never acquired touches nothing.
    AetherSDR::SleepInhibitor idle;
    idle.release();
    if (idle.isHeld()) {
        std::fprintf(stderr, "FAIL: release() on an idle inhibitor reports held\n");
        ++g_failures;
    }

    if (g_failures == 0)
        std::printf("sleep_inhibitor_backend_test: all passed\n");
    return g_failures == 0 ? 0 : 1;
}
