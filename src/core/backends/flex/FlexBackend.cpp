#include "core/backends/flex/FlexBackend.h"

#include <QThread>

#include "core/RadioConnection.h"
#include "core/PanadapterStream.h"
#include "models/ModelCapabilities.h"

namespace AetherSDR {

FlexBackend::FlexBackend(QObject* parent)
    : IRadioBackend(parent)
{
    // Own the wire objects + their worker threads. Order is load-bearing and
    // preserved verbatim from the former RadioModel ctor (#502): PanadapterStream
    // thread FIRST, then RadioConnection thread. Both objects are parentless and
    // moved onto their thread; the thread is this-parented.
    m_networkThread = new QThread(this);
    m_networkThread->setObjectName("PanadapterStream");
    m_panStream = new PanadapterStream;   // no parent — moved to thread
    m_panStream->moveToThread(m_networkThread);
    connect(m_networkThread, &QThread::started, m_panStream, &PanadapterStream::init);
    m_networkThread->start();

    m_connThread = new QThread(this);
    m_connThread->setObjectName("RadioConnection");
    m_connection = new RadioConnection;   // no parent — moved to thread
    m_connection->moveToThread(m_connThread);
    connect(m_connThread, &QThread::started, m_connection, &RadioConnection::init);
    m_connThread->start();

    // Observe wire lifecycle and re-emit as the interface's own signals. Queued
    // (auto) connections: the connection lives on its worker thread.
    connect(m_connection, &RadioConnection::connected,
            this, &IRadioBackend::connected);
    connect(m_connection, &RadioConnection::disconnected,
            this, &IRadioBackend::disconnected);
    connect(m_connection, &RadioConnection::errorOccurred,
            this, &IRadioBackend::connectionError);
}

FlexBackend::~FlexBackend()
{
    // Sever our own lifecycle observation of the connection FIRST — as the old
    // ~RadioModel's earlier m_backend.reset() effectively did (the backend was
    // destroyed, auto-disconnecting these links, before the wire teardown ran).
    // Otherwise disconnectFromRadio below could re-emit connected/disconnected
    // through this half-destroyed backend. (#4058 review)
    if (m_connection) {
        disconnect(m_connection, nullptr, this, nullptr);
    }

    // Teardown in the exact #502 order the former RadioModel dtor used:
    // connection first (BlockingQueued disconnect → deleteLater → thread
    // quit/wait), then panStream (BlockingQueued stop → …).
    if (m_connection && m_connThread && m_connThread->isRunning()) {
        RadioConnection* connection = m_connection;
        QMetaObject::invokeMethod(connection, &RadioConnection::disconnectFromRadio,
                                  Qt::BlockingQueuedConnection);
        connection->deleteLater();
        m_connThread->quit();
        m_connThread->wait(3000);
    } else {
        delete m_connection;
    }
    if (m_connThread && m_connThread->isRunning()) {
        m_connThread->quit();
        m_connThread->wait(3000);
    }
    m_connection = nullptr;

    if (m_panStream && m_networkThread && m_networkThread->isRunning()) {
        PanadapterStream* panStream = m_panStream;
        QMetaObject::invokeMethod(panStream, &PanadapterStream::stop,
                                  Qt::BlockingQueuedConnection);
        panStream->deleteLater();
        m_networkThread->quit();
        m_networkThread->wait(3000);
    } else {
        delete m_panStream;
    }
    if (m_networkThread && m_networkThread->isRunning()) {
        m_networkThread->quit();
        m_networkThread->wait(3000);
    }
    m_panStream = nullptr;
}

void FlexBackend::setCommandSink(std::function<void(const QString&)> sink)
{
    m_sink = std::move(sink);
}

void FlexBackend::setModelProvider(std::function<QString()> provider)
{
    m_modelProvider = std::move(provider);
}

RadioCapabilities FlexBackend::capabilities() const
{
    RadioCapabilities caps;
    caps.family = QStringLiteral("flex");
    caps.model = m_modelProvider ? m_modelProvider() : QString();

    // Seed from the FlexLib-sourced platform table (Principle I). This is the
    // derived-from-name truth used to *seed* the reported capabilities; a fuller
    // FlexBackend refines these from live radio status as touchpoints convert.
    const ModelCapabilities mc = capabilitiesFor(caps.model);
    caps.maxSlices = mc.maxSlices;
    // approx: pan capacity is not strictly slice count on real Flex hardware;
    // refined from live radio status in a later touchpoint conversion.
    caps.maxPanadapters = mc.maxSlices;
    caps.hasExtendedDsp = mc.hasExtendedDsp();

    // Every current FlexRadio transmits; RX-only WAN/observer nuance is layered
    // in later. Sample rates and TX power range are refined as their touchpoints
    // convert (they are not part of this skeleton).
    caps.canTransmit = true;
    caps.hasTuner = true;

    // Advertise NO extension namespaces yet: no flex verb is routed through the
    // seam, and invokeExtension() can't produce a reply. Advertising "flex"
    // would let a client pre-check the namespace and then hang awaiting an
    // extensionResult/Error that never comes. "flex" is declared here when the
    // first amp/tuner/DAX verb converts.
    return caps;
}

void FlexBackend::connectRadio(const RadioConnectRequest& /*request*/)
{
    // RadioModel still orchestrates connect (RadioInfo assembly, WAN/SmartLink
    // duality, auto-reconnect); the backend owns the objects but not yet the
    // connect flow — that adaptation moves behind the seam in a later increment.
}

void FlexBackend::disconnectRadio()
{
    // RadioModel still orchestrates the staged gracefulDisconnect
    // (handle/streamId/seq). Owned by the backend later.
}

bool FlexBackend::isConnected() const
{
    return m_connection && m_connection->isConnected();
}

void FlexBackend::setSliceFrequency(int sliceId, double hz)
{
    // Matches SliceModel::setFrequency's wire string exactly.
    send(QStringLiteral("slice tune %1 %2 autopan=0")
             .arg(sliceId)
             .arg(hz / 1'000'000.0, 0, 'f', 6));
}

void FlexBackend::setSliceMode(int sliceId, const QString& mode)
{
    send(QStringLiteral("slice set %1 mode=%2").arg(sliceId).arg(mode));
}

void FlexBackend::setSliceFilter(int sliceId, int lowHz, int highHz)
{
    send(QStringLiteral("filt %1 %2 %3").arg(sliceId).arg(lowHz).arg(highHz));
}

void FlexBackend::setKeying(bool key)
{
    // Keying is only translated here; the interlock/authorization decision is
    // made above the seam (RFC §6). Matches RadioModel::setTransmit's wire form.
    send(QStringLiteral("xmit %1").arg(key ? 1 : 0));
}

void FlexBackend::invokeExtension(const QString& /*ns*/, const QString& /*verb*/,
                                  quint64 requestId, const QVariant& /*arg*/)
{
    // No flex extension verbs are routed through the seam yet. Honor the async
    // contract by construction: a caller awaiting a reply (requestId != 0) gets
    // an error, never a hang. Real verbs land with the amp/tuner/DAX touchpoint
    // conversions.
    if (requestId != 0) {
        emit extensionError(requestId,
                            QStringLiteral("flex: no extension verbs implemented"));
    }
}

void FlexBackend::send(const QString& cmd)
{
    if (m_sink) {
        m_sink(cmd);
    }
}

}  // namespace AetherSDR
