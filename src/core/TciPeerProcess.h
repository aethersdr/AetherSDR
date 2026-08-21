#pragma once

#include <QHostAddress>
#include <QString>

namespace AetherSDR {

// Best-effort identity of the LOCAL program behind a TCP connection to us.
// TCI carries no client-identification message and the WebSocket handshake
// is a bare upgrade, so for a same-machine client the OS socket→pid map is
// the only source (#5087).  Remote peers are never resolved.
struct TciPeerProcessInfo {
    bool    resolved{false};
    QString name;       // "wsjtx"
    QString exePath;    // "/usr/bin/wsjtx"
    QString version;    // best-effort; empty when unknown — never guessed
};

// Resolve the local process that owns the TCP connection whose endpoint, as
// seen from our side, is peerAddr:peerPort (i.e. the CLIENT's local address
// and port).  Returns an unresolved struct for a non-loopback peer or on any
// failure — this is decoration for a diagnostic log line, never a gate.
// Blocking and potentially slow (a per-process descriptor sweep): call it
// off the GUI thread.
TciPeerProcessInfo resolveLoopbackPeerProcess(const QHostAddress& peerAddr,
                                             quint16 peerPort);

} // namespace AetherSDR
