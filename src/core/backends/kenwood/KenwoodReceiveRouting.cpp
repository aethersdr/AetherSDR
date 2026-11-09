#include "KenwoodBackend.h"

#include <QMetaType>
#include <QPointer>
#include <QScopeGuard>

namespace AetherSDR::kenwood {
QString KenwoodBackend::receiveRoutingContextKey() const
{
    const qint64 frequency = m_receiveVfo >= 0 && m_receiveVfo <= 1 ? m_frequency[m_receiveVfo] : 0;
    return QStringLiteral("%1:%2:%3:%4").arg(receiveControlContext()).arg(m_tune.has_value()).arg(m_tuneSerial).arg(frequency);
}

bool KenwoodBackend::receiveRoutingWritable() const
{
    return isConnected() && m_identityConfirmed && m_receiveVfo >= 0 && m_receiveVfo <= 1
        && m_frequency[m_receiveVfo] > 0 && encodeMode(m_mode).has_value()
        && m_tfSet && !*m_tfSet && m_txStateKnown && !m_txObserved
        && !m_keyIntent && !m_cwIntent && !m_atuIntent && !m_tune
        && m_receiveRoutingContext == receiveRoutingContextKey();
}

void KenwoodBackend::resetReceiveRouting()
{
    ++m_receiveRoutingRevision;
    m_receiveRoutingContext.clear(); m_receiveRoutingAntenna.reset(); m_receiveRoutingExternal.reset();
    m_receiveRoutingPending.clear(); m_receiveRoutingPredispatch.clear(); m_receiveRoutingMinimumBatches.clear();
    m_receiveRoutingInterested = m_receiveRoutingQuerying = m_receiveRoutingRefreshPending = false;
    m_receiveRoutingMinimumBatch = m_controlIngressSequence;
}

bool KenwoodBackend::refreshReceiveRoutingContext(bool publish)
{
    const QString context = receiveRoutingContextKey();
    if (m_receiveRoutingContext == context) { return false; }
    m_receiveRoutingContext = context; ++m_receiveRoutingRevision;
    m_receiveRoutingAntenna.reset(); m_receiveRoutingExternal.reset();
    m_receiveRoutingPending.clear(); m_receiveRoutingPredispatch.clear(); m_receiveRoutingMinimumBatches.clear();
    m_receiveRoutingMinimumBatch = m_controlIngressSequence;
    if (publish) {
        const QPointer<KenwoodBackend> guard(this);
        const quint64 generation = m_generation;
        const quint64 revision = m_receiveRoutingRevision;
        const quint64 querySerial = m_receiveRoutingQuerySerial;
        publishReceiveRouting();
        if (guard && generation == m_generation && revision == m_receiveRoutingRevision && querySerial == m_receiveRoutingQuerySerial) { queryReceiveRouting(m_receiveRoutingRefreshPending); }
    }
    return true;
}

QVariantMap KenwoodBackend::receiveRoutingStatus() const
{
    const bool current = m_receiveRoutingContext == receiveRoutingContextKey();
    const bool ready = receiveRoutingWritable();
    const QString reason = ready ? QString{} : QStringLiteral("Requires confirmed receive frequency, source, mode, RX and TF-SET off; no local transmit intent.");
    QVariantMap operations;
    for (const QString& key : {QStringLiteral("rxAntenna"), QStringLiteral("externalOutput")}) {
        QString unavailable = reason;
        const bool known = current && (key == "rxAntenna" ? m_receiveRoutingAntenna.has_value() : m_receiveRoutingExternal.has_value());
        if (unavailable.isEmpty() && m_receiveRoutingPending.contains(key)) { unavailable = QStringLiteral("Waiting for routing readback; last confirmed value retained."); }
        if (unavailable.isEmpty() && !known) { unavailable = QStringLiteral("Waiting for complete antenna routing readback for this receive frequency."); }
        operations.insert(key, QVariantMap{{"editable", unavailable.isEmpty()}, {"pending", m_receiveRoutingPending.contains(key)}, {"reason", unavailable}});
    }
    QVariantMap status{{"context", QStringLiteral("%1:%2").arg(m_generation).arg(m_receiveRoutingRevision)},
        {"statusRevision", QString::number(m_receiveRoutingStatusRevision)}, {"mode", m_mode}, {"editable", ready}, {"reason", reason}, {"operations", operations}};
    if (current && m_receiveRoutingAntenna) { status.insert("rxAntenna", *m_receiveRoutingAntenna); }
    if (current && m_receiveRoutingExternal) { status.insert("externalOutput", *m_receiveRoutingExternal); }
    return status;
}

void KenwoodBackend::publishReceiveRouting()
{
    ++m_receiveRoutingStatusRevision;
    emit extensionStatus(QStringLiteral("kenwood"), QStringLiteral("receiveRouting"), receiveRoutingStatus());
}

void KenwoodBackend::queryReceiveRouting(bool force)
{
    if ((!force && !m_receiveRoutingInterested) || m_receiveRoutingQuerying || !m_receiveRoutingPredispatch.isEmpty() || !receiveRoutingWritable()) { return; }
    const QPointer<KenwoodBackend> guard(this);
    const quint64 generation = m_generation;
    m_receiveRoutingQuerying = true;
    ++m_receiveRoutingQuerySerial;
    m_receiveRoutingLastQueryBatch = m_controlIngressSequence;
    m_receiveRoutingLastQueryContext = m_receiveRoutingContext;
    const auto lifetime = qScopeGuard([guard, generation] { if (guard && generation == guard->m_generation) { guard->m_receiveRoutingQuerying = false; } });
    send("AN");
}

void KenwoodBackend::adoptReceiveRoutingContext()
{
    refreshReceiveRoutingContext();
}

bool KenwoodBackend::adoptReceiveRouting(const QByteArray& message, quint64 ingressBatch)
{
    if (message.size() != 6 || !message.startsWith("AN") || (message[2] != '1' && message[2] != '2')) { return false; }
    for (int index = 3; index < 6; ++index) { if (message[index] != '0' && message[index] != '1') { return false; } }
    const QPointer<KenwoodBackend> guard(this);
    const quint64 generation = m_generation;
    const bool changedContext = refreshReceiveRoutingContext(false);
    if (changedContext) {
        const quint64 revision = m_receiveRoutingRevision;
        const quint64 observation = m_receiveRoutingObservationSerial;
        publishReceiveRouting();
        if (!guard || generation != m_generation || revision != m_receiveRoutingRevision
            || observation != m_receiveRoutingObservationSerial) { return false; }
    }
    if (ingressBatch <= m_receiveRoutingMinimumBatches.value("rxAntenna", 0)
        || ingressBatch <= m_receiveRoutingMinimumBatches.value("externalOutput", 0)) { return false; }
    if (ingressBatch <= m_receiveRoutingMinimumBatch) {
        m_receiveRoutingRefreshPending = true;
        if (receiveRoutingWritable() && (m_receiveRoutingLastQueryContext != m_receiveRoutingContext
            || m_receiveRoutingLastQueryBatch < ingressBatch)) { queryReceiveRouting(true); }
        return false;
    }
    const bool ready = receiveRoutingWritable();
    const bool antenna = message[3] == '1';
    const bool external = message[5] == '1';
    if (ready) {
        m_receiveRoutingRefreshPending = false;
        for (const QString& key : {QStringLiteral("rxAntenna"), QStringLiteral("externalOutput")}) {
            std::optional<bool>& observed = key == "rxAntenna" ? m_receiveRoutingAntenna : m_receiveRoutingExternal;
            const bool value = key == "rxAntenna" ? antenna : external;
            const bool predispatch = m_receiveRoutingPending.contains(key)
                && m_receiveRoutingPredispatch.value(key, 0) == m_receiveRoutingPending.value(key);
            if (predispatch && observed && *observed == value) { continue; }
            observed = value;
            m_receiveRoutingPending.remove(key); m_receiveRoutingPredispatch.remove(key);
        }
    }
    const quint64 revision = m_receiveRoutingRevision;
    const quint64 observation = ++m_receiveRoutingObservationSerial;
    publishReceiveRouting();
    if (!guard || generation != m_generation || revision != m_receiveRoutingRevision || observation != m_receiveRoutingObservationSerial) { return false; }
    if (changedContext) { queryReceiveRouting(); }
    return guard && generation == m_generation && revision == m_receiveRoutingRevision && observation == m_receiveRoutingObservationSerial;
}

bool KenwoodBackend::handleReceiveRoutingExtension(const QString& verb, quint64 requestId, const QVariant& argument)
{
    if (verb != "receive.routing.get" && verb != "receive.routing.rxAntenna.set" && verb != "receive.routing.externalOutput.set") { return false; }
    const QPointer<KenwoodBackend> guard(this);
    const quint64 generation = m_generation;
    const bool wasInterested = m_receiveRoutingInterested;
    const bool changed = refreshReceiveRoutingContext();
    if (!guard || generation != m_generation) { return true; }
    if (verb == "receive.routing.get") {
        if (argument.isValid() && (argument.typeId() != QMetaType::QVariantMap || !argument.toMap().isEmpty())) {
            emit extensionError(requestId, QStringLiteral("Receive routing query accepts no argument.")); return true;
        }
        m_receiveRoutingInterested = true;
        if (!changed || !wasInterested) { queryReceiveRouting(); }
        if (guard && generation == m_generation) { emit extensionResult(requestId, receiveRoutingStatus()); }
        return true;
    }
    const QString key = verb == "receive.routing.rxAntenna.set" ? QStringLiteral("rxAntenna") : QStringLiteral("externalOutput");
    const QVariantMap fields = argument.toMap();
    const QVariantMap state = receiveRoutingStatus();
    if (argument.typeId() != QMetaType::QVariantMap || fields.size() != 2 || fields.value("context").typeId() != QMetaType::QString
        || fields.value("context") != state.value("context") || fields.value("value").typeId() != QMetaType::Bool
        || !state.value("operations").toMap().value(key).toMap().value("editable").toBool()) {
        emit extensionError(requestId, QStringLiteral("Receive routing requires a strict boolean and current editable frequency context.")); return true;
    }
    const QByteArray digit = fields.value("value").toBool() ? "1" : "0";
    const QByteArray command = key == "rxAntenna" ? "AN9" + digit + "99" : "AN999" + digit;
    const QString context = receiveRoutingContextKey();
    const quint64 revision = m_receiveRoutingRevision;
    const quint64 ticket = ++m_receiveRoutingTicket;
    m_receiveRoutingPending.insert(key, ticket); m_receiveRoutingPredispatch.insert(key, ticket);
    m_receiveRoutingMinimumBatches.insert(key, m_controlIngressSequence);
    const auto current = [guard, generation, revision, context] {
        return guard && generation == guard->m_generation && revision == guard->m_receiveRoutingRevision
            && context == guard->receiveRoutingContextKey() && guard->receiveRoutingWritable();
    };
    const auto abandon = [guard, key, ticket](bool invalidate) {
        if (guard && guard->m_receiveRoutingPending.value(key, 0) == ticket) {
            guard->m_receiveRoutingPending.remove(key); guard->m_receiveRoutingPredispatch.remove(key);
            if (invalidate) {
                if (key == "rxAntenna") { guard->m_receiveRoutingAntenna.reset(); }
                else { guard->m_receiveRoutingExternal.reset(); }
            }
            guard->publishReceiveRouting();
        }
    };
    publishReceiveRouting();
    if (!current() || m_receiveRoutingPending.value(key, 0) != ticket) {
        abandon(false);
        if (guard && generation == m_generation) { emit extensionError(requestId, QStringLiteral("Receive routing ownership changed before dispatch.")); }
        return true;
    }
    m_receiveRoutingPredispatch.remove(key);
    m_receiveRoutingMinimumBatches.insert(key, m_controlIngressSequence);
    const bool sent = send(command);
    if (!guard || generation != m_generation) { return true; }
    if (!sent) {
        abandon(true);
        if (guard && generation == m_generation) { emit extensionError(requestId, QStringLiteral("Receive routing setter was not dispatched; read settings again.")); }
        return true;
    }
    if (m_receiveRoutingPending.value(key, 0) == ticket) {
        if (!current()) {
            abandon(false);
            if (guard && generation == m_generation) { emit extensionError(requestId, QStringLiteral("Receive routing context changed during dispatch.")); }
            return true;
        }
        const bool queried = send("AN");
        if (!guard || generation != m_generation) { return true; }
        if (!queried || (m_receiveRoutingPending.value(key, 0) == ticket && !current())) {
            abandon(!queried);
            if (guard && generation == m_generation) { emit extensionError(requestId, QStringLiteral("Receive routing readback failed or context changed.")); }
            return true;
        }
    }
    if (context != receiveRoutingContextKey()) { emit extensionError(requestId, QStringLiteral("Receive routing source changed during handoff; outcome unconfirmed.")); }
    else { emit extensionResult(requestId, true); }
    return true;
}
} // namespace AetherSDR::kenwood
