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
};

struct RadioDisconnectRequest {
    // Only the receive stream owned by this session may be removed on exit.
    quint32 receiveStreamId{0};
};

struct ReceiveTransportResult {
    bool ready{false};
    quint16 localPort{0};
};

} // namespace AetherSDR

Q_DECLARE_METATYPE(AetherSDR::ConnectionSession)
