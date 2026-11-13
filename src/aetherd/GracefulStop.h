#pragma once

#include <chrono>

class QCoreApplication;

namespace AetherSDR::aetherd {

// A stop request ends aetherd the way quitting does, so teardown releases the
// radio: ~ModelRadioConnectionTarget calls RadioModel::disconnectFromRadio(),
// which stops the radio's stream. Killed by a signal instead, aetherd leaves the
// radio running for a host that is gone: an ANAN reports itself busy for ~8 s,
// and an HL2 streams at the dead host until power-cycled (Hl2EmergencyStop.h).
//
// POSIX: the first SIGTERM, SIGINT or SIGHUP wakes the event loop through a
// self-pipe and the loop quits. A second one, or the grace period running out
// (a stuck loop is why people reach for kill), fires the HL2 emergency stop and
// dies by the signal exactly as before. SIGQUIT keeps the GUI's behavior (the
// HL2 emergency stop, then the default action). An inherited SIG_IGN stays
// ignored. Windows: a console control event queues the quit; for close, logoff
// and shutdown, which end the process when the handler returns, the handler
// waits for teardown, bounded by the same grace period.
//
// Construct right after the QCoreApplication, before anything that owns a
// radio: the destructor, which then runs last, marks teardown complete.
class GracefulStop {
public:
    GracefulStop(QCoreApplication& app, std::chrono::seconds grace);
    ~GracefulStop();
    GracefulStop(const GracefulStop&) = delete;
    GracefulStop& operator=(const GracefulStop&) = delete;
};

} // namespace AetherSDR::aetherd
