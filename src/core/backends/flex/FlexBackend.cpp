#include "core/backends/flex/FlexBackend.h"

#include "core/RadioConnection.h"
#include "models/ModelCapabilities.h"

namespace AetherSDR {

FlexBackend::FlexBackend(QObject* parent)
    : IRadioBackend(parent)
{
}

FlexBackend::~FlexBackend() = default;

void FlexBackend::attachConnection(RadioConnection* conn)
{
    if (m_connection == conn) {
        return;
    }
    if (m_connection) {
        disconnect(m_connection, nullptr, this, nullptr);
    }
    m_connection = conn;
    if (!m_connection) {
        return;
    }
    // Observe wire lifecycle and re-emit as the interface's own signals. Queued
    // (auto) connections: the connection lives on its worker thread.
    connect(m_connection, &RadioConnection::connected,
            this, &IRadioBackend::connected);
    connect(m_connection, &RadioConnection::disconnected,
            this, &IRadioBackend::disconnected);
    connect(m_connection, &RadioConnection::errorOccurred,
            this, &IRadioBackend::connectionError);
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
    caps.maxPanadapters = mc.maxSlices;   // pan capacity tracks slice capacity
    caps.hasExtendedDsp = mc.hasExtendedDsp();

    // Every current FlexRadio transmits; RX-only WAN/observer nuance is layered
    // in later. Sample rates and TX power range are refined as their touchpoints
    // convert (they are not part of this skeleton).
    caps.canTransmit = true;
    caps.hasTuner = true;

    caps.extensionNamespaces = { QStringLiteral("flex") };
    return caps;
}

void FlexBackend::connectRadio(const RadioConnectRequest& /*request*/)
{
    // 2.2 skeleton: RadioModel still orchestrates connect (RadioInfo assembly,
    // WAN/SmartLink duality, auto-reconnect). The backend will own this in a
    // later increment once the RadioConnectRequest→RadioInfo adaptation and the
    // WAN branch move behind the seam.
}

void FlexBackend::disconnectRadio()
{
    // 2.2 skeleton: RadioModel still orchestrates the staged gracefulDisconnect
    // (handle/streamId/seq) and teardown ordering. Owned by the backend later.
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
                                  quint64 /*requestId*/, const QVariant& /*arg*/)
{
    // No flex extension verbs are routed through the seam yet; they land with
    // the amp/tuner/DAX touchpoint conversions. A real reply would arrive via
    // extensionResult()/extensionError() keyed by requestId.
}

void FlexBackend::send(const QString& cmd)
{
    if (m_sink) {
        m_sink(cmd);
    }
}

}  // namespace AetherSDR
