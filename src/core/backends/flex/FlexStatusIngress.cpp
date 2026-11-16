#include "FlexStatusIngress.h"

#include "RadioConnection.h"
#include "WanConnection.h"

#include <QPointer>

namespace AetherSDR {

FlexStatusIngress::FlexStatusIngress(QObject* parent)
    : QObject(parent)
{
}

template <typename Transport>
void FlexStatusIngress::route(Transport* transport)
{
    unbind();
    if (!transport) { return; }
    const quint64 token = ++m_routes;
    m_route = token;
    m_connection = connect(transport, &Transport::lineReceived, this,
        [this, token](const ParsedMessage& line, quint64 epoch) {
            if (token != m_route || epoch != m_epoch) { return; }
            // A decoded delta can end the session or the backend that owns us.
            const QPointer<FlexStatusIngress> alive(this);
            decodeLine(line);
            if (alive && token == m_route && epoch == m_epoch) { emit lineAccepted(line); }
        }, Qt::QueuedConnection);
}

void FlexStatusIngress::bind(RadioConnection* connection)
{
    route(connection);
}

quint64 FlexStatusIngress::bindLive(WanConnection* connection)
{
    route(connection);
    if (connection) { connection->setLineEpoch(beginSession()); }
    return m_epoch;
}

void FlexStatusIngress::unbind()
{
    QObject::disconnect(m_connection);
    m_connection = {};
    m_route = 0;   // lines already posted through the old route are refused
}

quint64 FlexStatusIngress::beginSession()
{
    return ++m_epoch;
}

void FlexStatusIngress::retire(quint64 epoch)
{
    if (epoch == m_epoch) { ++m_epoch; }
}

} // namespace AetherSDR
