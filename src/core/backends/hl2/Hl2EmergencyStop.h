#pragma once

#include <QtGlobal>   // qintptr / quint16

#include <array>
#include <cstdint>

class QHostAddress;

namespace AetherSDR::hl2 {

// Release the radio from inside a signal handler.
//
// WHY THIS EXISTS. The Hermes-Lite 2 must be told to stop streaming. When it is
// not, it keeps sending EP6 at a host that is gone and then stops answering
// discovery — alive at the network layer, invisible to every client, and
// requiring a physical power cycle. Reproduced three times: `kill <pid>` on a
// connected AetherSDR wedges the radio every time.
//
// Hl2Backend's destructor already sends the stop, and that covers a normal
// quit. It does not run on a signal: Qt tears nothing down for SIGTERM, so the
// process simply vanishes mid-stream. The gateware watchdog is documented as
// the anti-wedge mechanism for exactly this case (MetisProtocol.h,
// kRunWatchdogDisable) and did NOT recover it in practice — which is why this
// belt exists alongside that brace rather than instead of it.
//
// WHY IT IS NOT A SELF-PIPE. The textbook Qt answer is to write to a pipe from
// the handler and do the real work back on the event loop. That would be
// useless here: an unresponsive event loop is one of the main reasons anyone
// reaches for kill in the first place, so the fix must not depend on the thing
// that may already be stuck. fire() therefore does the whole job in the
// handler, using nothing but sendto() on an address resolved in advance.
//
// EVERYTHING IS PRE-COMPUTED so that fire() is async-signal-safe: the socket
// descriptor, the destination sockaddr and the 64-byte stop datagram are all
// built by arm(), on a normal thread, while the link is coming up.
//
// SIGKILL cannot be caught, so `kill -9` still wedges the radio. Nothing in a
// process can change that.

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

// Install handlers for the TERMINATING signals (SIGTERM/SIGINT/SIGHUP/SIGQUIT)
// so fireEmergencyStop() runs before the process dies. Each handler restores
// the default disposition and re-raises, so the exit status is unchanged.
//
// A signal already inherited as SIG_IGN is left ignored — see the
// implementation for why that matters more than it looks.
//
// Crash signals are deliberately NOT taken over; they belong to the platform's
// crash reporting.
//
// Call once, early in main(). Safe to call when no radio is connected.
void installEmergencyStopSignalHandlers() noexcept;

}  // namespace AetherSDR::hl2
