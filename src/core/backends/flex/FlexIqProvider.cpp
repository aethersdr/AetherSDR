#include "core/backends/flex/FlexIqProvider.h"

#include <QtEndian>

#include <cstring>
#include <utility>

#include "core/LogManager.h"
#include "models/DaxIqModel.h"

namespace AetherSDR {

namespace {
// What a Flex DAX-IQ stream can actually be set to (FlexLib: daxiq_rate).
const QVector<int> kFlexRatesHz{24000, 48000, 96000, 192000};

bool rateSupported(int hz)
{
    return kFlexRatesHz.contains(hz);
}
}  // namespace

FlexIqProvider::FlexIqProvider(QObject* parent) : IqProvider(parent) {}

void FlexIqProvider::setPanResolvers(PanLister lister, PanCenterHzOfPan center)
{
    m_panLister = std::move(lister);
    m_panCenter = std::move(center);
}

void FlexIqProvider::setConnected(bool on)
{
    if (m_connected == on)
        return;
    m_connected = on;
    if (!on)
        handleDisconnect();
    emit endpointsChanged();
}

IqProviderCaps FlexIqProvider::capabilities() const
{
    IqProviderCaps caps;
    caps.maxConcurrentEndpoints = kNumChannels;
    caps.supportedRatesHz       = kFlexRatesHz;
    // A Flex sets daxiq_rate per stream, and moving one changes nothing else on
    // the radio — which is why a TCI client may have its requested rate here and
    // may not on a backend with one shared DDC rate.
    caps.rateTopology           = IqRateTopology::PerStream;
    caps.rateChangeIsRadioWide  = false;
    return caps;
}

long long FlexIqProvider::centerFor(const QString& panId) const
{
    return m_panCenter ? m_panCenter(panId) : 0;
}

int FlexIqProvider::currentRateFor(int channel) const
{
    if (!m_dax || channel < 1 || channel > kNumChannels)
        return 48000;
    return m_dax->stream(channel).sampleRate;
}

QVector<IqEndpoint> FlexIqProvider::endpoints() const
{
    QVector<IqEndpoint> out;
    if (!m_panLister)
        return out;
    const QStringList pans = m_panLister();
    out.reserve(pans.size());
    for (const QString& panId : pans) {
        IqEndpoint e;
        e.id    = panId;
        e.label = QStringLiteral("Panadapter %1").arg(panId);
        e.live  = m_connected;
        const auto b = m_bindings.constFind(panId);
        e.achievedRateHz = (b != m_bindings.constEnd()) ? currentRateFor(b->channel)
                                                        : 48000;
        // The PAN centre, not the slice frequency: on a Flex the DAX-IQ stream
        // is a panadapter stream (FlexLib: DAXIQChannel is a Panadapter
        // property), so a skimmer told the slice frequency mis-maps every spot
        // by (slice − panCentre) Hz. (#3910/#3913/#4172)
        e.centerHz = centerFor(panId);
        out.append(e);
    }
    return out;
}

IqLease FlexIqProvider::openEndpoint(const QString& endpointId, int requestedRateHz)
{
    if (!m_dax)
        return IqLease::refuse(IqRefusal::NoProvider);
    if (!m_connected)
        return IqLease::refuse(IqRefusal::NotConnected);
    if (m_panLister && !m_panLister().contains(endpointId))
        return IqLease::refuse(IqRefusal::UnknownEndpoint);

    // Reuse a channel this pan is ALREADY bound to before creating anything.
    // Such a stream belongs to the DAX-IQ applet or the WFM demodulator, and
    // this consumer borrows it: removing it on our own release would silently
    // kill a stream another local consumer is using.
    int channel = 0;
    bool owned  = false;
    for (int ch = 1; ch <= kNumChannels; ++ch) {
        const auto& s = m_dax->stream(ch);
        if (s.exists && s.panId == endpointId) {
            channel = ch;
            break;
        }
    }
    if (channel == 0) {
        for (int ch = 1; ch <= kNumChannels; ++ch) {
            if (!m_dax->stream(ch).exists && !m_channelPan.contains(ch)) {
                channel = ch;
                owned   = true;
                break;
            }
        }
    }
    if (channel == 0) {
        return IqLease::refuse(
            IqRefusal::CapacityExceeded,
            QStringLiteral("all %1 DAX-IQ channels are in use").arg(kNumChannels));
    }

    if (owned) {
        m_dax->createStream(channel);
        // Creating the stream is not enough to make IQ flow: `daxiq_channel` is
        // a Panadapter property, so the radio streams nothing (or a stale pan's
        // IQ) until a panadapter is routed to this channel. The GUI does this
        // from the spectrum overlay menu; a skimmer client has no such hook.
        if (m_sendCommand) {
            m_sendCommand(QStringLiteral("display pan set %1 daxiq_channel=%2")
                              .arg(endpointId).arg(channel));
        }
    }

    int achieved = currentRateFor(channel);
    if (requestedRateHz > 0 && rateSupported(requestedRateHz)
        && requestedRateHz != achieved) {
        m_dax->setSampleRate(channel, requestedRateHz);
        achieved = requestedRateHz;
    }

    m_bindings.insert(endpointId, Binding{channel, owned});
    m_channelPan.insert(channel, endpointId);

    IqLease l;
    l.granted        = true;
    l.endpointId     = endpointId;
    l.achievedRateHz = achieved;
    l.centerHz       = centerFor(endpointId);
    l.detail = QStringLiteral("DAX-IQ channel %1 (%2)")
                   .arg(channel)
                   .arg(owned ? QStringLiteral("created") : QStringLiteral("borrowed"));
    qCInfo(lcDax) << "FlexIqProvider: opened" << endpointId << l.detail
                  << "achieved" << achieved << "Hz";
    return l;
}

void FlexIqProvider::closeEndpoint(const QString& endpointId)
{
    const auto it = m_bindings.constFind(endpointId);
    if (it == m_bindings.constEnd())
        return;
    const Binding b = *it;
    m_bindings.remove(endpointId);
    m_channelPan.remove(b.channel);
    if (b.owned && m_dax) {
        m_dax->removeStream(b.channel);
        qCInfo(lcDax) << "FlexIqProvider: removed owned DAX-IQ channel" << b.channel
                      << "for" << endpointId;
    } else {
        qCInfo(lcDax) << "FlexIqProvider: detached borrowed DAX-IQ channel"
                      << b.channel << "for" << endpointId;
    }
}

IqLease FlexIqProvider::adjustRate(const QString& endpointId, int requestedRateHz)
{
    const auto it = m_bindings.constFind(endpointId);
    if (it == m_bindings.constEnd() || !m_dax)
        return IqProvider::adjustRate(endpointId, requestedRateHz);

    const int channel = it->channel;
    int achieved = currentRateFor(channel);
    if (requestedRateHz > 0) {
        if (rateSupported(requestedRateHz)) {
            if (requestedRateHz != achieved) {
                // PER STREAM, addressed by the endpoint the caller actually
                // leased. This used to be hardwired to DAX channel 1 on both
                // the get and the set path, so TRX 1-3 reported — and set — a
                // rate belonging to another receiver entirely.
                m_dax->setSampleRate(channel, requestedRateHz);
                // Echoed OPTIMISTICALLY, because a skimmer blocks on this reply
                // and the radio's daxiq_rate status is a round trip away
                // (#3910). The radio stays authoritative: endpoints() reads the
                // rate the radio reported, so the moment its status lands the
                // optimistic value is superseded rather than remembered over it.
                achieved = requestedRateHz;
            }
        } else {
            // Refused without hanging the requester: the rate that remains in
            // force is reported instead. (#3913 review)
            qCInfo(lcDax) << "FlexIqProvider: unsupported rate" << requestedRateHz
                          << "for" << endpointId << "— keeping" << achieved;
        }
    }

    IqLease l;
    l.granted        = true;
    l.endpointId     = endpointId;
    l.achievedRateHz = achieved;
    l.centerHz       = centerFor(endpointId);
    return l;
}

void FlexIqProvider::feedRawIqPacket(int daxChannel, const QByteArray& lePayload,
                                     int sampleRateHz)
{
    const auto it = m_channelPan.constFind(daxChannel);
    if (it == m_channelPan.constEnd())
        return;
    const QString endpointId = *it;
    if (!isLeased(endpointId))
        return;

    // dax_iq payloads are LITTLE-endian float32 — the radio reports
    // payload_endian=little for this stream type alone. Normalised ONCE, here,
    // so no consumer has to know it. (#3913)
    const int numFloats = static_cast<int>(lePayload.size() / 4);
    const int pairs     = numFloats / 2;
    if (pairs <= 0)
        return;

    std::vector<IqSample> canonical(static_cast<std::size_t>(pairs));
    const char* src = lePayload.constData();
    for (int n = 0; n < pairs; ++n) {
        quint32 ri = 0, rq = 0;
        std::memcpy(&ri, src + (n * 2) * 4, 4);
        std::memcpy(&rq, src + (n * 2 + 1) * 4, 4);
        ri = qFromLittleEndian(ri);
        rq = qFromLittleEndian(rq);
        float fi = 0.0f, fq = 0.0f;
        std::memcpy(&fi, &ri, 4);
        std::memcpy(&fq, &rq, 4);
        // Already the analytic convention on this transport: a Flex DAX-IQ
        // stream puts RF above the pan centre at a positive frequency, so no
        // conjugation here. The HL2 adapter conjugates because its wire does
        // not — the invariant is the same, the transports differ.
        canonical[static_cast<std::size_t>(n)] = IqSample(fi, fq);
    }

    publishBlock(endpointId, sampleRateHz, centerFor(endpointId), std::move(canonical));
}

void FlexIqProvider::handleDisconnect()
{
    // A hard disconnect never delivers a per-stream "removed" status, so the
    // bindings would otherwise survive into the next session and a lease would
    // be re-served from a channel the radio no longer has. (Same failure shape
    // as DaxIqModel::handleDisconnect / #3522.)
    m_bindings.clear();
    m_channelPan.clear();
    invalidateAllEndpoints(IqRefusal::NotConnected,
                           QStringLiteral("radio disconnected"));
}

}  // namespace AetherSDR
