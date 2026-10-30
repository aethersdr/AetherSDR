#include "core/backends/flex/FlexReceiveData.h"
#include <QMutexLocker>
#include <algorithm>
#include <cmath>
#include <utility>

namespace AetherSDR {
namespace {
bool validBins(const QVector<float>& bins)
{
    return !bins.empty() && bins.size() <= FlexReceiveData::kMaxBins
        && std::all_of(bins.cbegin(), bins.cend(), [](float value) { return std::isfinite(value); });
}
bool expired(const ReceiveReceipt& receipt, qint64 now)
{
    return receipt.monotonicNs <= 0 || receipt.monotonicNs > now
        || now - receipt.monotonicNs > FlexReceiveData::kMaxAgeNs;
}
}

FlexReceiveData::FlexReceiveData(Wake wake) : m_wake(std::move(wake)) {}
FlexReceiveData::~FlexReceiveData() { close(); }

void FlexReceiveData::retireLocked()
{
    if (m_session) { m_session->active.store(false, std::memory_order_release); }
    m_session.reset();
    m_pans.clear();
    m_waterfalls.clear();
    m_definitions.clear();
    m_display.clear();
    m_meters.clear();
    m_telemetry.reset();
    m_displayBytes = 0;
    m_meterBytes = 0;
    m_scheduled = false;
    accountLocked();
}
void FlexReceiveData::begin()
{
    QMutexLocker lock(&m_mutex);
    retireLocked();
    ++m_generation;
    m_session = std::make_shared<ReceiveDataEpoch>();
    m_stats = {};
}
void FlexReceiveData::retire()
{
    QMutexLocker lock(&m_mutex);
    retireLocked();
}
void FlexReceiveData::close()
{
    QMutexLocker lock(&m_mutex);
    retireLocked();
    m_wake = {};
}
void FlexReceiveData::clearStreams()
{
    QMutexLocker lock(&m_mutex);
    for (const auto& epoch : std::as_const(m_pans)) { epoch->active.store(false, std::memory_order_release); }
    for (const auto& epoch : std::as_const(m_waterfalls)) { epoch->active.store(false, std::memory_order_release); }
    m_pans.clear();
    m_waterfalls.clear();
    m_display.clear();
    m_displayBytes = 0;
    accountLocked();
}
ReceiveDataLease FlexReceiveData::sessionLease() const
{
    QMutexLocker lock(&m_mutex);
    return m_session;
}
void FlexReceiveData::registerStream(quint32 id, bool waterfall)
{
    QMutexLocker lock(&m_mutex);
    auto& streams = waterfall ? m_waterfalls : m_pans;
    if (!m_session || streams.contains(id)) { return; }
    if (streams.size() >= kMaxStreams) { ++m_stats.rejected; return; }
    auto epoch = std::make_shared<ReceiveDataEpoch>();
    epoch->session = m_session;
    streams.insert(id, std::move(epoch));
}
void FlexReceiveData::unregisterStream(quint32 id, bool waterfall)
{
    QMutexLocker lock(&m_mutex);
    auto& streams = waterfall ? m_waterfalls : m_pans;
    const auto found = streams.find(id);
    if (found != streams.end()) {
        (*found)->active.store(false, std::memory_order_release);
        streams.erase(found);
    }
    for (auto it = m_display.begin(); it != m_display.end();) {
        if (streamId(*it) == id && std::holds_alternative<ReceiveWaterfallRow>(*it) == waterfall) {
            m_displayBytes -= bytes(*it);
            it = m_display.erase(it);
        } else { ++it; }
    }
    accountLocked();
}
ReceiveDataLease FlexReceiveData::streamLease(quint32 id, bool waterfall) const
{
    QMutexLocker lock(&m_mutex);
    return (waterfall ? m_waterfalls : m_pans).value(id);
}
void FlexReceiveData::defineMeter(const MeterDef& definition)
{
    QMutexLocker lock(&m_mutex);
    if (!m_session || definition.index < 0 || definition.index > 65535) { return; }
    auto found = m_definitions.find(definition.index);
    if (found != m_definitions.end()) {
        const MeterDef& previous = found->definition;
        if (previous.source == definition.source && previous.sourceIndex == definition.sourceIndex
            && previous.name == definition.name && previous.unit == definition.unit) {
            found->definition = definition;
            return;
        }
        found->epoch->active.store(false, std::memory_order_release);
    } else if (m_definitions.size() >= kMaxDefinitions) {
        ++m_stats.rejected;
        return;
    }
    auto epoch = std::make_shared<ReceiveDataEpoch>();
    epoch->session = m_session;
    m_definitions.insert(definition.index, Meter{definition, std::move(epoch)});
}
void FlexReceiveData::removeMeter(int id)
{
    QMutexLocker lock(&m_mutex);
    const auto found = m_definitions.find(id);
    if (found != m_definitions.end()) {
        found->epoch->active.store(false, std::memory_order_release);
        m_definitions.erase(found);
    }
}
float FlexReceiveData::convertMeter(const QString& unit, qint16 raw)
{
    // FlexLib Meter.UpdateValue: retain the current modern-firmware scaling.
    if (unit == QLatin1String("dBm") || unit == QLatin1String("dB")
        || unit == QLatin1String("dBFS") || unit == QLatin1String("SWR")) {
        return static_cast<float>(raw) / 128.0f;
    }
    if (unit == QLatin1String("Volts") || unit == QLatin1String("Amps")) {
        return static_cast<float>(raw) / 256.0f;
    }
    if (unit == QLatin1String("degF") || unit == QLatin1String("degC")) {
        return static_cast<float>(raw) / 64.0f;
    }
    return static_cast<float>(raw);
}
qsizetype FlexReceiveData::bytes(const Display& value)
{
    return std::visit([](const auto& frame) -> qsizetype {
        if constexpr (std::is_same_v<std::decay_t<decltype(frame)>, ReceiveSpectrumFrame>) {
            return sizeof(frame) + frame.binsDbm.capacity() * sizeof(float);
        } else { return sizeof(frame) + frame.intensity.capacity() * sizeof(float); }
    }, value);
}
qsizetype FlexReceiveData::bytes(const ReceiveMeterBatch& value)
{
    return sizeof(value) + value.samples.capacity() * sizeof(ReceiveMeterSample);
}
quint32 FlexReceiveData::streamId(const Display& value)
{
    return std::visit([](const auto& frame) { return frame.streamId; }, value);
}
void FlexReceiveData::accountLocked()
{
    m_stats.queuedBytes = m_displayBytes + m_meterBytes;
    m_stats.peakQueuedBytes = std::max(m_stats.peakQueuedBytes, m_stats.queuedBytes);
}
void FlexReceiveData::wakeLocked()
{
    if (!m_scheduled && !m_draining && m_wake && m_session
        && (!m_display.empty() || !m_meters.empty() || m_telemetry)) {
        m_scheduled = true;
        m_wake(m_generation);
    }
}
void FlexReceiveData::dropDisplayLocked(std::deque<Display>::iterator it)
{
    if (std::holds_alternative<ReceiveSpectrumFrame>(*it)) { ++m_stats.spectrumReplaced; }
    else { ++m_stats.waterfallDropped; }
    m_displayBytes -= bytes(*it);
    m_display.erase(it);
}
void FlexReceiveData::dropMeterLocked()
{
    m_meterBytes -= bytes(m_meters.front());
    m_meters.pop_front();
    ++m_stats.meterBatchesDropped;
}
void FlexReceiveData::displayLocked(Display value)
{
    const qsizetype size = bytes(value);
    if (size > kMaxDisplayBytes) { ++m_stats.rejected; return; }
    const bool waterfall = std::holds_alternative<ReceiveWaterfallRow>(value);
    const quint32 id = streamId(value);
    qsizetype sameStream = 0;
    for (const Display& pending : m_display) {
        if (streamId(pending) == id
            && std::holds_alternative<ReceiveWaterfallRow>(pending) == waterfall) { ++sameStream; }
    }
    if (sameStream >= (waterfall ? kRowsPerStream : 1)) {
        const auto oldest = std::find_if(m_display.begin(), m_display.end(), [id, waterfall](const Display& pending) {
            return streamId(pending) == id
                && std::holds_alternative<ReceiveWaterfallRow>(pending) == waterfall;
        });
        dropDisplayLocked(oldest);
    }
    while (!m_display.empty() && (m_displayBytes + size > kMaxDisplayBytes
                                 || m_display.size() >= kMaxDisplayFrames)) {
        dropDisplayLocked(m_display.begin());
    }
    m_displayBytes += size;
    m_display.push_back(std::move(value));
    accountLocked();
    wakeLocked();
}
void FlexReceiveData::spectrum(ReceiveSpectrumFrame frame)
{
    if (!frame.current() || !frame.decodeScale.valid() || !validBins(frame.binsDbm)) { return; }
    frame.binsDbm.detach();
    QMutexLocker lock(&m_mutex);
    if (frame.lease != m_pans.value(frame.streamId) || !frame.current()) { return; }
    displayLocked(std::move(frame));
}
void FlexReceiveData::waterfall(ReceiveWaterfallRow row)
{
    if (!row.current() || !validBins(row.intensity) || !std::isfinite(row.lowMhz)
        || !std::isfinite(row.highMhz) || row.highMhz <= row.lowMhz) { return; }
    row.intensity.detach();
    QMutexLocker lock(&m_mutex);
    if (row.lease != m_waterfalls.value(row.streamId) || !row.current()) { return; }
    displayLocked(std::move(row));
}
void FlexReceiveData::meters(const QVector<quint16>& ids, const QVector<qint16>& values,
                             ReceiveReceipt receipt, ReceiveDataLease session)
{
    QMutexLocker lock(&m_mutex);
    if (!m_session || ids.empty() || (session && session != m_session)) { return; }
    if (ids.size() != values.size() || ids.size() > 16384) { ++m_stats.rejected; return; }
    ReceiveMeterBatch batch;
    batch.lease = m_session;
    batch.receipt = receipt;
    batch.samples.reserve(std::min(ids.size(), kMaxDefinitions));
    for (qsizetype i = 0; i < ids.size(); ++i) {
        const auto found = m_definitions.constFind(ids[i]);
        if (found != m_definitions.cend()) {
            batch.samples.append({ids[i], convertMeter(found->definition.unit, values[i]), found->epoch});
        }
    }
    if (batch.samples.empty()) { return; }
    const qsizetype size = bytes(batch);
    if (size > kMaxMeterBytes || expired(receipt, receiveMonotonicNs())) {
        ++m_stats.meterBatchesDropped;
        return;
    }
    while (!m_meters.empty() && (m_meterBytes + size > kMaxMeterBytes
        || m_meters.size() >= kMaxMeterBatches || expired(m_meters.front().receipt, receiveMonotonicNs()))) {
        dropMeterLocked();
    }
    m_meterBytes += size;
    m_meters.push_back(std::move(batch));
    accountLocked();
    wakeLocked();
}
void FlexReceiveData::telemetry(ReceiveTelemetry snapshot)
{
    QMutexLocker lock(&m_mutex);
    if (!m_session || snapshot.lease != m_session || !snapshot.current()) { return; }
    snapshot.delivery = m_stats;
    m_telemetry = std::move(snapshot);
    wakeLocked();
}
bool FlexReceiveData::beginDrain()
{
    QMutexLocker lock(&m_mutex);
    if (m_draining) { return false; }
    m_draining = true;
    return true;
}
void FlexReceiveData::endDrain()
{
    QMutexLocker lock(&m_mutex);
    m_draining = false;
    wakeLocked();
}
QVector<FlexReceiveData::Event> FlexReceiveData::take(quint64 generation, qint64 nowNs)
{
    QMutexLocker lock(&m_mutex);
    QVector<Event> result;
    if (!m_session || generation != m_generation) { return result; }
    m_scheduled = false;
    if (m_telemetry) {
        result.append(std::move(*m_telemetry));
        m_telemetry.reset();
    }
    // Count examined entries too: expiry must not turn one drain into an
    // unbounded cleanup pass. Alternation reserves service for meter packets.
    for (qsizetype i = 0; i < kDrainEvents && (!m_display.empty() || !m_meters.empty()); ++i) {
        m_meterTurn = !m_meterTurn;
        if (!m_meters.empty() && (m_meterTurn || m_display.empty())) {
            ReceiveMeterBatch batch = std::move(m_meters.front());
            m_meterBytes -= bytes(batch);
            m_meters.pop_front();
            if (batch.current() && !expired(batch.receipt, nowNs)) { result.append(std::move(batch)); }
            else { ++m_stats.meterBatchesDropped; }
        } else {
            Display display = std::move(m_display.front());
            m_displayBytes -= bytes(display);
            m_display.pop_front();
            std::visit([&](auto& frame) {
                if (frame.current() && !expired(frame.receipt, nowNs)) { result.append(std::move(frame)); }
                else if constexpr (std::is_same_v<std::decay_t<decltype(frame)>, ReceiveSpectrumFrame>) {
                    ++m_stats.spectrumReplaced;
                } else { ++m_stats.waterfallDropped; }
            }, display);
        }
    }
    accountLocked();
    wakeLocked();
    return result;
}
ReceiveDeliveryStats FlexReceiveData::stats() const
{
    QMutexLocker lock(&m_mutex);
    return m_stats;
}

} // namespace AetherSDR
