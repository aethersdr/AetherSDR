#include "Hl2IqProvider.h"

#include "MetisProtocol.h"
#include "core/LogManager.h"

#include <algorithm>

namespace AetherSDR {

using AetherSDR::hl2::kMaxReceivers;
using AetherSDR::hl2::maxReceiversAtRate;

namespace {
// The DDC rates the HL2 actually runs at. 24 kHz is deliberately absent: the
// hardware has no such rate, and inventing one here by resampling would make
// the provider report an acquisition rate the radio never produced. Rate
// conversion, if it ever proves necessary for SDC, belongs ABOVE this seam.
const QVector<int> kHl2RatesHz{48000, 96000, 192000, 384000};
}  // namespace

Hl2IqProvider::Hl2IqProvider(QObject* parent)
    : IqProvider(parent)
{
}

IqProviderCaps Hl2IqProvider::capabilities() const
{
    std::lock_guard<std::mutex> g(m_routeMutex);
    IqProviderCaps caps;
    // Report what the LINK will actually carry at the rate we are running,
    // bounded by what the gateware reports. maxReceiversAtRate() is the
    // existing budget the backend already caps receiver count with — reused,
    // not reimplemented, so the two can never disagree.
    caps.maxConcurrentEndpoints =
        std::min(m_boardReceiverLimit,
                 maxReceiversAtRate(m_sampleRateHz, kMaxReceivers));
    caps.supportedRatesHz = kHl2RatesHz;
    caps.rateTopology = IqRateTopology::SharedAcrossReceivers;
    caps.rateChangeIsRadioWide = true;
    return caps;
}

QVector<IqEndpoint> Hl2IqProvider::endpoints() const
{
    std::lock_guard<std::mutex> g(m_routeMutex);
    QVector<IqEndpoint> out;
    out.reserve(m_routes.size());
    for (int i = 0; i < m_routes.size(); ++i) {
        IqEndpoint e;
        e.id = m_routes[i].endpointId;
        e.label = QStringLiteral("DDC %1").arg(i);
        e.live = m_connected;
        e.achievedRateHz = m_sampleRateHz;
        // The DDC's NCO, which is the pan centre — NOT the slice's tuned
        // frequency. The two are independent on this backend by design (the
        // slice is shifted inside the passband and the NCO moves only when the
        // target would leave the window), so reporting the slice here would
        // mis-map every skimmer spot by the shift.
        e.centerHz = m_routes[i].centerHz;
        out.append(e);
    }
    return out;
}

void Hl2IqProvider::publishRoutes(const QVector<Route>& routes, int sampleRateHz,
                                  int boardReceiverLimit)
{
    QVector<QString> vanished;
    {
        std::lock_guard<std::mutex> g(m_routeMutex);
        for (const Route& old : m_routes) {
            bool stillThere = false;
            for (const Route& r : routes) {
                if (r.endpointId == old.endpointId) { stillThere = true; break; }
            }
            if (!stillThere)
                vanished.append(old.endpointId);
        }
        m_routes = routes;
        if (sampleRateHz > 0)
            m_sampleRateHz = sampleRateHz;
        if (boardReceiverLimit > 0)
            m_boardReceiverLimit = boardReceiverLimit;
    }
    // A receiver that went away invalidates its lease. It is NOT rebound to
    // whichever DDC now occupies its wire index — the wire indices are
    // contiguous and renumber on every rebuild, which is precisely how a
    // subscription would otherwise slide onto another band unnoticed.
    for (const QString& id : vanished)
        invalidateEndpoint(id, IqRefusal::UnknownEndpoint);

    // Snapshot first, then update OUTSIDE the route lock: noteAchieved() emits
    // endpointsChanged(), and a slot on that calls endpoints(), which takes the
    // same lock.
    QVector<Route> snapshot;
    int rate = 0;
    {
        std::lock_guard<std::mutex> g(m_routeMutex);
        snapshot = m_routes;
        rate = m_sampleRateHz;
    }
    for (const Route& r : snapshot)
        noteAchieved(r.endpointId, rate, r.centerHz);
    emit endpointsChanged();
}

void Hl2IqProvider::setConnected(bool on)
{
    {
        std::lock_guard<std::mutex> g(m_routeMutex);
        if (m_connected == on)
            return;
        m_connected = on;
    }
    if (!on)
        invalidateAllEndpoints(IqRefusal::NotConnected);
    emit endpointsChanged();
}

IqLease Hl2IqProvider::openEndpoint(const QString& endpointId, int requestedRateHz)
{
    int rate = 0;
    long long center = 0;
    bool found = false;
    int endpointCount = 0;
    {
        std::lock_guard<std::mutex> g(m_routeMutex);
        if (!m_connected)
            return IqLease::refuse(IqRefusal::NotConnected);
        rate = m_sampleRateHz;
        endpointCount = m_routes.size();
        for (const Route& r : m_routes) {
            if (r.endpointId == endpointId) {
                center = r.centerHz;
                found = true;
                break;
            }
        }
    }
    if (!found)
        return IqLease::refuse(IqRefusal::UnknownEndpoint);

    // The link budget, reported rather than overrun. Four receivers fit through
    // 192 kHz; at 384 kHz only three do, and the fourth would not fail cleanly —
    // it would drop EP6 packets, which is a simultaneous gap in every receiver.
    const int admitted = maxReceiversAtRate(rate, kMaxReceivers);
    if (endpointCount > admitted) {
        return IqLease::refuse(
            IqRefusal::BackendRefused,
            QStringLiteral("EP6 link budget admits %1 receivers at %2 kHz, %3 open")
                .arg(admitted).arg(rate / 1000).arg(endpointCount));
    }

    // No radio-side lifecycle to start: the DDC is already running, because the
    // application's own panadapter and DSP chain are using it. Attaching a tap
    // is the whole of "open" here, and detaching it is the whole of "close" —
    // which is what stops a TCI stop from tearing down a receiver the operator
    // is still looking at.
    IqLease l;
    l.granted = true;
    l.refusal = IqRefusal::None;
    l.endpointId = endpointId;
    l.achievedRateHz = rate;   // requested vs achieved: see adjustRate()
    l.centerHz = center;
    if (requestedRateHz > 0 && requestedRateHz != rate) {
        l.detail = QStringLiteral("requested %1 Hz; radio-wide DDC rate is %2 Hz")
                       .arg(requestedRateHz).arg(rate);
        qCInfo(lcHl2) << "Hl2IqProvider:" << l.detail;
    }
    return l;
}

void Hl2IqProvider::closeEndpoint(const QString& endpointId)
{
    // Detach only. The receiver and its panadapter keep running.
    qCInfo(lcHl2) << "Hl2IqProvider: detached IQ tap from" << endpointId;
}

IqLease Hl2IqProvider::adjustRate(const QString& endpointId, int requestedRateHz)
{
    // ONE DDC rate for the whole radio: honouring a client's request would
    // re-span every panadapter and rebuild every receive DSP chain, on a radio
    // the operator is using, because a skimmer asked. So the current rate is
    // reported and nothing is changed. If interoperability ever proves SDC
    // needs its exact rate, the place to add conversion is a provider-neutral
    // resampler ABOVE this — never by making acquisition dishonest.
    if (requestedRateHz > 0) {
        std::lock_guard<std::mutex> g(m_routeMutex);
        if (requestedRateHz != m_sampleRateHz) {
            qCInfo(lcHl2) << "Hl2IqProvider: keeping radio-wide DDC rate"
                          << m_sampleRateHz << "Hz; client asked for"
                          << requestedRateHz;
        }
    }
    return IqProvider::adjustRate(endpointId, requestedRateHz);
}

void Hl2IqProvider::feedWireBlock(int ddcIndex,
                                  const std::vector<std::complex<float>>& wireIq)
{
    if (ddcIndex < 0 || wireIq.empty())
        return;

    QString endpointId;
    int rate = 0;
    long long center = 0;
    {
        std::lock_guard<std::mutex> g(m_routeMutex);
        if (ddcIndex >= m_routes.size())
            return;
        endpointId = m_routes[ddcIndex].endpointId;
        center = m_routes[ddcIndex].centerHz;
        rate = m_sampleRateHz;
    }
    if (endpointId.isEmpty() || !isLeased(endpointId))
        return;   // nobody subscribed: the sample path costs a hash lookup

    // CONJUGATE. This is the entire handedness fix and it must happen exactly
    // once, here at the transport boundary.
    //
    // HPSDR wire IQ is the conjugate of the analytic convention: a signal ABOVE
    // the NCO arrives at a NEGATIVE frequency. Hl2RxDsp already conjugates for
    // the spectrum path for the same reason — fed the raw wire it drew 40 m
    // mirrored, putting FT8 at 7.071 instead of 7.074. A skimmer fed raw wire
    // would report every spot mirrored about the DDC centre in exactly the same
    // way, and the buffer MetisClient hands us is reused, so this copy is
    // mandatory regardless.
    std::vector<std::complex<float>> canonical(wireIq.size());
    for (std::size_t n = 0; n < wireIq.size(); ++n)
        canonical[n] = std::conj(wireIq[n]);

    publishBlock(endpointId, rate, center, std::move(canonical));
}

}  // namespace AetherSDR
