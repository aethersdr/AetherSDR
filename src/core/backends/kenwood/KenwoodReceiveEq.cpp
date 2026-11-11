#include "KenwoodBackend.h"

#include <QMetaType>
#include <QPointer>
#include <QScopeGuard>

namespace AetherSDR::kenwood {
namespace {
QVariantList rawVariant(const QList<int>& raw)
{
    QVariantList result;
    for (int value : raw) { result.append(value); }
    return result;
}
std::optional<QList<int>> rawArgument(const QVariant& argument)
{
    if (argument.typeId() != QMetaType::QVariantList || argument.toList().size() != 18) { return {}; }
    QList<int> values;
    for (const QVariant& item : argument.toList()) {
        if (item.typeId() != QMetaType::Int || item.toInt() < 0 || item.toInt() > 30) { return {}; }
        values.append(item.toInt());
    }
    return values;
}
QByteArray curveCommand(const QList<int>& values)
{
    QByteArray command("UR");
    for (int value : values) { command += QByteArray::number(value).rightJustified(2, '0'); }
    return command;
}
}

QString KenwoodBackend::receiveEqContextKey() const
{
    return QStringLiteral("%1:%2:%3").arg(receiveControlContext()).arg(m_tune.has_value()).arg(m_tuneSerial);
}

bool KenwoodBackend::receiveEqWritable() const
{
    return isConnected() && m_identityConfirmed && m_receiveVfo >= 0 && m_receiveVfo <= 1
        && m_frequency[m_receiveVfo] > 0 && encodeMode(m_mode).has_value()
        && m_tfSet && !*m_tfSet && m_txStateKnown && !m_txObserved
        && !m_keyIntent && !m_cwIntent && !m_atuIntent && !m_tune
        && m_receiveEqContext == receiveEqContextKey();
}

void KenwoodBackend::resetReceiveEq()
{
    ++m_receiveEqRevision;
    ++m_receiveEqCurveRevision;
    m_receiveEqContext.clear();
    m_receiveEqEnabled.reset(); m_receiveEqEffect.reset(); m_receiveEqCurve.clear();
    m_receiveEqPending.clear(); m_receiveEqMinimumBatches.clear(); m_receiveEqCopy.reset();
    m_receiveEqInterested = m_receiveEqPredispatch = m_receiveEqQuerying = false;
    m_receiveEqAwaitingEffect = m_receiveEqCurveQualified = false;
    m_receiveEqMinimumBatch = m_receiveEqCurveMinimumBatch = m_controlIngressSequence;
}

bool KenwoodBackend::refreshReceiveEqContext()
{
    const QString context = receiveEqContextKey();
    if (m_receiveEqContext == context) { return false; }
    m_receiveEqContext = context;
    ++m_receiveEqRevision; ++m_receiveEqCurveRevision;
    m_receiveEqEnabled.reset(); m_receiveEqEffect.reset(); m_receiveEqCurve.clear();
    m_receiveEqPending.clear(); m_receiveEqMinimumBatches.clear(); m_receiveEqCopy.reset();
    m_receiveEqPredispatch = m_receiveEqAwaitingEffect = m_receiveEqCurveQualified = false;
    m_receiveEqMinimumBatch = m_receiveEqCurveMinimumBatch = m_controlIngressSequence;
    const QPointer<KenwoodBackend> guard(this);
    const quint64 generation = m_generation;
    const quint64 revision = m_receiveEqRevision;
    publishReceiveEq();
    if (guard && generation == m_generation && revision == m_receiveEqRevision) { queryReceiveEq(); }
    return true;
}

QVariantMap KenwoodBackend::receiveEqCopyStatus() const
{
    if (!m_receiveEqCopy) { return {}; }
    const ReceiveEqCopy& record = *m_receiveEqCopy;
    QString state = QStringLiteral("sentUnverified");
    QString reason = QStringLiteral("Copy request sent; destination not yet verified.");
    if (record.baseContext != receiveEqContextKey() || !receiveEqWritable()) {
        state = QStringLiteral("stale"); reason = QStringLiteral("Copy inspection requires its original receive session, source and mode.");
    } else if (m_receiveEqEffect && *m_receiveEqEffect == record.destination + 6
        && m_receiveEqCurveQualified && !m_receiveEqAwaitingEffect && m_receiveEqPending.isEmpty()
        && m_receiveEqCurveSequence > record.minimumCurveSequence) {
        const bool matched = m_receiveEqCurve == record.sourceCurve;
        state = matched ? QStringLiteral("matched") : QStringLiteral("differs");
        reason = matched ? QStringLiteral("Observed destination matches the captured source curve.")
            : QStringLiteral("Destination differs from captured source; copy outcome indeterminate.");
    }
    return {{"token", record.token}, {"sourceEffect", record.sourceEffect}, {"destination", record.destination},
        {"sourceCurveRaw", rawVariant(record.sourceCurve)}, {"state", state}, {"reason", reason}};
}

QVariantMap KenwoodBackend::receiveEqStatus() const
{
    const bool current = m_receiveEqContext == receiveEqContextKey();
    const bool ready = receiveEqWritable();
    const bool busy = !m_receiveEqPending.isEmpty();
    QVariantMap operations;
    for (const QString& id : {QStringLiteral("enabled"), QStringLiteral("effect"), QStringLiteral("curve"), QStringLiteral("reset"), QStringLiteral("copy")}) {
        QString reason;
        if (!ready) { reason = QStringLiteral("Requires confirmed receive source, mode, RX and TF-SET off; no local transmit intent."); }
        else if (busy) { reason = QStringLiteral("Waiting for the current native equalizer operation."); }
        else if (id == "enabled" && !m_receiveEqEnabled) { reason = QStringLiteral("Waiting for native receive equalizer enable readback."); }
        else if (id == "effect" && !m_receiveEqEffect) { reason = QStringLiteral("Waiting for native receive equalizer effect readback."); }
        else if (id != "enabled" && id != "effect"
            && (!m_receiveEqEffect || !m_receiveEqCurveQualified || m_receiveEqAwaitingEffect || m_receiveEqCurve.size() != 18)) {
            reason = QStringLiteral("Requires a fresh complete curve owned by the confirmed selected effect.");
        }
        const QString pendingKey = id == "reset" ? QStringLiteral("curve") : id;
        operations.insert(id, QVariantMap{{"editable", reason.isEmpty()}, {"pending", m_receiveEqPending.contains(pendingKey)}, {"reason", reason}});
    }
    QVariantMap status{{"context", QStringLiteral("%1:%2").arg(m_generation).arg(m_receiveEqRevision)},
        {"statusRevision", QString::number(m_receiveEqStatusRevision)}, {"mode", m_mode},
        {"curveRevision", QString::number(m_receiveEqCurveRevision)}, {"radioStoredCurve", true}, {"operations", operations}};
    if (current && m_receiveEqEnabled) { status.insert("enabled", *m_receiveEqEnabled); }
    if (current && m_receiveEqEffect) { status.insert("effect", *m_receiveEqEffect); }
    if (current && m_receiveEqCurve.size() == 18) { status.insert("curveRaw", rawVariant(m_receiveEqCurve)); }
    const QVariantMap copy = receiveEqCopyStatus();
    if (!copy.isEmpty()) { status.insert("copyRecord", copy); }
    return status;
}

void KenwoodBackend::publishReceiveEq()
{
    ++m_receiveEqStatusRevision;
    emit extensionStatus(QStringLiteral("kenwood"), QStringLiteral("receiveEq"), receiveEqStatus());
}

void KenwoodBackend::queryReceiveEq()
{
    if (!m_receiveEqInterested || m_receiveEqPredispatch || m_receiveEqQuerying || !receiveEqWritable()) { return; }
    const QPointer<KenwoodBackend> guard(this);
    const quint64 generation = m_generation;
    const quint64 revision = m_receiveEqRevision;
    m_receiveEqQuerying = true;
    const auto queryLifetime = qScopeGuard([guard, generation] {
        if (guard && generation == guard->m_generation) { guard->m_receiveEqQuerying = false; }
    });
    QList<QByteArray> queries{"EQR0", "EQR1"};
    if (m_receiveEqEffect && !m_receiveEqAwaitingEffect) { queries.append("UR"); }
    for (const QByteArray& query : queries) {
        if (!send(query) || !guard || generation != m_generation || revision != m_receiveEqRevision || !receiveEqWritable()) { return; }
    }
}

void KenwoodBackend::adoptReceiveEq(const QByteArray& message, quint64 ingressBatch)
{
    const QPointer<KenwoodBackend> guard(this);
    const quint64 generation = m_generation;
    refreshReceiveEqContext();
    if (!guard || generation != m_generation || !receiveEqWritable() || ingressBatch <= m_receiveEqMinimumBatch) { return; }
    QString field;
    std::optional<bool> enabled;
    std::optional<int> effect;
    QList<int> curve;
    if (message.size() == 5 && message.startsWith("EQR0") && (message[4] == '0' || message[4] == '1')) {
        field = QStringLiteral("enabled"); enabled = message[4] == '1';
    } else if (message.size() == 5 && message.startsWith("EQR1") && message[4] >= '0' && message[4] <= '8') {
        field = QStringLiteral("effect"); effect = message[4] - '0';
    } else if (message.size() == 38 && message.startsWith("UR")) {
        if (!m_receiveEqEffect || m_receiveEqAwaitingEffect || ingressBatch <= m_receiveEqCurveMinimumBatch) { return; }
        field = QStringLiteral("curve");
        for (int band = 0; band < 18; ++band) {
            const char a = message[2 + band * 2]; const char b = message[3 + band * 2];
            if (a < '0' || a > '9' || b < '0' || b > '9') { return; }
            const int value = (a - '0') * 10 + b - '0';
            if (value > 30) { return; }
            curve.append(value);
        }
    } else { return; }
    if (ingressBatch <= m_receiveEqMinimumBatches.value(field, 0)) { return; }
    const bool changed = enabled ? (m_receiveEqEnabled && m_receiveEqEnabled != enabled)
        : effect ? (m_receiveEqEffect && m_receiveEqEffect != effect) : m_receiveEqCurve.size() == 18 && m_receiveEqCurve != curve;
    if (m_receiveEqPredispatch && !m_receiveEqPending.isEmpty()) {
        if (!changed) { return; }
        ++m_receiveEqRevision;
        m_receiveEqPending.clear(); m_receiveEqPredispatch = false;
    }
    bool queryCurve = false;
    if (enabled) {
        m_receiveEqEnabled = enabled;
    } else if (effect) {
        const bool freshOwnership = !m_receiveEqEffect || m_receiveEqEffect != effect || m_receiveEqAwaitingEffect || m_receiveEqPending.contains("effect");
        m_receiveEqEffect = effect;
        if (freshOwnership) {
            ++m_receiveEqRevision; ++m_receiveEqCurveRevision;
            m_receiveEqCurve.clear(); m_receiveEqCurveQualified = false;
            m_receiveEqAwaitingEffect = false;
            m_receiveEqCurveMinimumBatch = ingressBatch;
            queryCurve = true;
        } else if (!m_receiveEqCurveQualified) { queryCurve = true; }
    } else {
        if (m_receiveEqCurve != curve || !m_receiveEqCurveQualified) { ++m_receiveEqCurveRevision; }
        m_receiveEqCurve = curve; m_receiveEqCurveQualified = true; ++m_receiveEqCurveSequence;
    }
    m_receiveEqPending.remove(field);
    const quint64 revision = m_receiveEqRevision;
    publishReceiveEq();
    if (guard && generation == m_generation && revision == m_receiveEqRevision && queryCurve
        && m_receiveEqInterested && !m_receiveEqPredispatch && receiveEqWritable()) { send("UR"); }
}

bool KenwoodBackend::handleReceiveEqExtension(const QString& verb, quint64 requestId, const QVariant& argument)
{
    if (verb != "receive.eq.get" && verb != "receive.eq.enabled.set" && verb != "receive.eq.effect.set"
        && verb != "receive.eq.curve.set" && verb != "receive.eq.curve.reset" && verb != "receive.eq.copy"
        && verb != "receive.eq.inspection.get") { return false; }
    const QPointer<KenwoodBackend> guard(this);
    const quint64 generation = m_generation;
    const bool wasInterested = m_receiveEqInterested;
    const bool changed = refreshReceiveEqContext();
    if (!guard || generation != m_generation) { return true; }
    if (verb == "receive.eq.get") {
        if (argument.isValid() && (argument.typeId() != QMetaType::QVariantMap || !argument.toMap().isEmpty())) { emit extensionError(requestId, QStringLiteral("Receive EQ query accepts no argument.")); return true; }
        m_receiveEqInterested = true;
        if (!changed || !wasInterested) { queryReceiveEq(); }
        if (guard && generation == m_generation) { emit extensionResult(requestId, receiveEqStatus()); }
        return true;
    }
    const QVariantMap fields = argument.toMap();
    if (verb == "receive.eq.inspection.get") {
        const QVariant token = fields.value("copyToken");
        if (argument.typeId() != QMetaType::QVariantMap || fields.size() != 1 || token.typeId() != QMetaType::QString
            || token.toString().size() > 128 || !m_receiveEqCopy || token.toString() != m_receiveEqCopy->token) {
            emit extensionError(requestId, QStringLiteral("Copy inspection requires its current immutable copy token."));
        } else {
            const QVariantMap copyStatus = receiveEqCopyStatus();
            const QString state = copyStatus.value("state").toString();
            emit extensionResult(requestId, QVariantMap{{"copyToken", token},
                {"state", state == QLatin1String("sentUnverified") ? QStringLiteral("awaiting") : state},
                {"reason", copyStatus.value("reason")}});
        }
        return true;
    }
    const bool enable = verb == "receive.eq.enabled.set";
    const bool select = verb == "receive.eq.effect.set";
    const bool reset = verb == "receive.eq.curve.reset";
    const bool copy = verb == "receive.eq.copy";
    const QString key = enable ? QStringLiteral("enabled") : select ? QStringLiteral("effect") : copy ? QStringLiteral("copy") : QStringLiteral("curve");
    const QString operation = reset ? QStringLiteral("reset") : key;
    const QVariantMap status = receiveEqStatus();
    bool valid = argument.typeId() == QMetaType::QVariantMap && fields.value("context").typeId() == QMetaType::QString
        && fields.value("context") == status.value("context") && fields.size() == (enable || select ? 2 : 3)
        && status.value("operations").toMap().value(operation).toMap().value("editable").toBool();
    QByteArray command;
    QByteArray query;
    if (enable) {
        valid = valid && fields.value("enabled").typeId() == QMetaType::Bool;
        command = fields.value("enabled").toBool() ? "EQR01" : "EQR00"; query = "EQR0";
    } else if (select) {
        const QVariant effect = fields.value("effect");
        valid = valid && effect.typeId() == QMetaType::Int && effect.toInt() >= 0 && effect.toInt() <= 8;
        command = "EQR1" + QByteArray::number(effect.toInt()); query = "EQR1";
    } else {
        valid = valid && fields.value("curveRevision").typeId() == QMetaType::QString && fields.value("curveRevision") == status.value("curveRevision");
        if (copy) {
            const QVariant destination = fields.value("destination");
            valid = valid && destination.typeId() == QMetaType::Int && destination.toInt() >= 0 && destination.toInt() <= 2;
            command = "EQR2" + QByteArray::number(destination.toInt());
        } else if (reset) {
            const QVariant bands = fields.value("bands");
            QList<int> chosen;
            QList<int> raw = m_receiveEqCurve;
            valid = valid && bands.typeId() == QMetaType::QVariantList && !bands.toList().isEmpty() && bands.toList().size() <= 18 && raw.size() == 18;
            for (const QVariant& value : bands.toList()) {
                const int band = value.toInt();
                if (value.typeId() != QMetaType::Int || band < 0 || band >= 18 || chosen.contains(band)) { valid = false; break; }
                chosen.append(band);
                if (raw.size() == 18) { raw[band] = 99; }
            }
            command = curveCommand(raw); query = "UR";
        } else {
            const std::optional<QList<int>> raw = rawArgument(fields.value("curveRaw"));
            valid = valid && raw.has_value();
            if (raw) { command = curveCommand(*raw); }
            query = "UR";
        }
    }
    if (!valid) { emit extensionError(requestId, QStringLiteral("Receive EQ operation requires exact typed arguments and current editable readback ownership.")); return true; }
    const QString baseContext = receiveEqContextKey();
    const quint64 revision = m_receiveEqRevision;
    const quint64 curveRevision = m_receiveEqCurveRevision;
    const quint64 ticket = ++m_receiveEqTicket;
    const std::optional<int> sourceEffect = m_receiveEqEffect;
    const QList<int> sourceCurve = m_receiveEqCurve;
    const quint64 minimumCurveSequence = m_receiveEqCurveSequence;
    m_receiveEqPending.insert(key, ticket);
    m_receiveEqMinimumBatches.insert(key, m_controlIngressSequence);
    m_receiveEqPredispatch = true;
    const auto current = [guard, generation, revision, curveRevision, enable, select, baseContext] {
        return guard && generation == guard->m_generation && revision == guard->m_receiveEqRevision
            && (enable || select || curveRevision == guard->m_receiveEqCurveRevision)
            && baseContext == guard->receiveEqContextKey() && guard->receiveEqWritable();
    };
    const auto abandon = [guard, key, ticket] {
        if (guard && guard->m_receiveEqPending.value(key, 0) == ticket) {
            guard->m_receiveEqPending.remove(key); guard->m_receiveEqPredispatch = false; guard->publishReceiveEq();
        }
    };
    publishReceiveEq();
    if (!current() || m_receiveEqPending.value(key, 0) != ticket) {
        abandon();
        if (guard && generation == m_generation) { emit extensionError(requestId, QStringLiteral("Receive EQ ownership changed before dispatch.")); }
        return true;
    }
    m_receiveEqPredispatch = false;
    if (select) { m_receiveEqAwaitingEffect = true; m_receiveEqCurveQualified = false; m_receiveEqCurveMinimumBatch = m_controlIngressSequence; }
    const bool sent = send(command);
    if (!guard || generation != m_generation) { return true; }
    if (!sent) {
        abandon();
        if (guard && generation == m_generation) { emit extensionError(requestId, QStringLiteral("Receive EQ command was not dispatched; radio state remains unconfirmed.")); }
        return true;
    }
    if (copy) {
        if (!current() || m_receiveEqPending.value(key, 0) != ticket) {
            abandon();
            if (guard && generation == m_generation) { emit extensionError(requestId, QStringLiteral("Copy command sent, but receive ownership changed; destination outcome is unverified.")); }
            return true;
        }
        m_receiveEqPending.remove(key);
        const QString token = QStringLiteral("%1:%2").arg(m_generation).arg(++m_receiveEqCopySequence);
        m_receiveEqCopy = ReceiveEqCopy{token, baseContext, sourceEffect.value(), fields.value("destination").toInt(), sourceCurve, minimumCurveSequence};
        emit extensionResult(requestId, QVariantMap{{"sent", true}, {"verified", false}, {"copyToken", token}});
        if (!guard || generation != m_generation) { return true; }
        publishReceiveEq();
        for (const QByteArray& scalarQuery : {QByteArray("EQR0"), QByteArray("EQR1")}) {
            if (!current() || !m_receiveEqCopy || m_receiveEqCopy->token != token || !m_receiveEqPending.isEmpty()) { return true; }
            if (!send(scalarQuery) || !guard || generation != m_generation) { return true; }
        }
        return true;
    }
    if (m_receiveEqPending.value(key, 0) == ticket) {
        if (!current()) {
            abandon();
            if (guard && generation == m_generation) { emit extensionError(requestId, QStringLiteral("Receive EQ ownership changed during dispatch.")); }
            return true;
        }
        const bool queried = send(query);
        if (!guard || generation != m_generation) { return true; }
        if (!queried || (m_receiveEqPending.value(key, 0) == ticket && !current())) {
            abandon();
            if (guard && generation == m_generation) { emit extensionError(requestId, QStringLiteral("Receive EQ readback query failed or its context changed.")); }
            return true;
        }
    }
    if (select && guard && generation == m_generation && baseContext == receiveEqContextKey()) {
        const bool queriedEnable = send("EQR0");
        if (!guard || generation != m_generation) { return true; }
        if (!queriedEnable) { emit extensionError(requestId, QStringLiteral("Receive EQ enable readback query was not dispatched.")); return true; }
    }
    if (guard && generation == m_generation) {
        if (baseContext != receiveEqContextKey()) { emit extensionError(requestId, QStringLiteral("Receive EQ source context changed during handoff; radio outcome is unconfirmed.")); }
        else { emit extensionResult(requestId, true); }
    }
    return true;
}
} // namespace AetherSDR::kenwood
