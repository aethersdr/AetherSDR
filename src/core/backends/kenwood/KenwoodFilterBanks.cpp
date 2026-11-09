#include "KenwoodBackend.h"

#include <QMetaType>
#include <QPointer>
#include <QStringList>
#include <algorithm>

namespace AetherSDR::kenwood {
namespace {
const QStringList kBankFields{QStringLiteral("roofing"), QStringLiteral("ifShape"), QStringLiteral("afType")};
QString bankId(int bank, const QString& field) { return QStringLiteral("bank%1.%2").arg(bank).arg(field); }
QByteArray bankQuery(const QString& id)
{
    if (id == QLatin1String("active")) { return "FL0"; }
    for (int bank = 0; bank < 3; ++bank) {
        for (int field = 0; field < kBankFields.size(); ++field) {
            if (id == bankId(bank, kBankFields[field])) { return "FL" + QByteArray::number(field + 1) + QByteArray::number(bank); }
        }
    }
    return {};
}
std::optional<int> decimal(const QByteArray& bytes)
{
    if (bytes.isEmpty()) { return {}; }
    int value = 0;
    for (char digit : bytes) {
        if (digit < '0' || digit > '9') { return {}; }
        value = value * 10 + digit - '0';
    }
    return value;
}
}

QString KenwoodBackend::filterBankContextKey() const
{
    return QStringLiteral("%1:%2:%3").arg(receiveControlContext()).arg(m_tune.has_value()).arg(m_tuneSerial);
}

bool KenwoodBackend::filterBanksWritable() const
{
    return isConnected() && m_identityConfirmed && m_receiveVfo >= 0 && m_receiveVfo <= 1
        && m_frequency[m_receiveVfo] > 0 && encodeMode(m_mode).has_value()
        && m_tfSet && !*m_tfSet && m_txStateKnown && !m_txObserved
        && !m_keyIntent && !m_cwIntent && !m_atuIntent && !m_tune
        && m_filterBankContext == filterBankContextKey();
}

void KenwoodBackend::resetFilterBanks()
{
    ++m_filterBankRevision;
    m_filterBankContext.clear();
    m_filterBankValues.clear();
    m_filterBankBandwidths.clear();
    m_filterBankPending.clear();
    m_filterBankMinimumBatches.clear();
    m_filterBankCount.reset();
    m_filter270Installed.reset();
    m_filterBanksInterested = false;
    m_filterBanksQueried = false;
    m_filterBankMinimumBatch = m_controlIngressSequence;
}

bool KenwoodBackend::refreshFilterBankContext()
{
    const QString context = filterBankContextKey();
    if (m_filterBankContext == context) { return false; }
    const bool retireAxes = m_filterBanksInterested || m_filterBankValues.contains(QStringLiteral("active")) || m_filterBankAxesPending;
    m_filterBankContext = context;
    ++m_filterBankRevision;
    m_filterBankValues.clear();
    m_filterBankBandwidths.clear();
    m_filterBankPending.clear();
    m_filterBankMinimumBatches.clear();
    m_filterBankCount.reset();
    m_filter270Installed.reset();
    m_filterBanksQueried = false;
    m_filterBankMinimumBatch = m_controlIngressSequence;
    if (retireAxes) {
        m_filterBankAxesAwaitingBank = true;
        m_filterBankAxesSetterDispatched = true;
        m_filterBankAxesPendingId = QStringLiteral("active");
        m_filterBankAxesPendingTicket = 0;
        m_filterBankAxesMinimumBatch = m_controlIngressSequence;
    }
    const QPointer<KenwoodBackend> guard(this);
    const quint64 generation = m_generation;
    const quint64 revision = m_filterBankRevision;
    if (retireAxes) {
        invalidateFilterBankAxes(false);
        if (!guard || generation != m_generation || revision != m_filterBankRevision) { return true; }
    }
    publishFilterBanks();
    if (guard && generation == m_generation && revision == m_filterBankRevision) { queryFilterBanks(); }
    return true;
}

QVariantMap KenwoodBackend::filterBankStatus() const
{
    const bool current = m_filterBankContext == filterBankContextKey();
    const bool ready = filterBanksWritable();
    QVariantList controls;
    for (int row = -1; row < 9; ++row) {
        const bool active = row == -1;
        const int bank = active ? -1 : row / 3;
        const int field = active ? -1 : row % 3;
        const QString id = active ? QStringLiteral("active") : bankId(bank, kBankFields[field]);
        QString reason;
        if (!ready) { reason = QStringLiteral("Requires confirmed receive source, mode, RX and TF-SET off."); }
        else if (id != QLatin1String("active") && m_filterBankAxesAwaitingBank
            && m_filterBankAxesPendingId == QLatin1String("active")) { reason = QStringLiteral("Waiting for active receive filter confirmation before editing any bank."); }
        else if (!m_filterBankValues.contains(QStringLiteral("active"))) { reason = QStringLiteral("Waiting for current active receive filter readback."); }
        else if (bank == 2 && (!m_filterBankCount || *m_filterBankCount != 3)) { reason = QStringLiteral("Filter C requires confirmed three-filter menu configuration."); }
        else if (!active && field < 2 && (m_mode == QLatin1String("FM") || m_mode == QLatin1String("DFM"))) { reason = QStringLiteral("Roofing and IF shape are read-only in FM."); }
        else if (!current || !m_filterBankValues.contains(id)) { reason = QStringLiteral("Waiting for current bank readback."); }
        else if (m_filterBankPending.contains(id)) { reason = QStringLiteral("Waiting for radio readback; last confirmed value retained."); }
        QVariantList choices;
        const QStringList labels = active ? QStringList{"A", "B", "C"}
            : field == 0 ? QStringList{"Auto", "270 Hz", "500 Hz", "2.7 kHz", "6 kHz", "15 kHz"}
            : field == 1 ? QStringList{"Sharp", "Medium", "Soft", "None (FM)"}
                         : QStringList{"Narrow", "Medium", "Wide"};
        for (int choice = 0; choice < labels.size(); ++choice) {
            QString choiceReason = reason;
            if (choiceReason.isEmpty() && active && choice == 2 && (!m_filterBankCount || *m_filterBankCount != 3)) {
                choiceReason = QStringLiteral("Filter C requires confirmed three-filter menu configuration.");
            }
            if (choiceReason.isEmpty() && !active && field == 0 && choice == 1
                && (!m_filter270Installed || !*m_filter270Installed)) {
                choiceReason = QStringLiteral("270 Hz requires confirmed installed optional roofing filter.");
            }
            if (choiceReason.isEmpty() && !active && field == 1 && choice == 3) {
                choiceReason = QStringLiteral("None is an FM observation, not a writable shape.");
            }
            choices.append(QVariantMap{{"value", choice}, {"label", labels[choice]},
                {"editable", choiceReason.isEmpty()}, {"reason", choiceReason}});
        }
        const QString label = active ? QStringLiteral("Active receive filter")
            : QStringLiteral("Filter %1 %2").arg(QChar('A' + bank),
                field == 0 ? QStringLiteral("roofing") : field == 1 ? QStringLiteral("IF shape") : QStringLiteral("AF type"));
        QVariantMap control{{"id", id}, {"label", label}, {"editable", reason.isEmpty()},
            {"reason", reason}, {"choices", choices}, {"resettable", !active},
            {"pending", m_filterBankPending.contains(id)}};
        if (current && m_filterBankValues.contains(id)) { control.insert("value", m_filterBankValues.value(id)); }
        if (!active && field == 0 && current && m_filterBankBandwidths.contains(id)) {
            control.insert("actualBandwidthHz", m_filterBankBandwidths.value(id));
        }
        controls.append(control);
    }
    QVariantMap status{{"statusRevision", QString::number(m_filterBankStatusRevision)}, {"context", QStringLiteral("%1:%2").arg(m_generation).arg(m_filterBankRevision)},
        {"mode", m_mode}, {"controls", controls}};
    if (current && m_filterBankCount) { status.insert("bankCount", *m_filterBankCount); }
    if (current && m_filter270Installed) { status.insert("option270Installed", *m_filter270Installed); }
    return status;
}

void KenwoodBackend::publishFilterBanks()
{
    ++m_filterBankStatusRevision;
    emit extensionStatus(QStringLiteral("kenwood"), QStringLiteral("filterBanks"), filterBankStatus());
}

void KenwoodBackend::queryFilterBanks()
{
    if ((!m_filterBanksInterested && !m_filterBankAxesAwaitingBank) || m_filterBanksQueried || !filterBanksWritable()) { return; }
    if (m_filterBankAxesAwaitingBank && !m_filterBankAxesSetterDispatched
        && m_filterBankAxesPendingTicket != 0
        && m_filterBankPending.value(m_filterBankAxesPendingId, 0) == m_filterBankAxesPendingTicket) { return; }
    m_filterBanksQueried = true;
    const QPointer<KenwoodBackend> guard(this);
    const quint64 generation = m_generation;
    const quint64 revision = m_filterBankRevision;
    QList<QByteArray> queries{"EX00610", "FL0"};
    const int count = m_filterBankCount.value_or(2);
    for (int bank = 0; bank < count; ++bank) {
        for (int field = 1; field <= 3; ++field) { queries.append("FL" + QByteArray::number(field) + QByteArray::number(bank)); }
    }
    for (const QByteArray& query : queries) {
        const bool sent = send(query);
        if (!guard || generation != m_generation || revision != m_filterBankRevision || !filterBanksWritable()) { return; }
        if (!sent) { m_filterBanksQueried = false; return; }
    }
}

void KenwoodBackend::invalidateFilterBankAxes(bool query)
{
    ++m_filterRevision;
    m_filterBankAxesPending = true;
    m_filterBankAxesMinimumBatch = m_controlIngressSequence;
    m_filterLowIndex = m_filterHighIndex = -1;
    SliceDelta delta;
    delta.receiveFilterContextChanged = true;
    const QPointer<KenwoodBackend> guard(this);
    const quint64 generation = m_generation;
    const quint64 revision = m_filterRevision;
    emit sliceChanged(0, delta);
    if (!guard || generation != m_generation || revision != m_filterRevision) { return; }
    emit capabilitiesChanged();
    if (!guard || generation != m_generation || revision != m_filterRevision) { return; }
    publishNativeFilter();
    if (query && !m_filterBankAxesAwaitingBank && guard && generation == m_generation && revision == m_filterRevision) { queryFilter(); }
}

void KenwoodBackend::adoptFilterBank(const QByteArray& message, quint64 ingressBatch)
{
    const QPointer<KenwoodBackend> guard(this);
    const quint64 generation = m_generation;
    refreshFilterBankContext();
    if (!guard || generation != m_generation || !filterBanksWritable()
        || ingressBatch <= m_filterBankMinimumBatch) { return; }
    if (message == "EX00610 002" || message == "EX00610 003") {
        const int count = message.back() - '0';
        if (m_filterBankCount == count) { return; }
        m_filterBankCount = count;
        ++m_filterBankRevision;
        m_filterBankPending.clear();
        const bool invalidActive = count == 2 && m_filterBankValues.value(QStringLiteral("active"), -1) == 2;
        if (invalidActive) {
            m_filterBankValues.remove(QStringLiteral("active"));
            m_filterBankAxesAwaitingBank = true;
            m_filterBankAxesSetterDispatched = true;
            m_filterBankAxesPendingId = QStringLiteral("active");
            m_filterBankAxesPendingTicket = 0;
            invalidateFilterBankAxes(false);
            if (!guard || generation != m_generation) { return; }
        }
        if (count == 2) {
            for (const QString& field : kBankFields) {
                m_filterBankValues.remove(bankId(2, field));
                m_filterBankBandwidths.remove(bankId(2, field));
            }
        }
        m_filterBanksQueried = false;
        const quint64 revision = m_filterBankRevision;
        publishFilterBanks();
        if (guard && generation == m_generation && revision == m_filterBankRevision) { queryFilterBanks(); }
        return;
    }
    QString id;
    int value = -1;
    std::optional<int> bandwidth;
    std::optional<bool> option;
    if (message.size() == 5 && message.startsWith("FL0")
        && message[3] >= '0' && message[3] <= '2' && (message[4] == '0' || message[4] == '1')) {
        id = QStringLiteral("active"); value = message[3] - '0'; option = message[4] == '1';
        if (value == 2 && m_filterBankCount && *m_filterBankCount == 2) { return; }
    } else if ((message.size() == 9 && message.startsWith("FL1"))
        || (message.size() == 5 && (message.startsWith("FL2") || message.startsWith("FL3")))) {
        const int bank = message[3] - '0';
        const int field = message[2] - '1';
        if (bank < 0 || bank > 2 || (bank == 2 && (!m_filterBankCount || *m_filterBankCount != 3))) { return; }
        value = message[4] - '0';
        const int maximum = field == 0 ? 5 : field == 1 ? 3 : 2;
        if (value < 0 || value > maximum || (field == 1 && value == 3 && m_mode != "FM" && m_mode != "DFM")) { return; }
        id = bankId(bank, kBankFields[field]);
        if (field == 0) {
            const std::optional<int> raw = decimal(message.mid(5, 4));
            if (!raw || *raw < 27 || *raw > 1500) { return; }
            bandwidth = *raw * 10;
        }
    } else { return; }
    if (ingressBatch <= m_filterBankMinimumBatches.value(id, 0)) { return; }
    const bool changed = m_filterBankValues.contains(id) && m_filterBankValues.value(id) != value;
    const bool activeChanged = id == "active" && (!m_filterBankValues.contains(id) || changed);
    const bool optionChanged = option && m_filter270Installed != option;
    const bool ownsPredispatch = m_filterBankAxesAwaitingBank && !m_filterBankAxesSetterDispatched
        && id == m_filterBankAxesPendingId && m_filterBankAxesPendingTicket != 0
        && m_filterBankPending.value(id, 0) == m_filterBankAxesPendingTicket;
    if (ownsPredispatch && !changed && !optionChanged) { return; }
    const bool supersedesPredispatch = ownsPredispatch && (changed || optionChanged);
    const bool confirmsAffectingRequest = m_filterBankAxesAwaitingBank && m_filterBankAxesSetterDispatched
        && id == m_filterBankAxesPendingId
        && (m_filterBankPending.value(id, 0) == m_filterBankAxesPendingTicket || !m_filterBankPending.contains(id));
    m_filterBankValues.insert(id, value);
    m_filterBankPending.remove(id);
    if (bandwidth) { m_filterBankBandwidths.insert(id, *bandwidth); }
    if (option) { m_filter270Installed = option; }
    if (activeChanged || changed || optionChanged) {
        ++m_filterBankRevision;
        m_filterBankPending.clear();
    }
    const quint64 revision = m_filterBankRevision;
    const int activeBank = m_filterBankValues.value(QStringLiteral("active"), -1);
    if (confirmsAffectingRequest || activeChanged || supersedesPredispatch) {
        m_filterBankAxesAwaitingBank = false;
        m_filterBankAxesSetterDispatched = true;
        m_filterBankAxesPendingId.clear();
        m_filterBankAxesPendingTicket = 0;
    }
    if (confirmsAffectingRequest || activeChanged || supersedesPredispatch || (changed && id.startsWith(QStringLiteral("bank%1.").arg(activeBank)))) {
        invalidateFilterBankAxes();
        if (!guard || generation != m_generation || revision != m_filterBankRevision) { return; }
    }
    if (activeChanged || optionChanged) { m_filterBanksQueried = false; }
    publishFilterBanks();
    if (guard && generation == m_generation && revision == m_filterBankRevision) { queryFilterBanks(); }
}

bool KenwoodBackend::handleFilterBankExtension(const QString& verb, quint64 requestId, const QVariant& argument)
{
    if (verb != "filter.banks.get" && verb != "filter.banks.set" && verb != "filter.banks.reset") { return false; }
    const QPointer<KenwoodBackend> guard(this);
    const quint64 generation = m_generation;
    const bool wasInterested = m_filterBanksInterested;
    const bool changed = refreshFilterBankContext();
    if (!guard || generation != m_generation) { return true; }
    if (verb == "filter.banks.get") {
        if (argument.isValid()) { emit extensionError(requestId, QStringLiteral("Filter bank query accepts no argument.")); return true; }
        m_filterBanksInterested = true;
        if (!changed) { m_filterBanksQueried = false; }
        if (!changed || !wasInterested) { queryFilterBanks(); }
        if (guard && generation == m_generation) { emit extensionResult(requestId, filterBankStatus()); }
        return true;
    }
    const QVariantMap fields = argument.toMap();
    const bool reset = verb == "filter.banks.reset";
    const QString id = fields.value("id").toString();
    const QVariant requested = fields.value("value");
    const QByteArray query = bankQuery(id);
    QVariantMap control;
    for (const QVariant& row : filterBankStatus().value("controls").toList()) {
        if (row.toMap().value("id").toString() == id) { control = row.toMap(); break; }
    }
    bool admitted = false;
    if (reset) { admitted = control.value("resettable").toBool(); }
    else if (requested.typeId() == QMetaType::Int) {
        for (const QVariant& choice : control.value("choices").toList()) {
            const QVariantMap item = choice.toMap();
            if (item.value("value") == requested && item.value("editable").toBool()) { admitted = true; }
        }
    }
    if (argument.typeId() != QMetaType::QVariantMap || fields.size() != (reset ? 2 : 3)
        || fields.value("context").typeId() != QMetaType::QString || fields.value("id").typeId() != QMetaType::QString
        || fields.value("context") != filterBankStatus().value("context") || query.isEmpty()
        || !control.value("editable").toBool() || !admitted) {
        emit extensionError(requestId, QStringLiteral("Filter bank request requires current editable readback and a supported choice.")); return true;
    }
    const quint64 revision = m_filterBankRevision;
    const quint64 ticket = ++m_filterBankTicket;
    m_filterBankPending.insert(id, ticket);
    m_filterBankMinimumBatches.insert(id, m_controlIngressSequence);
    const auto current = [guard, generation, revision] {
        return guard && generation == guard->m_generation && revision == guard->m_filterBankRevision && guard->filterBanksWritable();
    };
    const auto abandon = [guard, id, ticket] {
        if (guard && guard->m_filterBankPending.value(id, 0) == ticket) {
            guard->m_filterBankPending.remove(id); guard->publishFilterBanks();
        }
    };
    const int activeBank = m_filterBankValues.value(QStringLiteral("active"), -1);
    const bool affectsAxes = id == QLatin1String("active") || id.startsWith(QStringLiteral("bank%1.").arg(activeBank));
    if (affectsAxes) {
        m_filterBankAxesAwaitingBank = true;
        m_filterBankAxesSetterDispatched = false;
        m_filterBankAxesPendingId = id;
        m_filterBankAxesPendingTicket = ticket;
        invalidateFilterBankAxes(false);
        if (!current()) {
            abandon();
            if (guard && generation == m_generation) { emit extensionError(requestId, QStringLiteral("Filter bank context changed before axis retirement completed.")); }
            return true;
        }
    }
    publishFilterBanks();
    if (!current() || m_filterBankPending.value(id, 0) != ticket) {
        abandon();
        if (guard && generation == m_generation) { emit extensionError(requestId, QStringLiteral("Filter bank context changed before dispatch.")); }
        return true;
    }
    if (affectsAxes && m_filterBankAxesPendingTicket == ticket && m_filterBankAxesPendingId == id) {
        m_filterBankAxesSetterDispatched = true;
    }
    const bool sent = send(query + QByteArray::number(reset ? 9 : requested.toInt()));
    if (!current() || !sent) {
        abandon();
        if (guard && generation == m_generation) { emit extensionError(requestId, QStringLiteral("Filter bank context changed or setter dispatch failed.")); }
        return true;
    }
    if (m_filterBankPending.value(id, 0) == ticket) {
        const bool queried = send(query);
        if (!current() || !queried) {
            abandon();
            if (guard && generation == m_generation) { emit extensionError(requestId, QStringLiteral("Filter bank readback context changed or query failed.")); }
            return true;
        }
    }
    if (current()) { emit extensionResult(requestId, true); }
    return true;
}
} // namespace AetherSDR::kenwood

