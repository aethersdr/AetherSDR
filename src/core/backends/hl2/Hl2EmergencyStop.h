#pragma once

#include <QtGlobal>   // qintptr / quint16

#include <array>
#include <cstdint>

class QHostAddress;

namespace AetherSDR::hl2 {

// Release the radio from inside a signal handler. An HL2 never told to stop
// keeps streaming EP6 at a dead host and stops answering discovery until power
// cycled; Hl2Backend's destructor does not run on a signal, and the gateware
// watchdog (MetisProtocol.h, kRunWatchdogDisable) does not recover it in
// practice. Not a self-pipe: a stuck event loop is why people reach for kill.
// arm() precomputes fd, sockaddr and the 64-byte stop datagram so fire() is
// async-signal-safe. SIGKILL cannot be caught.

// Publish the parameters needed to stop the radio. Called by MetisClient once
// its socket is bound and the destination is known. Passing an invalid fd
// disarms.
void armEmergencyStop(qintptr fd, const QHostAddress& host, quint16 port,
                      const std::array<std::uint8_t, 64>& stopPacket) noexcept;

// Forget the armed radio. Called from MetisClient::stop(), which has already
// sent the stop through the normal path.
void disarmEmergencyStop() noexcept;

// Send the stop datagram. ASYNC-SIGNAL-SAFE — calls only sendto(). A no-op
// when nothing is armed, so it is always safe to call from a handler.
//
// Sends the datagram a few times: this is UDP, the packet is 64 bytes, and the
// cost of a lost one is a radio the operator has to walk over to and unplug.
void fireEmergencyStop() noexcept;

// Install handlers for SIGTERM/SIGINT/SIGHUP/SIGQUIT that fire the stop, restore
// the default disposition and re-raise (exit status unchanged). An inherited
// SIG_IGN stays ignored; crash signals are left to crash reporting. Call once,
// early in main(); safe with no radio connected.
void installEmergencyStopSignalHandlers() noexcept;

}  // namespace AetherSDR::hl2
