#pragma once

#include <QHostAddress>
#include <QMetaType>
#include <QtGlobal>

namespace AetherSDR {

// Immutable owner-thread view of one established command transport. Workers
// publish it at the connection edge; readers never borrow socket state.
struct ConnectionSession {
    quint64 epoch{0};
    quint32 clientHandle{0};
    QHostAddress remoteAddress;
    QHostAddress localAddress;
    quint16 localPort{0};
    enum class Transport { Lan, Wan };
    Transport transport{Transport::Lan};
};

struct RadioDisconnectRequest {
    enum class Completion { Wait, Deferred };
    // Only the receive stream owned by this session may be removed on exit.
    quint32 receiveStreamId{0};
    // Deferred retires the session now; bounded transport cleanup runs on its
    // worker. Intentional disconnect and destruction still wait for completion.
    Completion completion{Completion::Wait};
};

struct ReceiveTransportResult {
    bool ready{false};
    quint16 localPort{0};
    enum class Transport { Lan, Wan };
    Transport transport{Transport::Lan};
};

} // namespace AetherSDR

Q_DECLARE_METATYPE(AetherSDR::ConnectionSession)
