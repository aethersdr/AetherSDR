#include "KenwoodBackend.h"

#include <QMetaType>
#include <QPointer>
#include <QScopeGuard>

namespace AetherSDR::kenwood {
namespace {
const QStringList kPresetBanks{QStringLiteral("slow"), QStringLiteral("mid"), QStringLiteral("fast")};
std::optional<int> agcDecimal(const QByteArray& bytes, int minimum, int maximum)
{
    int value = 0;
    for (char digit : bytes) {
        if (digit < '0' || digit > '9') { return {}; }
        value = value * 10 + digit - '0';
    }
    return value >= minimum && value <= maximum ? std::optional<int>(value) : std::nullopt;
}
QByteArray presetCommand(const std::array<int, 3>& raw)
{
    QByteArray command("GT");
    for (int value : raw) { command += QByteArray::number(value).rightJustified(2, '0'); }
    return command;
}
}

QString KenwoodBackend::receiveAgcContextKey() const
{
    return QStringLiteral("%1:%2:%3").arg(receiveControlContext()).arg(m_tune.has_value()).arg(m_tuneSerial);
}

bool KenwoodBackend::receiveAgcWritable() const
{
    return isConnected() && m_identityConfirmed && m_receiveVfo >= 0 && m_receiveVfo <= 1
        && m_frequency[m_receiveVfo] > 0 && encodeMode(m_mode).has_value()
        && m_mode != QLatin1String("FM") && m_mode != QLatin1String("DFM")
        && m_tfSet && !*m_tfSet && m_txStateKnown && !m_txObserved
        && !m_keyIntent && !m_cwIntent && !m_atuIntent && !m_tune
        && m_receiveAgcContext == receiveAgcContextKey();
}

void KenwoodBackend::resetReceiveAgc()
{
    ++m_receiveAgcRevision; ++m_receiveAgcVectorRevision; ++m_receiveAgcRestoreRevision;
    m_receiveAgcContext.clear();
    m_receiveAgcPresets.reset(); m_receiveAgcQuickEnabled.reset(); m_receiveAgcQuickThreshold.reset(); m_receiveAgcSelected.reset();
    m_receiveAgcPending.clear(); m_receiveAgcPredispatch.clear(); m_receiveAgcMinimumBatches.clear();
    m_receiveAgcInterested = m_receiveAgcQuerying = false;
    m_receiveAgcMinimumBatch = m_controlIngressSequence;
}

bool KenwoodBackend::refreshReceiveAgcContext(bool publish)
{
    const QString context = receiveAgcContextKey();
    if (m_receiveAgcContext == context) { return false; }
    m_receiveAgcContext = context;
    ++m_receiveAgcRevision; ++m_receiveAgcVectorRevision; ++m_receiveAgcRestoreRevision;
    m_receiveAgcPresets.reset(); m_receiveAgcQuickEnabled.reset(); m_receiveAgcQuickThreshold.reset(); m_receiveAgcSelected.reset();
    m_receiveAgcPending.clear(); m_receiveAgcPredispatch.clear(); m_receiveAgcMinimumBatches.clear();
    m_receiveAgcMinimumBatch = m_controlIngressSequence;
    if (publish) {
        const QPointer<KenwoodBackend> guard(this);
        const quint64 generation = m_generation;
        const quint64 revision = m_receiveAgcRevision;
        publishReceiveAgc();
        if (guard && generation == m_generation && revision == m_receiveAgcRevision) { queryReceiveAgc(); }
    }
    return true;
}

QVariantMap KenwoodBackend::receiveAgcStatus() const
{
    const bool current = m_receiveAgcContext == receiveAgcContextKey();
    const bool ready = receiveAgcWritable();
    const QString reason = ready ? QString{} : QStringLiteral("Requires confirmed non-FM receive source, mode, RX and TF-SET off; no local transmit intent.");
    QVariantMap presets{{"revision", QString::number(m_receiveAgcVectorRevision)}, {"pending", m_receiveAgcPending.contains("presets")}};
    if (current && m_receiveAgcPresets) {
        for (int bank = 0; bank < 3; ++bank) { presets.insert(kPresetBanks[bank], (*m_receiveAgcPresets)[bank]); }
    }
    QVariantMap operations;
    for (const QString& operation : {QStringLiteral("slow"), QStringLiteral("mid"), QStringLiteral("fast"),
        QStringLiteral("quickEnabled"), QStringLiteral("quickThreshold"), QStringLiteral("restore")}) {
        const bool preset = kPresetBanks.contains(operation);
        const QString key = preset ? QStringLiteral("presets") : operation;
        QString unavailable = reason;
        if (unavailable.isEmpty() && m_receiveAgcPending.contains(key)) { unavailable = QStringLiteral("Waiting for native AGC readback; last confirmed value retained."); }
        if (unavailable.isEmpty() && preset && !m_receiveAgcPresets) { unavailable = QStringLiteral("Requires all three fresh confirmed preset indices."); }
        if (unavailable.isEmpty() && operation == "quickEnabled" && !m_receiveAgcQuickEnabled) { unavailable = QStringLiteral("Waiting for quick recovery enable readback."); }
        if (unavailable.isEmpty() && operation == "quickThreshold" && !m_receiveAgcQuickThreshold) { unavailable = QStringLiteral("Waiting for quick recovery threshold readback."); }
        if (unavailable.isEmpty() && operation == "restore" && (!m_receiveAgcSelected || *m_receiveAgcSelected != 0)) {
            unavailable = QStringLiteral("Restore requires current confirmed AGC Off; the restored preset is determined by the radio.");
        }
        operations.insert(operation, QVariantMap{{"editable", unavailable.isEmpty()}, {"pending", m_receiveAgcPending.contains(key)}, {"reason", unavailable}});
    }
    QVariantMap status{{"context", QStringLiteral("%1:%2").arg(m_generation).arg(m_receiveAgcRevision)},
        {"statusRevision", QString::number(m_receiveAgcStatusRevision)}, {"mode", m_mode},
        {"editable", ready}, {"reason", reason}, {"presets", presets}, {"restoreRevision", QString::number(m_receiveAgcRestoreRevision)}, {"operations", operations}};
    if (current && m_receiveAgcQuickEnabled) { status.insert("quickEnabled", *m_receiveAgcQuickEnabled); }
    if (current && m_receiveAgcQuickThreshold) { status.insert("quickThreshold", *m_receiveAgcQuickThreshold); }
    if (current && m_receiveAgcSelected) { status.insert("selected", *m_receiveAgcSelected); }
    return status;
}

void KenwoodBackend::publishReceiveAgc()
{
    ++m_receiveAgcStatusRevision;
    emit extensionStatus(QStringLiteral("kenwood"), QStringLiteral("receiveAgc"), receiveAgcStatus());
}

void KenwoodBackend::queryReceiveAgc()
{
    if (!m_receiveAgcInterested || m_receiveAgcQuerying || !m_receiveAgcPredispatch.isEmpty() || !receiveAgcWritable()) { return; }
    const QPointer<KenwoodBackend> guard(this);
    const quint64 generation = m_generation;
    const quint64 revision = m_receiveAgcRevision;
    m_receiveAgcQuerying = true;
    const auto lifetime = qScopeGuard([guard, generation] { if (guard && generation == guard->m_generation) { guard->m_receiveAgcQuerying = false; } });
    for (const QByteArray& query : {QByteArray("GT"), QByteArray("AQ0"), QByteArray("AQ1"), QByteArray("GC")}) {
        if (!send(query) || !guard || generation != m_generation || revision != m_receiveAgcRevision || !receiveAgcWritable()) { return; }
    }
}

bool KenwoodBackend::adoptReceiveAgcSelection(int value, quint64 ingressBatch)
{
    const QPointer<KenwoodBackend> guard(this);
    const quint64 generation = m_generation;
    const bool contextChanged = refreshReceiveAgcContext(false);
    if (ingressBatch <= m_receiveAgcMinimumBatches.value(QStringLiteral("restore"), 0)) { return false; }
    const bool qualified = receiveAgcWritable() && ingressBatch > m_receiveAgcMinimumBatch;
    const bool same = m_receiveAgcSelected && *m_receiveAgcSelected == value;
    const bool beforeDispatch = m_receiveAgcPending.contains("restore")
        && m_receiveAgcPredispatch.value("restore", 0) == m_receiveAgcPending.value("restore");
    if (qualified && beforeDispatch && same) { return true; }
    ++m_receiveAgcRestoreRevision;
    if (qualified) { m_receiveAgcSelected = value; } else { m_receiveAgcSelected.reset(); }
    if (m_receiveAgcPending.contains("restore") && ((!same && beforeDispatch)
        || !qualified || ingressBatch > m_receiveAgcMinimumBatches.value("restore", 0))) {
        m_receiveAgcPending.remove("restore"); m_receiveAgcPredispatch.remove("restore");
    }
    const quint64 authority = m_receiveAgcRestoreRevision;
    const quint64 revision = m_receiveAgcRevision;
    publishReceiveAgc();
    if (!guard || generation != m_generation || authority != m_receiveAgcRestoreRevision || revision != m_receiveAgcRevision) { return false; }
    if (contextChanged) { queryReceiveAgc(); }
    return guard && generation == m_generation && authority == m_receiveAgcRestoreRevision && revision == m_receiveAgcRevision;
}

void KenwoodBackend::adoptReceiveAgc(const QByteArray& message, quint64 ingressBatch)
{
    const QPointer<KenwoodBackend> guard(this);
    const quint64 generation = m_generation;
    refreshReceiveAgcContext();
    if (!guard || generation != m_generation || !receiveAgcWritable() || ingressBatch <= m_receiveAgcMinimumBatch) { return; }
    QString key;
    std::optional<std::array<int, 3>> presets;
    std::optional<int> scalar;
    if (message.size() == 8 && message.startsWith("GT")) {
        std::array<int, 3> raw{};
        for (int bank = 0; bank < 3; ++bank) {
            const std::optional<int> value = agcDecimal(message.mid(2 + bank * 2, 2), 1, 20);
            if (!value) { return; } raw[bank] = *value;
        }
        presets = raw; key = QStringLiteral("presets");
    } else if (message.size() == 4 && message.startsWith("AQ0")) {
        scalar = agcDecimal(message.mid(3, 1), 1, 2); key = QStringLiteral("quickEnabled");
    } else if (message.size() == 5 && message.startsWith("AQ1")) {
        scalar = agcDecimal(message.mid(3, 2), 1, 10); key = QStringLiteral("quickThreshold");
    } else { return; }
    if (!presets && !scalar) { return; }
    if (ingressBatch <= m_receiveAgcMinimumBatches.value(key, 0)) { return; }
    const bool changed = presets ? m_receiveAgcPresets != presets
        : key == "quickEnabled" ? m_receiveAgcQuickEnabled != scalar : m_receiveAgcQuickThreshold != scalar;
    const bool beforeDispatch = m_receiveAgcPending.contains(key)
        && m_receiveAgcPredispatch.value(key, 0) == m_receiveAgcPending.value(key);
    if (beforeDispatch && !changed) { return; }
    if (presets) {
        if (changed) { ++m_receiveAgcVectorRevision; }
        m_receiveAgcPresets = presets;
    } else if (key == "quickEnabled") { m_receiveAgcQuickEnabled = scalar; }
    else { m_receiveAgcQuickThreshold = scalar; }
    m_receiveAgcPending.remove(key); m_receiveAgcPredispatch.remove(key);
    publishReceiveAgc();
}

bool KenwoodBackend::handleReceiveAgcExtension(const QString& verb, quint64 requestId, const QVariant& argument)
{
    const QStringList verbs{QStringLiteral("receive.agc.get"), QStringLiteral("receive.agc.preset.set"), QStringLiteral("receive.agc.preset.reset"),
        QStringLiteral("receive.agc.quick.enabled.set"), QStringLiteral("receive.agc.quick.threshold.set"),
        QStringLiteral("receive.agc.quick.threshold.reset"), QStringLiteral("receive.agc.restore")};
    if (!verbs.contains(verb)) { return false; }
    const QPointer<KenwoodBackend> guard(this);
    const quint64 generation = m_generation;
    const bool wasInterested = m_receiveAgcInterested;
    const bool changed = refreshReceiveAgcContext();
    if (!guard || generation != m_generation) { return true; }
    if (verb == "receive.agc.get") {
        if (argument.isValid() && (argument.typeId() != QMetaType::QVariantMap || !argument.toMap().isEmpty())) {
            emit extensionError(requestId, QStringLiteral("Native AGC query accepts no argument.")); return true;
        }
        m_receiveAgcInterested = true;
        if (!changed || !wasInterested) { queryReceiveAgc(); }
        if (guard && generation == m_generation) { emit extensionResult(requestId, receiveAgcStatus()); }
        return true;
    }
    const bool preset = verb.startsWith("receive.agc.preset.");
    const bool restore = verb == "receive.agc.restore";
    const bool reset = verb.endsWith(".reset");
    const QString key = preset ? QStringLiteral("presets") : restore ? QStringLiteral("restore")
        : verb == "receive.agc.quick.enabled.set" ? QStringLiteral("quickEnabled") : QStringLiteral("quickThreshold");
    const QVariantMap fields = argument.toMap();
    const QString bank = fields.value("bank").toString();
    const int index = kPresetBanks.indexOf(bank);
    const QString operation = preset ? bank : key;
    const QVariantMap state = receiveAgcStatus();
    const int expected = preset ? (reset ? 3 : 4) : restore ? 2 : reset ? 1 : 2;
    const QVariant value = fields.value("value");
    bool valid = argument.typeId() == QMetaType::QVariantMap && fields.size() == expected
        && fields.value("context").typeId() == QMetaType::QString && fields.value("context") == state.value("context")
        && state.value("operations").toMap().value(operation).toMap().value("editable").toBool();
    QByteArray command, query;
    if (preset) {
        valid = valid && fields.value("bank").typeId() == QMetaType::QString && index >= 0
            && fields.value("revision").typeId() == QMetaType::QString
            && fields.value("revision") == state.value("presets").toMap().value("revision") && m_receiveAgcPresets.has_value()
            && (reset || (value.typeId() == QMetaType::Int && value.toInt() >= 1 && value.toInt() <= 20));
        if (valid) { std::array<int, 3> raw = *m_receiveAgcPresets; raw[index] = reset ? 99 : value.toInt(); command = presetCommand(raw); }
        query = "GT";
    } else if (restore) {
        valid = valid && fields.value("restoreRevision").typeId() == QMetaType::QString && fields.value("restoreRevision") == state.value("restoreRevision");
        command = "GC4"; query = "GC";
    } else {
        const int maximum = key == "quickEnabled" ? 2 : 10;
        valid = valid && (reset || (value.typeId() == QMetaType::Int && value.toInt() >= 1 && value.toInt() <= maximum));
        query = key == "quickEnabled" ? "AQ0" : "AQ1";
        command = query + QByteArray::number(reset ? 99 : value.toInt()).rightJustified(key == "quickEnabled" ? 1 : 2, '0');
    }
    if (!valid) { emit extensionError(requestId, QStringLiteral("Native AGC request requires exact typed values and current editable readback ownership.")); return true; }
    const QString context = receiveAgcContextKey();
    const quint64 revision = m_receiveAgcRevision;
    const quint64 vectorRevision = m_receiveAgcVectorRevision;
    const quint64 restoreRevision = m_receiveAgcRestoreRevision;
    const quint64 ticket = ++m_receiveAgcTicket;
    m_receiveAgcPending.insert(key, ticket); m_receiveAgcPredispatch.insert(key, ticket);
    m_receiveAgcMinimumBatches.insert(key, m_controlIngressSequence);
    const auto current = [guard, generation, revision, context, preset, restore, vectorRevision, restoreRevision] {
        return guard && generation == guard->m_generation && revision == guard->m_receiveAgcRevision
            && context == guard->receiveAgcContextKey() && guard->receiveAgcWritable()
            && (!preset || vectorRevision == guard->m_receiveAgcVectorRevision)
            && (!restore || restoreRevision == guard->m_receiveAgcRestoreRevision);
    };
    const auto abandon = [guard, key, ticket](bool invalidate) {
        if (guard && guard->m_receiveAgcPending.value(key, 0) == ticket) {
            guard->m_receiveAgcPending.remove(key); guard->m_receiveAgcPredispatch.remove(key);
            if (invalidate) {
                if (key == "presets") { guard->m_receiveAgcPresets.reset(); ++guard->m_receiveAgcVectorRevision; }
                if (key == "quickEnabled") { guard->m_receiveAgcQuickEnabled.reset(); }
                if (key == "quickThreshold") { guard->m_receiveAgcQuickThreshold.reset(); }
                if (key == "restore") { guard->m_receiveAgcSelected.reset(); ++guard->m_receiveAgcRestoreRevision; }
            }
            guard->publishReceiveAgc();
        }
    };
    publishReceiveAgc();
    if (!current() || m_receiveAgcPending.value(key, 0) != ticket) {
        abandon(false);
        if (guard && generation == m_generation) { emit extensionError(requestId, QStringLiteral("Native AGC ownership changed before dispatch.")); }
        return true;
    }
    m_receiveAgcPredispatch.remove(key);
    m_receiveAgcMinimumBatches.insert(key, m_controlIngressSequence);
    const bool sent = send(command);
    if (!guard || generation != m_generation) { return true; }
    if (!sent) {
        abandon(true);
        if (guard && generation == m_generation) { emit extensionError(requestId, QStringLiteral("Native AGC setter was not dispatched; read settings again.")); }
        return true;
    }
    if (m_receiveAgcPending.value(key, 0) == ticket) {
        if (!current()) {
            abandon(false);
            if (guard && generation == m_generation) { emit extensionError(requestId, QStringLiteral("Native AGC ownership changed during dispatch.")); }
            return true;
        }
        const bool queried = send(query);
        if (!guard || generation != m_generation) { return true; }
        if (!queried || (m_receiveAgcPending.value(key, 0) == ticket && !current())) {
            abandon(!queried);
            if (guard && generation == m_generation) { emit extensionError(requestId, QStringLiteral("Native AGC readback failed or context changed; read settings again.")); }
            return true;
        }
    }
    if (context != receiveAgcContextKey()) { emit extensionError(requestId, QStringLiteral("Native AGC source changed during handoff; outcome unconfirmed.")); }
    else { emit extensionResult(requestId, true); }
    return true;
}
} // namespace AetherSDR::kenwood
