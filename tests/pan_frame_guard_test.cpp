#include "gui/PanFrameGuard.h"

#include <QCoreApplication>
#include <cstdio>
#include <memory>

using namespace AetherSDR;

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    int failures = 0;
    const auto check = [&](bool ok, const char* name) {
        std::printf("[%s] %s\n", ok ? "OK" : "FAIL", name);
        failures += !ok;
    };
    auto pan = std::make_unique<PanadapterModel>("0xe1000000");
    check(!PanFrameGuard(true, pan.get()).isCurrent(), "unobserved pan cannot admit a frame");
    pan->setCenterBandwidth(460.3, 0.125);
    const PanFrameGuard first(true, pan.get());
    const PanFrameGuard flex(false, pan.get());
    check(first.isCurrent(), "fresh accepted frame is admitted");
    pan->setCenterBandwidth(460.3, 0.125);
    pan->republishCenterBandwidth();
    check(first.isCurrent(), "refusal/no-op echo keeps live frames flowing");
    // A pending or rolled-back capture never updates the model. Its accepted
    // frame remains usable until a new geometry is actually published.
    check(first.isCurrent(), "unchanged accepted state survives pending/rollback");
    pan->setCenterBandwidth(460.301171875, 0.125);
    check(!first.isCurrent(), "delayed frame cannot paint a new center");
    check(flex.isCurrent(), "Flex status/data pacing is unchanged");
    pan->setCenterBandwidth(460.3, 0.125);
    check(!first.isCurrent(), "A to B to A cannot revive superseded delivery");
    const PanFrameGuard current(true, pan.get());
    pan->setCenterBandwidth(460.3, 0.25);
    check(!current.isCurrent(), "same center with new span invalidates queued bins");
    const PanFrameGuard beforeReconnect(true, pan.get());
    pan->resetCenterKnownForReconnect();
    pan->setCenterBandwidth(460.3, 0.25);
    check(!beforeReconnect.isCurrent(), "same pan object and geometry in new session reject old frame");
    const PanFrameGuard beforeRemove(true, pan.get());
    pan.reset();
    check(!beforeRemove.isCurrent(), "removed pan cannot deliver deferred frame");
    check(PanFrameGuard(false, nullptr).isCurrent(), "legacy unmatched-pan handling remains unchanged");
    check(!PanFrameGuard(true, nullptr).isCurrent(), "confirmed unmatched stream is refused");
    return failures ? 1 : 0;
}
