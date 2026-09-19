#include "models/RadioSession.h"

#include "core/CatPort.h"
#ifdef HAVE_WEBSOCKETS
#include "core/TciServer.h"
#endif

#include <QCoreApplication>
#include <QMetaObject>
#include <QThread>

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
    if (!m_tciServer) {
        return;
    }
    // Dedicated I/O thread so WSJT-X audio/chrono are not pinned to the GUI
    // nested loop that macOS/Windows run while the operator drags the window.
    // QWebSocketServer is created later in TciServer::start() on this thread
    // (Windows QSocketNotifier affinity — see MainWindow_Spots.cpp #1929).
    m_tciThread = new QThread;
    m_tciThread->setObjectName(QStringLiteral("TciServer"));
    m_tciServer->moveToThread(m_tciThread);
    m_tciThread->start();
}

void RadioSession::shutdownTciServer()
{
    if (!m_tciServer) {
        delete m_tciThread;
        m_tciThread = nullptr;
        return;
    }

    if (m_tciThread && m_tciThread->isRunning()
        && m_tciServer->thread() == m_tciThread) {
        // stop() hops I/O onto the TCI thread and runs PTT abort / DAX
        // release here, on RadioModel's thread. Do not invoke the whole
        // stop() on the TCI thread — that BlockingQueued-hops abort back
        // to this thread and deadlocks.
        m_tciServer->stop();
        QMetaObject::invokeMethod(m_tciServer, [server = m_tciServer]() {
            if (QCoreApplication::instance()) {
                server->moveToThread(QCoreApplication::instance()->thread());
            }
        }, Qt::BlockingQueuedConnection);
        m_tciThread->quit();
        m_tciThread->wait(3000);
    } else {
        m_tciServer->stop();
    }

    delete m_tciServer;
    m_tciServer = nullptr;
    delete m_tciThread;
    m_tciThread = nullptr;
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
