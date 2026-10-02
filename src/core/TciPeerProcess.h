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

// Resolve the local process owning the TCP connection whose CLIENT-side
// endpoint is peerAddr:peerPort. Returns unresolved for non-loopback peers or
// any failure; it's log decoration, never a gate. Blocking (per-process
// descriptor sweep): call off the GUI thread. Linux/macOS check the owner uid
// matches ours; on Windows only OpenProcess() rights limit it, so an elevated
// instance can name another session's client.
TciPeerProcessInfo resolveLoopbackPeerProcess(const QHostAddress& peerAddr,
                                             quint16 peerPort);

} // namespace AetherSDR
