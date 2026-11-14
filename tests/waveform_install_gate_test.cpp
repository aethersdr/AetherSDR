// Unit tests for the Docker-waveform install gate policy (#4210). The key
// regression guard: install availability follows the radio's LIVE WFP runtime
// state plus the no-WFP-hardware platform check, and is NOT influenced by the
// "wfp" license feature (which #4186 wrongly added as a gate, blocking installs
// on radios that were powered, ready, and even already running a waveform).

#include "gui/WaveformInstallGate.h"

#include <cstdio>

using namespace AetherSDR;
using B = DockerWaveformInstallBlocker;

namespace {

int g_failed = 0;
int g_total = 0;

void report(const char* label, bool ok)
{
    ++g_total;
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", label);
    if (!ok) {
        ++g_failed;
    }
}

// Helper: the gate never takes a license-feature argument — that's the point.
B gate(bool connected, RadioPlatform platform,
       bool seen, bool powered, bool ready)
{
    return dockerWaveformInstallBlocker(connected, platform, seen, powered, ready);
}

}  // namespace

int main()
{
    // ── The #4210 regression case ───────────────────────────────────────────
    // A connected WFP-capable radio that is powered + ready installs fine. There
    // is no license-feature input at all, so a disabled "wfp" entitlement can no
    // longer block it — exactly the behavior #4186 broke.
    report("BigBend powered+ready -> None (install allowed regardless of license)",
           gate(true, RadioPlatform::BigBend, true, true, true) == B::None);
    report("DragonFire powered+ready -> None",
           gate(true, RadioPlatform::DragonFire, true, true, true) == B::None);

    // ── Not connected ───────────────────────────────────────────────────────
    report("not connected -> NotConnected",
           gate(false, RadioPlatform::BigBend, true, true, true) == B::NotConnected);
    report("not connected wins over every other input",
           gate(false, RadioPlatform::Microburst, false, false, false)
               == B::NotConnected);

    // ── Genuine hard incompatibility: no WFP hardware (kept from #4186) ──────
    report("Microburst -> UnsupportedPlatform (no WFP hardware)",
           gate(true, RadioPlatform::Microburst, true, true, true)
               == B::UnsupportedPlatform);
    report("DeepEddy -> UnsupportedPlatform",
           gate(true, RadioPlatform::DeepEddy, true, true, true)
               == B::UnsupportedPlatform);
    report("isKnownNonWfpPlatform: Microburst/DeepEddy true, others false",
           isKnownNonWfpPlatform(RadioPlatform::Microburst)
               && isKnownNonWfpPlatform(RadioPlatform::DeepEddy)
               && !isKnownNonWfpPlatform(RadioPlatform::BigBend)
               && !isKnownNonWfpPlatform(RadioPlatform::DragonFire)
               && !isKnownNonWfpPlatform(RadioPlatform::Unknown));

    // ── Live runtime gating ─────────────────────────────────────────────────
    report("status not yet seen -> RuntimeStatusUnknown",
           gate(true, RadioPlatform::BigBend, false, true, true)
               == B::RuntimeStatusUnknown);
    report("not powered -> WfpNotReady",
           gate(true, RadioPlatform::BigBend, true, false, true) == B::WfpNotReady);
    report("not ready -> WfpNotReady",
           gate(true, RadioPlatform::BigBend, true, true, false) == B::WfpNotReady);
    report("neither powered nor ready -> WfpNotReady",
           gate(true, RadioPlatform::BigBend, true, false, false) == B::WfpNotReady);

    // ── Ordering: hard incompatibility outranks runtime state ───────────────
    report("unsupported platform outranks not-yet-seen runtime status",
           gate(true, RadioPlatform::Microburst, false, false, false)
               == B::UnsupportedPlatform);

    // ── Remote Access package: legacy waveform exactly where Docker can't go ──
    report("Microburst -> native remote-access package",
           remoteAccessUsesNativePackage(RadioPlatform::Microburst));
    report("DeepEddy -> native remote-access package",
           remoteAccessUsesNativePackage(RadioPlatform::DeepEddy));
    report("BigBend -> container remote-access package",
           !remoteAccessUsesNativePackage(RadioPlatform::BigBend));
    report("DragonFire -> container remote-access package",
           !remoteAccessUsesNativePackage(RadioPlatform::DragonFire));
    // An unidentified radio stays on the container path, where the Docker
    // gate holds it at "Checking" rather than guessing the legacy package.
    report("Unknown -> container remote-access package",
           !remoteAccessUsesNativePackage(RadioPlatform::Unknown));

    // ── Radio health tools: a connected waveform radio on its own LAN ─────────
    using H = RadioHealthBlocker;
    report("health: connected waveform radio on the LAN -> None",
           radioHealthBlocker(true, true, false, true) == H::None);
    report("health: not connected wins",
           radioHealthBlocker(false, true, false, true) == H::NotConnected);
    report("health: a radio family without waveforms -> NoWaveforms",
           radioHealthBlocker(true, false, false, true) == H::NoWaveforms);
    report("health: SmartLink -> Remote, even with a private remembered address",
           radioHealthBlocker(true, true, true, true) == H::Remote);
    report("health: a non-LAN address (e.g. a tailnet) -> NotLan",
           radioHealthBlocker(true, true, false, false) == H::NotLan);

    // ── AetherGPS: legacy on a FLEX-6000, else a container under the Docker gate ──
    using G = AetherGpsBlocker;
    using D = DockerWaveformInstallBlocker;
    report("gps: a connected FLEX-6000 takes the legacy package -> None",
           aetherGpsBlocker(true, true, true, D::UnsupportedPlatform) == G::None);
    report("gps: a container radio with its WFP ready -> None",
           aetherGpsBlocker(true, true, false, D::None) == G::None);
    report("gps: a container radio whose WFP isn't ready -> Container",
           aetherGpsBlocker(true, true, false, D::WfpNotReady) == G::Container
               && aetherGpsBlocker(true, true, false, D::RuntimeStatusUnknown) == G::Container);
    report("gps: not connected wins",
           aetherGpsBlocker(false, true, false, D::NotConnected) == G::NotConnected);
    report("gps: a radio family without waveforms -> NoWaveforms",
           aetherGpsBlocker(true, false, true, D::None) == G::NoWaveforms);

    if (g_failed == 0) {
        std::printf("\nAll %d waveform-install-gate tests passed.\n", g_total);
        return 0;
    }
    std::printf("\n%d of %d waveform-install-gate tests failed.\n", g_failed, g_total);
    return 1;
}
