#include "core/backends/sim/SimBackend.h"

namespace AetherSDR {

SimBackend::SimBackend(QObject* parent) : IRadioBackend(parent) {}

SimBackend::~SimBackend() = default;

QString SimBackend::demoModelName() { return QStringLiteral("AetherSDR Demo"); }
QString SimBackend::demoSerial()    { return QStringLiteral("DEMO-0001"); }

RadioCapabilities SimBackend::capabilities() const
{
    RadioCapabilities caps;
    caps.family = QStringLiteral("sim");
    caps.model  = demoModelName();
    caps.maxSlices = 1;          // Phase 1: a single slice. Phase 2 raises this.
    caps.maxPanadapters = 1;
    caps.sampleRatesHz = {};     // no data plane yet
    // A simulator must never look like something that can key a transmitter
    // (Principle VI). TX stays off in the skeleton.
    caps.canTransmit = false;
    caps.txPowerMaxWatts = 0.0;
    caps.hasTuner = false;
    caps.hasAmplifier = false;
    caps.hasExtendedDsp = false;
    return caps;
}

void SimBackend::connectRadio(const RadioConnectRequest& /*request*/)
{
    if (m_connected) {
        return;   // idempotent — a second connect is a no-op, not a reset
    }
    m_connected = true;
    emit connected();
    emit capabilitiesChanged();
    emitInitialState();
}

void SimBackend::disconnectRadio()
{
    if (!m_connected) {
        return;
    }
    m_connected = false;
    emit sliceRemoved(kSliceId);
    emit disconnected();
}

bool SimBackend::isConnected() const { return m_connected; }

void SimBackend::emitInitialState()
{
    // Radio-global snapshot: identity + free-slot count. Present-only fields.
    RadioDelta radio;
    radio.model = demoModelName();
    radio.nickname = QStringLiteral("Demo Simulator");
    radio.slicesAvailable = capabilities().maxSlices;
    emit radioChanged(radio);

    // One active slice on a sensible default, so the UI has something live to
    // show the moment demo mode connects.
    SliceDelta slice;
    slice.letter = QStringLiteral("A");
    slice.frequency = m_sliceFreqMhz;
    slice.mode = m_sliceMode;
    slice.filterLow = m_filterLowHz;
    slice.filterHigh = m_filterHighHz;
    slice.active = true;
    slice.inUse = true;
    emit sliceChanged(kSliceId, slice);
}

void SimBackend::setSliceFrequency(int sliceId, double hz)
{
    if (!m_connected || sliceId != kSliceId) {
        return;
    }
    m_sliceFreqMhz = hz / 1.0e6;
    SliceDelta d;
    d.frequency = m_sliceFreqMhz;
    emit sliceChanged(kSliceId, d);   // radio is authoritative: echo the change back (Principle II)
}

void SimBackend::setSliceMode(int sliceId, const QString& mode)
{
    if (!m_connected || sliceId != kSliceId) {
        return;
    }
    m_sliceMode = mode;
    SliceDelta d;
    d.mode = m_sliceMode;
    emit sliceChanged(kSliceId, d);
}

void SimBackend::setSliceFilter(int sliceId, int lowHz, int highHz)
{
    if (!m_connected || sliceId != kSliceId) {
        return;
    }
    m_filterLowHz = lowHz;
    m_filterHighHz = highHz;
    SliceDelta d;
    d.filterLow = m_filterLowHz;
    d.filterHigh = m_filterHighHz;
    emit sliceChanged(kSliceId, d);
}

void SimBackend::setKeying(bool /*key*/)
{
    // RX-only in the skeleton (capabilities().canTransmit == false). The engine
    // TX guard above the seam already denies keying, but a no-op here makes the
    // fail-closed behavior explicit at the backend too (Principle VI).
}

void SimBackend::invokeExtension(const QString& /*ns*/, const QString& /*verb*/,
                                 quint64 requestId, const QVariant& /*arg*/)
{
    // No vendor extensions in the skeleton. A requestId of 0 means "no reply
    // expected"; anything else gets an explicit error so a caller correlating a
    // reply is never left waiting.
    if (requestId != 0) {
        emit extensionError(requestId,
                            QStringLiteral("sim backend has no extensions"));
    }
}

}  // namespace AetherSDR