namespace AetherSDR::kenwood {
namespace {
bool fmSequence(const QVariant& value)
{
    if (value.typeId() != QMetaType::QString || value.toString().isEmpty() || value.toString().size() > 20) { return false; }
    bool ok = false;
    const quint64 number = value.toString().toULongLong(&ok);
    return ok && QString::number(number) == value.toString();
}
bool fmMode(const QString& mode) { return mode == QLatin1String("FM") || mode == QLatin1String("DFM"); }
}

void KenwoodBackend::resetFmWidth()
{
    ++m_fmWidthEpoch; ++m_fmWidthContextEpoch;
    m_fmWidthHanded = false; m_fmWidthWriteTicket = 0;
    m_fmWidthFacts.clear(); m_fmWidthOwned.clear(); m_fmWidthBatches.clear();
    m_fmWidthReading = m_fmWidthDispatched = m_fmWidthWritePending = false;
    m_fmWidthQueue.clear(); m_fmWidthPending.clear(); m_fmWidthSignature.clear(); m_fmWidthReadToken.clear();
    m_fmWidthReadReason = QStringLiteral("Explicitly read FM Normal/Narrow.");
    m_fmWidthLastWrite.clear(); m_fmWidthDirty = true;
}

void KenwoodBackend::refreshFmWidthSafety()
{
    // The session-global receive epoch also fences local-intent ABA between callbacks.
    const QString signature = QStringLiteral("%1:%2:%3:%4:%5:%6:%7:%8:%9:%10")
        .arg(m_generation).arg(isConnected()).arg(m_identityConfirmed).arg(m_txStateKnown).arg(m_txObserved)
        .arg(m_keyIntent).arg(m_cwIntent).arg(m_atuIntent).arg(m_tune ? m_tune->serial : 0).arg(m_receiverOptionsEpoch);
    if (signature == m_fmWidthSignature) { return; }
    m_fmWidthSignature = signature; ++m_fmWidthEpoch; ++m_fmWidthContextEpoch;
    m_fmWidthOwned.clear(); m_fmWidthReading = false; m_fmWidthPending.clear(); m_fmWidthQueue.clear();
    m_fmWidthReadReason = QStringLiteral("Session or receive safety changed; explicitly Read again.");
    if (m_fmWidthWritePending) {
        m_fmWidthLastWrite.insert("state", QStringLiteral("indeterminate"));
        m_fmWidthLastWrite.insert("reason", m_fmWidthReadReason);
        m_fmWidthWritePending = false; m_fmWidthWriteTicket = 0;
    }
    m_fmWidthDirty = true;
}

bool KenwoodBackend::fmWidthEligible(int display) const
{
    if (!isConnected() || !m_identityConfirmed || !fmMode(m_fmWidthFacts.value(display ? "OM1" : "OM0"))) { return false; }
    return display == 0 || (m_fmWidthFacts.value("TS") == "1"
        || (m_fmWidthFacts.contains("FR") && m_fmWidthFacts.contains("FT") && m_fmWidthFacts.value("FR") != m_fmWidthFacts.value("FT")));
}

QString KenwoodBackend::fmWidthUnavailable() const
{
    if (!isConnected() || !m_identityConfirmed) { return QStringLiteral("Requires a connected identified TS-890S."); }
    if (m_fmWidthReading || m_fmWidthWritePending) { return QStringLiteral("Waiting for explicit native width work."); }
    if (!m_txStateKnown || m_txObserved || m_keyIntent || m_cwIntent || m_atuIntent || m_tune
        || m_fmWidthFacts.value("TS") != "0" || !m_tfSet || *m_tfSet) { return QStringLiteral("Apply requires confirmed RX and TF-SET off, without local transmit/tune intent."); }
    const QStringList required{"FR", "FT", "TS", "MV", "QR", "SF0", "SF1", "OM0", "FW0"};
    for (const QString& key : required) {
        if (!m_fmWidthOwned.contains(key)) { return QStringLiteral("Explicitly Read the current physical receive source and width before Apply."); }
    }
    const int receive = m_fmWidthFacts.value("FR").toInt();
    const std::optional<VfoState> source = decodeVfoState(m_fmWidthFacts.value(receive ? "SF1" : "SF0").toLatin1());
    if (m_fmWidthFacts.value("MV") != "0" || !m_fmWidthFacts.value("QR").startsWith("0")
        || receive != m_receiveVfo || !source || source->frequencyHz != m_frequency[receive]
        || source->mode != m_mode || m_fmWidthFacts.value("OM0") != m_mode
        || m_fmWidthFacts.value("FT").toInt() != m_transmitVfo || !fmWidthEligible(0)) {
        return QStringLiteral("Requires coherent current physical VFO FM/FM-D receive ownership; memory is excluded.");
    }
    return {};
}

QVariantMap KenwoodBackend::fmWidthStatus() const
{
    const bool connected = isConnected() && m_identityConfirmed;
    QVariantList displays;
    for (int display = 0; display < 2; ++display) {
        const QString key = display ? QStringLiteral("FW1") : QStringLiteral("FW0");
        const bool eligible = fmWidthEligible(display);
        const bool known = connected && eligible && m_fmWidthFacts.contains(key);
        QVariantMap row{{"display", display}, {"eligible", eligible}, {"known", known},
            {"reason", eligible ? QStringLiteral("Last native observation; no passband or deviation inferred.")
                : QStringLiteral("Requires FM/FM-D on this display; right display also requires split or TF-SET.")}};
        if (known) { row.insert("value", m_fmWidthFacts.value(key).toInt()); }
        displays.append(row);
    }
    const QString reason = fmWidthUnavailable();
    return {{"schemaVersion", 1}, {"statusRevision", QString::number(m_fmWidthStatusRevision)},
        {"context", QStringLiteral("fm:%1:%2").arg(m_generation).arg(m_fmWidthEpoch)},
        {"revision", QString::number(m_fmWidthEpoch)},
        {"connected", connected}, {"editable", reason.isEmpty()}, {"reason", reason}, {"displays", displays},
        {"refresh", QVariantMap{{"token", m_fmWidthReadToken}, {"busy", m_fmWidthReading},
            {"pendingKey", m_fmWidthPending}, {"reason", m_fmWidthReadReason}}},
        {"lastWrite", m_fmWidthLastWrite}};
}

void KenwoodBackend::publishFmWidth()
{
    m_fmWidthDirty = false; ++m_fmWidthStatusRevision;
    const QVariantMap snapshot = fmWidthStatus();
    emit extensionStatus(QStringLiteral("kenwood"), QStringLiteral("fmWidth"), snapshot);
}

void KenwoodBackend::beginFmWidthRead()
{
    m_fmWidthOwned.clear(); m_fmWidthQueue = {"FR", "FT", "TS", "MV", "QR", "SF0", "SF1", "OM0", "OM1", "FW0", "FW1"};
    m_fmWidthReading = true; m_fmWidthPending.clear(); m_fmWidthDispatched = false;
    m_fmWidthReadToken = QString::number(++m_fmWidthSerial);
    m_fmWidthReadReason = QStringLiteral("Reading display roles, FM mode and Normal/Narrow; no setting changed.");
    m_fmWidthDirty = true;
}

void KenwoodBackend::advanceFmWidthRead()
{
    if (!m_fmWidthReading || !m_fmWidthPending.isEmpty()) { return; }
    while (!m_fmWidthQueue.isEmpty()) {
        const QString next = m_fmWidthQueue.takeFirst();
        if ((next == "FW0" && !fmWidthEligible(0)) || (next == "FW1" && !fmWidthEligible(1))) { continue; }
        m_fmWidthPending = next; m_fmWidthDispatched = false;
        m_fmWidthMinimumBatch = std::max(m_controlIngressSequence, m_fmWidthBatches.value(next));
        const quint64 serial = ++m_fmWidthSerial;
        m_fmWidthPendingTicket = serial;
        const quint64 generation = m_generation;
        const QPointer<KenwoodBackend> guard(this);
        publishFmWidth();
        if (!guard || generation != m_generation || !m_fmWidthReading || serial != m_fmWidthPendingTicket || m_fmWidthPending != next) { return; }
        refreshFmWidthSafety();
        if (!m_fmWidthReading || serial != m_fmWidthPendingTicket) { return; }
        m_fmWidthMinimumBatch = std::max(m_fmWidthMinimumBatch, m_controlIngressSequence);
        m_fmWidthDispatched = true;
        m_fmWidthDeadline = (m_clock.isValid() ? m_clock.elapsed() : 0) + 1500;
        const bool sent = send(next.toLatin1());
        if (!guard || generation != m_generation) { return; }
        if (!sent && serial == m_fmWidthPendingTicket) {
            m_fmWidthReading = false; m_fmWidthOwned.clear(); m_fmWidthPending.clear(); m_fmWidthQueue.clear();
            m_fmWidthReadReason = QStringLiteral("Native query refused; explicitly Read again.");
            if (m_fmWidthWritePending) {
                m_fmWidthWritePending = false; m_fmWidthWriteTicket = 0;
                m_fmWidthLastWrite.insert("state", QStringLiteral("sent_unverified"));
                m_fmWidthLastWrite.insert("reason", QStringLiteral("Readback query refused; no retry or inverse."));
            }
            m_fmWidthDirty = true;
        }
        return;
    }
    m_fmWidthReading = false;
    m_fmWidthReadReason = QStringLiteral("Read finished. Display observations are not hardware effect verification.");
    if (m_fmWidthWritePending) {
        const bool consistent = m_fmWidthOwned.contains("FW0") && m_fmWidthFacts.value("FW0").toInt() == m_fmWidthRequested;
        m_fmWidthLastWrite.insert("state", consistent ? QStringLiteral("readback_consistent") : QStringLiteral("sent_unverified"));
        m_fmWidthLastWrite.insert("reason", QStringLiteral("Later readback is observational, not a causal acknowledgement."));
        m_fmWidthWritePending = false;
    }
    m_fmWidthDirty = true;
}

void KenwoodBackend::adoptFmWidth(const QByteArray& message, quint64 batch)
{
    refreshFmWidthSafety();
    if (message == "?" && m_fmWidthReading && m_fmWidthDispatched && batch > m_fmWidthMinimumBatch) {
        m_fmWidthReading = false; m_fmWidthPending.clear(); m_fmWidthQueue.clear(); m_fmWidthOwned.clear();
        m_fmWidthReadReason = QStringLiteral("Radio error during owned width read; no retry.");
        if (m_fmWidthWritePending) { m_fmWidthWritePending = false; m_fmWidthLastWrite.insert("state", QStringLiteral("sent_unverified")); }
        m_fmWidthDirty = true; return;
    }
    QString key; QString value; bool valid = false;
    for (const QString& scalar : {QStringLiteral("FR"), QStringLiteral("FT"), QStringLiteral("TS"), QStringLiteral("MV"), QStringLiteral("QR")}) {
        if (message.startsWith(scalar.toLatin1())) {
            key = scalar; valid = message.size() == 3 && (message[2] == '0' || message[2] == '1');
            if (valid) { value = QString(QChar(message[2])); } break;
        }
    }
    if (message.startsWith("QR")) {
        key = "QR";
        valid = message.size() == 4 && (message[2] == '0' || message[2] == '1')
            && (message[3] == ' ' || (message[3] >= '0' && message[3] <= '9'));
        if (valid) { value = QString::fromLatin1(message.mid(2)); }
    }
    if (message.startsWith("OM0") || message.startsWith("OM1")) {
        key = QString::fromLatin1(message.left(3));
        const std::optional<QString> mode = message.size() == 4 ? decodeMode(message[3]) : std::nullopt;
        valid = mode.has_value(); if (mode) { value = *mode; }
    } else if (message.startsWith("SF0") || message.startsWith("SF1")) {
        key = QString::fromLatin1(message.left(3));
        valid = decodeVfoState(message).has_value(); if (valid) { value = QString::fromLatin1(message); }
    } else if (message.startsWith("FW")) {
        if (message.size() < 3 || (message[2] != '0' && message[2] != '1')) { return; }
        key = QString::fromLatin1(message.left(3));
        valid = message.size() == 4 && (message[3] == '0' || message[3] == '1');
        if (valid) { value = QString(QChar(message[3])); }
    }
    if (message.startsWith("FA") || message.startsWith("FB")) {
        key = QString::fromLatin1(message.left(2));
        const std::optional<qint64> frequency = decodeFrequency(message);
        valid = frequency.has_value(); if (frequency) { value = QString::number(*frequency); }
    }
    if (key.isEmpty() || batch <= m_fmWidthBatches.value(key)) { return; }
    const bool matching = m_fmWidthReading && key == m_fmWidthPending;
    if (matching && batch <= m_fmWidthMinimumBatch) { return; }
    m_fmWidthBatches[key] = batch;
    const bool owned = matching && m_fmWidthDispatched;
    const bool changed = !valid || !m_fmWidthFacts.contains(key) || m_fmWidthFacts.value(key) != value;
    if (changed) {
        ++m_fmWidthEpoch;
        if (!key.startsWith("FW")) {
            ++m_fmWidthContextEpoch;
            if (m_fmWidthWritePending) {
                m_fmWidthWritePending = false; m_fmWidthWriteTicket = 0;
                m_fmWidthLastWrite.insert("state", m_fmWidthHanded ? QStringLiteral("indeterminate") : QStringLiteral("rejected"));
                m_fmWidthLastWrite.insert("reason", QStringLiteral("Native operating context changed during width intent."));
            }
            m_fmWidthOwned.remove("FW0"); m_fmWidthOwned.remove("FW1");
            m_fmWidthFacts.remove("FW0"); m_fmWidthFacts.remove("FW1");
        } else { m_fmWidthOwned.remove(key); }
        if (!owned && m_fmWidthReading) {
            m_fmWidthReading = false; m_fmWidthPending.clear(); m_fmWidthQueue.clear(); m_fmWidthOwned.clear();
            m_fmWidthReadReason = QStringLiteral("Native context changed outside the owned read; explicitly Read again.");
        }
        if (!owned && m_fmWidthWritePending && !(m_fmWidthHanded && key == "FW0" && valid && value.toInt() == m_fmWidthRequested)) {
            m_fmWidthWritePending = false; m_fmWidthWriteTicket = 0; m_fmWidthLastWrite.insert("state", QStringLiteral("indeterminate"));
            m_fmWidthLastWrite.insert("reason", QStringLiteral("Native context changed during width intent."));
        }
    }
    if (valid) {
        m_fmWidthFacts[key] = value;
        if (key == "SF0" || key == "SF1") {
            const VfoState state = *decodeVfoState(message);
            const QString frequencyKey = state.vfo ? QStringLiteral("FB") : QStringLiteral("FA");
            m_fmWidthFacts[frequencyKey] = QString::number(state.frequencyHz);
            m_fmWidthBatches[frequencyKey] = std::max(m_fmWidthBatches.value(frequencyKey), batch);
        }
    } else { m_fmWidthFacts.remove(key); m_fmWidthOwned.remove(key); }
    if (owned) {
        if (valid) { m_fmWidthOwned.insert(key); }
        else {
            m_fmWidthOwned.clear(); m_fmWidthReading = false; m_fmWidthQueue.clear();
            m_fmWidthReadReason = QStringLiteral("Malformed native reply; explicitly Read again.");
            if (m_fmWidthWritePending) {
                m_fmWidthWritePending = false; m_fmWidthWriteTicket = 0;
                m_fmWidthLastWrite.insert("state", QStringLiteral("sent_unverified"));
                m_fmWidthLastWrite.insert("reason", QStringLiteral("Malformed readback; no retry or inverse."));
            }
        }
        m_fmWidthPending.clear(); m_fmWidthDispatched = false;
    }
    m_fmWidthDirty = true;
}

void KenwoodBackend::pollFmWidth(qint64 nowMs)
{
    refreshFmWidthSafety();
    if (m_fmWidthReading && !m_fmWidthPending.isEmpty() && nowMs >= m_fmWidthDeadline && m_fmWidthDispatched) {
        m_fmWidthReading = false; m_fmWidthPending.clear(); m_fmWidthQueue.clear(); m_fmWidthOwned.clear();
        m_fmWidthReadReason = QStringLiteral("Native read timed out; no retry or automatic inverse.");
        if (m_fmWidthWritePending) { m_fmWidthWritePending = false; m_fmWidthLastWrite.insert("state", QStringLiteral("sent_unverified")); }
        m_fmWidthDirty = true;
    }
    if (m_fmWidthDirty) { publishFmWidth(); }
}

bool KenwoodBackend::handleFmWidthExtension(const QString& verb, quint64 requestId, const QVariant& argument)
{
    if (!QStringList{"filter.fm.get", "filter.fm.read", "filter.fm.cancel", "filter.fm.set"}.contains(verb)) { return false; }
    const QPointer<KenwoodBackend> guard(this); const quint64 generation = m_generation;
    refreshFmWidthSafety();
    const QVariantMap args = argument.toMap();
    if (verb == "filter.fm.get") {
        if (argument.isValid()) { emit extensionError(requestId, QStringLiteral("Cached width get takes no argument.")); }
        else { ++m_fmWidthStatusRevision; emit extensionResult(requestId, fmWidthStatus()); } return true;
    }
    if (verb == "filter.fm.cancel") {
        if (argument.typeId() != QMetaType::QVariantMap || args.size() != 1 || !fmSequence(args.value("token")) || args.value("token") != m_fmWidthReadToken || !m_fmWidthReading) {
            emit extensionError(requestId, QStringLiteral("Cancel requires the current read token.")); return true;
        }
        m_fmWidthReading = false; m_fmWidthPending.clear(); m_fmWidthQueue.clear(); m_fmWidthOwned.clear(); ++m_fmWidthEpoch;
        m_fmWidthWritePending = false; m_fmWidthWriteTicket = 0; m_fmWidthReadReason = QStringLiteral("Cancelled; native facts retained without Apply authority.");
        if (!m_fmWidthLastWrite.isEmpty()) { m_fmWidthLastWrite.insert("state", QStringLiteral("sent_unverified")); }
        publishFmWidth(); if (guard && generation == m_generation) { emit extensionResult(requestId, fmWidthStatus()); } return true;
    }
    if (verb == "filter.fm.read") {
        if (argument.isValid() || !isConnected() || !m_identityConfirmed || m_fmWidthReading || m_fmWidthWritePending) {
            emit extensionError(requestId, QStringLiteral("Read requires an identified connection and no pending width work.")); return true;
        }
        beginFmWidthRead(); const QString token = m_fmWidthReadToken;
        advanceFmWidthRead(); if (!guard || generation != m_generation) { return true; }
        if (token == m_fmWidthReadToken) { emit extensionResult(requestId, fmWidthStatus()); } return true;
    }
    if (argument.typeId() != QMetaType::QVariantMap || args.size() != 2 || !fmSequence(args.value("revision"))
        || args.value("revision").toString() != QString::number(m_fmWidthEpoch)
        || args.value("value").typeId() != QMetaType::Int || args.value("value").toInt() < 0 || args.value("value").toInt() > 1
        || !fmWidthUnavailable().isEmpty()) {
        emit extensionError(requestId, QStringLiteral("Apply requires an exact current owned RX width revision and Normal/Narrow value.")); return true;
    }
    const quint64 epoch = m_fmWidthEpoch;
    const quint64 writeTicket = ++m_fmWidthSerial;
    m_fmWidthWriteTicket = writeTicket;
    const quint64 safety = m_receiverOptionsEpoch;
    const quint64 contextEpoch = m_fmWidthContextEpoch;
    m_fmWidthRequested = args.value("value").toInt(); m_fmWidthWritePending = true; m_fmWidthHanded = false;
    m_fmWidthLastWrite = {{"state", QStringLiteral("pending")}, {"reason", QStringLiteral("Awaiting explicit native setting handoff.")}};
    publishFmWidth(); if (!guard || generation != m_generation) { return true; }
    refreshFmWidthSafety();
    if (m_fmWidthWriteTicket != writeTicket) {
        emit extensionError(requestId, QStringLiteral("Width intent superseded before handoff.")); return true;
    }
    // Temporarily inspect availability without letting our own pending marker hide it.
    m_fmWidthWritePending = false;
    const bool current = epoch == m_fmWidthEpoch && safety == m_receiverOptionsEpoch && fmWidthUnavailable().isEmpty();
    if (!current) { m_fmWidthWriteTicket = 0; m_fmWidthLastWrite.insert("state", QStringLiteral("rejected")); publishFmWidth(); if (guard && generation == m_generation) { emit extensionError(requestId, QStringLiteral("Width intent retired before handoff.")); } return true; }
    m_fmWidthWritePending = true; m_fmWidthOwned.clear();
    m_fmWidthBatches["FW0"] = std::max(m_fmWidthBatches.value("FW0"), m_controlIngressSequence);
    m_fmWidthHanded = true;
    const bool sent = send(m_fmWidthRequested ? "FW01" : "FW00");
    if (!guard || generation != m_generation) { return true; }
    refreshFmWidthSafety();
    if (m_fmWidthWriteTicket != writeTicket) {
        emit extensionResult(requestId, QVariantMap{{"sent", sent}, {"verified", false}, {"superseded", true}}); return true;
    }
    if (sent) {
        m_fmWidthLastWrite.insert("state", QStringLiteral("sent_unverified"));
        m_fmWidthLastWrite.insert("reason", QStringLiteral("Native setting sent; explicit readback is observational only."));
        if (contextEpoch == m_fmWidthContextEpoch && safety == m_receiverOptionsEpoch && m_fmWidthWritePending) { beginFmWidthRead(); advanceFmWidthRead(); }
        else { m_fmWidthWritePending = false; m_fmWidthWriteTicket = 0; m_fmWidthLastWrite.insert("state", QStringLiteral("indeterminate")); }
    } else { m_fmWidthWritePending = false; m_fmWidthWriteTicket = 0; m_fmWidthLastWrite.insert("state", QStringLiteral("rejected")); m_fmWidthLastWrite.insert("reason", QStringLiteral("Native width transport refused; no retry or inverse.")); }
    if (!guard || generation != m_generation) { return true; }
    publishFmWidth(); if (guard && generation == m_generation) { emit extensionResult(requestId, QVariantMap{{"sent", sent}, {"verified", false}}); }
    return true;
}
}
