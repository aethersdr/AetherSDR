#pragma once

#include "CommandParser.h"

#include <QMetaObject>
#include <QObject>

namespace AetherSDR {

class RadioConnection;
class WanConnection;

// The one entry for Flex wire lines. Each transport (the LAN RadioConnection,
// a SmartLink WanConnection, the synthetic Demo connection) emits every line
// once as lineReceived(line, epoch), tagged with the status epoch it was
// stamped with when its session began. The ingress lives on its owner's
// thread and takes each line over one queued hop, accepting it only from the
// current route and the current epoch: a line from an abandoned attempt, a
// retired session or a replaced transport never reaches the model, however
// it was queued.
class FlexStatusIngress final : public QObject {
    Q_OBJECT
public:
    explicit FlexStatusIngress(QObject* parent = nullptr);
    // Route a LAN (or synthetic) connection; its next session is stamped with
    // beginSession()'s epoch before it connects.
    void bind(RadioConnection* connection);
    // Route a SmartLink session that is already live (it binds after its
    // handshake): stamps and returns a fresh epoch.
    quint64 bindLive(WanConnection* connection);
    void unbind();
    // A fresh epoch for the session about to start; every line queued so far
    // is refused. The caller stamps the transport with it before connecting.
    quint64 beginSession();
    // Refuse the given epoch from now on, if it is still the current one.
    void retire(quint64 epoch);
    quint64 epoch() const { return m_epoch; }

signals:
    void lineAccepted(const AetherSDR::ParsedMessage& line);

private:
    template <typename Transport> void route(Transport* transport);
    QMetaObject::Connection m_connection;
    quint64 m_route{0};    // the current route's token; 0 = none
    quint64 m_routes{0};
    quint64 m_epoch{0};
};

} // namespace AetherSDR
