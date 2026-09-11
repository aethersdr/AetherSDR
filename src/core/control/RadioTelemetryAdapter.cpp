#include "RadioTelemetryAdapter.h"

#include "models/RadioModel.h"

#include <QDateTime>

#include <algorithm>
#include <cmath>
#include <utility>

namespace AetherSDR::control {
namespace {
// Reject rather than truncate identifiers/descriptions into misleading aliases.
bool boundedText(const QString& text, qsizetype limit)
{
    if (text.size() > limit || !text.isValidUtf16()) {
        return false;
    }
    for (QChar ch : text) {
        if (ch.isNull() || ch.category() == QChar::Other_Control) {
            return false;
        }
    }
    return true;
}
} // namespace

RadioTelemetryAdapter::RadioTelemetryAdapter(RadioModel* radio, ControlResourceStore* store,
    QString sessionId, QObject* parent, std::function<qint64()> clockMs)
    : QObject(parent), m_radio(radio), m_store(store), m_sessionId(std::move(sessionId)),
      m_clockMs(std::move(clockMs))
{
    m_clock.start();
    MeterModel* meters = &radio->meterModel();
    connect(meters, &MeterModel::meterDefinitionChanged, this, [this](int id) { define(id); });
    connect(meters, &MeterModel::meterUpdated, this, &RadioTelemetryAdapter::sample);
    connect(meters, &MeterModel::meterRemoved, this, [this](int id) {
        m_meters.remove(id);
        m_store->remove({QStringLiteral("meter"), m_sessionId, QString::number(id)});
    });
    connect(meters, &MeterModel::metersCleared, this, &RadioTelemetryAdapter::clear);
    connect(radio, &RadioModel::connectionStateChanged, this, &RadioTelemetryAdapter::resetConnection);
    connect(radio, &RadioModel::backendRebuilt, this, &RadioTelemetryAdapter::resetConnection);
    connect(radio, &RadioModel::capabilitiesChanged, this, [this] {
        m_canTransmit = m_radio->backendCapabilities().canTransmit;
        m_confirmedTransmit.reset();
        publishTransmit();
    });
    connect(radio, &RadioModel::radioTransmitConfirmed, this, [this](bool active) {
        if (m_radio->isConnected()) {
            m_confirmedTransmit = active;
            publishTransmit();
        }
    });
    const auto activity = [this](bool active) {
        if (active && m_confirmedTransmit != true) {
            m_confirmedTransmit.reset();
        }
        publishTransmit();
    };
    connect(radio, &RadioModel::radioTransmittingChanged, this, activity);
    connect(&radio->transmitModel(), &TransmitModel::transmittingChanged, this, activity);
    connect(&radio->transmitModel(), &TransmitModel::moxChanged, this, activity);
    connect(&radio->transmitModel(), &TransmitModel::tuneChanged, this, activity);
    connect(&m_timer, &QTimer::timeout, this, &RadioTelemetryAdapter::flush);
    m_timer.setInterval(kPublishIntervalMs);
    // A precise timer never publishes more frequently than the advertised cap.
    m_timer.setTimerType(Qt::PreciseTimer);
    resetConnection();
}

RadioTelemetryAdapter::~RadioTelemetryAdapter()
{
    // The owning RadioResourceAdapter is already destroying its members.
    // Unlike a live clear, teardown must neither emit deliveryChanged (which
    // re-enters that owner) nor restart the publication timer.
    m_timer.stop();
    for (auto it = m_meters.cbegin(); it != m_meters.cend(); ++it) {
        m_store->remove({QStringLiteral("meter"), m_sessionId, QString::number(it.key())});
    }
    m_store->remove({QStringLiteral("transmitState"), m_sessionId, {}});
}

qint64 RadioTelemetryAdapter::now() const
{
    return m_clockMs ? m_clockMs() : m_clock.elapsed();
}

QJsonObject RadioTelemetryAdapter::meterDelivery() const
{
    return {{QStringLiteral("maxEntries"), kMaxMeters}, {QStringLiteral("limited"), m_limited},
            {QStringLiteral("publishIntervalMs"), kPublishIntervalMs},
            {QStringLiteral("staleAfterMs"), kStaleAfterMs}};
}

void RadioTelemetryAdapter::clear()
{
    m_timer.stop();
    for (auto it = m_meters.cbegin(); it != m_meters.cend(); ++it) {
        m_store->remove({QStringLiteral("meter"), m_sessionId, QString::number(it.key())});
    }
    m_meters.clear();
    const bool wasLimited = std::exchange(m_limited, false);
    if (wasLimited) {
        emit deliveryChanged();
    }
    if (m_radio->isConnected()) {
        m_timer.start();
    }
}

void RadioTelemetryAdapter::resetConnection()
{
    clear();
    m_confirmedTransmit.reset();
    m_canTransmit = m_radio->backendCapabilities().canTransmit;
    if (m_radio->isConnected()) {
        for (int id : m_radio->meterModel().firstDefinedIndices(kMaxMeters)) {
            define(id);
        }
        if (m_radio->meterModel().definitionCount() > kMaxMeters && !m_limited) {
            m_limited = true;
            emit deliveryChanged();
        }
        m_timer.start();
    }
    publishTransmit();
    flush();
}

void RadioTelemetryAdapter::define(int index)
{
    if (!m_radio->isConnected()) {
        return;
    }
    const MeterDef* def = m_radio->meterModel().meterDef(index);
    if (!def) {
        return;
    }
    const bool valid = index >= 0 && index <= 65535
        && boundedText(def->source, 32) && boundedText(def->name, 32)
        && !def->source.isEmpty() && !def->name.isEmpty()
        && boundedText(def->unit, 16) && boundedText(def->description, 128)
        && std::isfinite(def->low) && std::isfinite(def->high) && def->low <= def->high;
    if (!valid || (!m_meters.contains(index) && m_meters.size() >= kMaxMeters)) {
        m_meters.remove(index);
        m_store->remove({QStringLiteral("meter"), m_sessionId, QString::number(index)});
        if (!m_limited) {
            m_limited = true;
            emit deliveryChanged();
        }
        return;
    }
    const QJsonObject definition{{QStringLiteral("source"), def->source},
        {QStringLiteral("sourceIndex"), def->sourceIndex}, {QStringLiteral("name"), def->name},
        {QStringLiteral("unit"), def->unit}, {QStringLiteral("minimum"), def->low},
        {QStringLiteral("maximum"), def->high}, {QStringLiteral("description"), def->description}};
    auto existing = m_meters.find(index);
    if (existing != m_meters.end() && existing->definition == definition) {
        return;
    }
    // Changed metadata (especially units) invalidates the old sample. A model
    // definition is not a sample, even if MeterModel retains its legacy value.
    m_meters[index] = Meter{definition, {}, 0, def->name == QStringLiteral("SWR")};
}

void RadioTelemetryAdapter::sample(int index, float value)
{
    auto it = m_meters.find(index);
    if (!m_radio->isConnected() || it == m_meters.end() || !std::isfinite(value)) {
        return;
    }
    it->sample = value;
    it->sampledAt = now(); // unchanged samples refresh liveness too
}

void RadioTelemetryAdapter::flush()
{
    if (!m_radio->isConnected()) {
        return;
    }
    const qint64 current = now();
    for (auto it = m_meters.cbegin(); it != m_meters.cend(); ++it) {
        const Meter& meter = it.value();
        const qint64 age = meter.sample ? std::clamp(current - meter.sampledAt,
            qint64(0), qint64(kStaleAfterMs + 1)) : -1;
        const bool fresh = meter.sample && age <= kStaleAfterMs;
        const bool valid = fresh && (!meter.swr || m_radio->meterModel().swrSampleLive(
            QDateTime::currentMSecsSinceEpoch(), MeterModel::kTxMeterStaleMs));
        QJsonObject value{{QStringLiteral("id"), QString::number(it.key())},
            {QStringLiteral("definition"), meter.definition},
            {QStringLiteral("sample"), QJsonObject{{QStringLiteral("known"), meter.sample.has_value()},
                {QStringLiteral("fresh"), fresh}, {QStringLiteral("valid"), valid},
                {QStringLiteral("ageMs"), age},
                {QStringLiteral("value"), valid ? QJsonValue(*meter.sample) : QJsonValue(QJsonValue::Null)}}}};
        m_store->upsert({QStringLiteral("meter"), m_sessionId, QString::number(it.key())}, value);
    }
}

void RadioTelemetryAdapter::publishTransmit()
{
    const bool connected = m_radio->isConnected();
    QString state = QStringLiteral("unknown");
    if (connected && !m_canTransmit) {
        state = QStringLiteral("unsupported");
    } else if (connected && m_confirmedTransmit
               && (*m_confirmedTransmit || (!m_radio->isRadioTransmitting()
                   && !m_radio->transmitModel().isTransmitting()
                   && !m_radio->transmitModel().isMox() && !m_radio->transmitModel().isTuning()))) {
        state = *m_confirmedTransmit ? QStringLiteral("transmitting") : QStringLiteral("idle");
    }
    const TransmitModel& tx = m_radio->transmitModel();
    m_store->upsert({QStringLiteral("transmitState"), m_sessionId, {}},
        {{QStringLiteral("connected"), connected}, {QStringLiteral("canTransmit"), m_canTransmit},
         {QStringLiteral("state"), state},
         {QStringLiteral("localRequests"), QJsonObject{{QStringLiteral("mox"), tx.isMox()},
             {QStringLiteral("tune"), tx.isTuning()}, {QStringLiteral("transmitting"), tx.isTransmitting()}}}});
}
} // namespace AetherSDR::control
