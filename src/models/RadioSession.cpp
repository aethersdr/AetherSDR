#include "models/RadioSession.h"

#include "core/CatPort.h"
#ifdef HAVE_WEBSOCKETS
#include "core/TciServer.h"
#endif

namespace AetherSDR {

RadioSession::RadioSession(QObject* parent)
    : QObject(parent)
{
}

RadioSession::~RadioSession()
{
    // Destruction order is the contract (#2385): every owned server holds a
    // raw RadioModel*, so they die here — in the destructor body — before
    // the m_radioModel member destructs.
#ifdef HAVE_WEBSOCKETS
    shutdownTciServer();
#endif
    for (CatPort*& port : m_catPorts) {
        delete port;
        port = nullptr;
    }
}

#ifdef HAVE_WEBSOCKETS
void RadioSession::setTciServer(TciServer* server)
{
    Q_ASSERT(!server || !server->parent());  // parent would recreate #2385
    shutdownTciServer();
    m_tciServer = server;
}

void RadioSession::shutdownTciServer()
{
    delete m_tciServer;
    m_tciServer = nullptr;
}
#endif

CatPort* RadioSession::catPort(int i) const
{
    return (i >= 0 && i < kCatPorts) ? m_catPorts[size_t(i)] : nullptr;
}

void RadioSession::setCatPort(int i, CatPort* port)
{
    if (i < 0 || i >= kCatPorts) {
        delete port;
        return;
    }
    Q_ASSERT(!port || !port->parent());
    delete m_catPorts[size_t(i)];
    m_catPorts[size_t(i)] = port;
}

} // namespace AetherSDR
