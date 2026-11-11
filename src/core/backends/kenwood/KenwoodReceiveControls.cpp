#include "KenwoodBackend.h"
#include "core/backends/SliceDelta.h"

#include <QByteArray>
#include <QList>
#include <QMap>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QMetaType>
#include <QPointer>
#include <QVariantList>
#include <cmath>
#include <algorithm>

namespace AetherSDR::kenwood {
namespace {
struct ReceiveControl {
    QString id;
    QString label;
    QByteArray command;
    int minimum;
    int maximum;
    int digits;
    QString unit;
    QStringList modes;
    QStringList choices;
    int reset{-1};
};
const QList<ReceiveControl>& controls()
{
    // PC Command Reference 2019-01-30 pp.4,6,7,25,47,48,49,50,57;
    // Instruction Manual EN-6-5..6-9. Reset sentinels are not normal values.
    static const QStringList all{"LSB", "USB", "CW", "FM", "AM", "RTTY", "CWR", "RTTYR", "PSK", "PSKR", "DIGL", "DIGU", "DFM", "DAM"};
    static const QStringList nonFm{"LSB", "USB", "CW", "AM", "RTTY", "CWR", "RTTYR", "PSK", "PSKR", "DIGL", "DIGU", "DAM"};
    static const QStringList cw{"CW", "CWR"};
    static const QStringList voice{"LSB", "USB", "FM", "AM", "DIGL", "DIGU", "DFM", "DAM"};
    static const QList<ReceiveControl> definitions{
        // APF choice IDs stay 1=Off/2=On; TS-890S firmware 1.01 uses wire
        // 0=Off/1=On, despite the command reference's shifted enumeration.
        {"apf.enabled", "Audio peak filter", "AP0", 1, 2, 1, "", {"CW", "CWR", "RTTY", "RTTYR"}, {"Off", "On"}},
        {"apf.shift", "APF shift", "AP1", 0, 80, 2, "index", cw, {}, 99},
        {"apf.bandwidth", "APF bandwidth", "AP2", 0, 2, 1, "", cw, {"Narrow (80 Hz)", "Medium (160 Hz)", "Wide (320 Hz)"}, 9},
        {"apf.gain", "APF gain", "AP3", 0, 6, 1, "dB", cw, {}, 9},
        {"notch.enabled", "Manual notch", "NT", 0, 1, 1, "", nonFm, {"Off", "On"}},
        {"notch.width", "Notch bandwidth", "NW", 0, 2, 1, "", nonFm, {"Normal", "Middle", "Wide"}},
        {"notch.position", "Notch position", "BP", 0, 255, 3, "index", nonFm, {}},
        {"nr.mode", "Noise reduction", "NR", 0, 2, 1, "", all, {"Off", "NR1", "NR2"}},
        {"nr1.level", "NR1 level", "RL1", 1, 10, 2, "level", all, {}, 99},
        {"nr2.time", "NR2 correlation time", "RL2", 0, 9, 2, "index", nonFm, {}, 99},
        {"nb1.enabled", "Noise blanker 1", "NB1", 0, 1, 1, "", nonFm, {"Off", "On"}},
        {"nb1.level", "NB1 level", "NL1", 1, 20, 3, "level", nonFm, {}, 999},
        {"nb2.enabled", "Noise blanker 2", "NB2", 0, 1, 1, "", nonFm, {"Off", "On"}},
        {"nb2.level", "NB2 level / pulse sensitivity", "NL2", 1, 10, 3, "level", nonFm, {}, 999},
        {"nb2.type", "NB2 type", "NBT", 0, 1, 1, "", nonFm, {"Type A", "Type B"}},
        {"nb2.depth", "NB2 type B depth", "NBD", 1, 20, 3, "index", nonFm, {}, 999},
        {"nb2.width", "NB2 type B width", "NBW", 1, 20, 3, "index", nonFm, {}, 999},
        {"bc.mode", "Beat canceler", "BC", 0, 2, 1, "", voice, {"Off", "BC1", "BC2"}},
        {"monitor.rx", "RX monitor", "MO1", 0, 1, 1, "", all, {"Off", "On"}},
        {"monitor.dsp", "DSP monitor", "MO2", 0, 1, 1, "", all, {"Off", "On"}}};
    return definitions;
}
QString modeReason(const ReceiveControl& control, const QString& mode)
{
    if (!control.modes.contains(mode)) {
        return QStringLiteral("Not available in the current receive mode.");
    }
    return {};
}
}

QString KenwoodBackend::receiveControlContext() const
{
    return QStringLiteral("%1:%2:%3:%4:%5:%6:%7:%8:%9:%10:%11")
        .arg(m_generation).arg(m_receiveVfo).arg(m_mode).arg(m_txStateKnown)
        .arg(m_txObserved).arg(m_tfSet ? (*m_tfSet ? 1 : 0) : -1)
        .arg(m_keyIntent).arg(m_cwIntent).arg(m_atuIntent)
        .arg(m_identityConfirmed).arg(isConnected());
}

bool KenwoodBackend::receiveControlsWritable() const
{
    return isConnected() && m_identityConfirmed && m_receiveVfo >= 0 && m_receiveVfo <= 1
        && !m_mode.isEmpty() && m_tfSet && !*m_tfSet && m_txStateKnown && !m_txObserved
        && !m_keyIntent && !m_cwIntent && !m_atuIntent
        && m_receiveControlsContext == receiveControlContext();
}

QVariantMap KenwoodBackend::receiveControlsStatus() const
{
    const bool ready = receiveControlsWritable();
    QVariantList rows;
    for (const ReceiveControl& control : controls()) {
        QString reason = ready ? modeReason(control, m_mode)
            : QStringLiteral("Requires confirmed receive operation, physical receive VFO and TF-SET off.");
        if (reason.isEmpty() && control.id.startsWith(QLatin1String("apf."))
            && (m_receiveControlsPending.contains(QStringLiteral("monitor.dsp"))
                || m_receiveDspMonitor != std::optional<bool>(false))) {
            reason = QStringLiteral("DSP monitor is active or awaiting readback; native APF controls are unavailable.");
        }
        if (reason.isEmpty() && control.id == "apf.enabled" && (m_mode == "RTTY" || m_mode == "RTTYR")
            && m_receiveFskSpacing != std::optional<int>(0)) {
            reason = QStringLiteral("FSK APF requires confirmed 170 Hz spacing (menu 2-06).");
        }
        if (reason.isEmpty() && control.id == "nr1.level" && (m_receiveControlsPending.contains("nr.mode")
            || m_receiveControlValues.value("nr.mode", -1) != 1)) {
            reason = QStringLiteral("Select NR1 and wait for radio readback.");
        }
        if (reason.isEmpty() && control.id == "nr2.time" && (m_receiveControlsPending.contains("nr.mode")
            || m_receiveControlValues.value("nr.mode", -1) != 2)) {
            reason = QStringLiteral("Select NR2 and wait for radio readback.");
        }
        if (reason.isEmpty() && (control.id == "nb2.level" || control.id == "nb2.depth" || control.id == "nb2.width")
            && m_receiveControlsPending.contains("nb2.type")) {
            reason = QStringLiteral("Waiting for NB2 type readback.");
        }
        if (reason.isEmpty() && (control.id == "nb2.depth" || control.id == "nb2.width")
            && m_receiveControlValues.value("nb2.type", -1) != 1) {
            reason = QStringLiteral("Select NB2 Type B and wait for radio readback.");
        }
        if (reason.isEmpty() && m_receiveControlsPending.contains(control.id)) {
            reason = QStringLiteral("Request sent; waiting for radio readback. Last confirmed value retained.");
        }
        if (reason.isEmpty() && !m_receiveControlValues.contains(control.id)) {
            reason = QStringLiteral("Waiting for current radio readback.");
        }
        QVariantMap row{{"id", control.id}, {"label", control.label}, {"unit", control.unit},
            {"minimum", control.minimum}, {"maximum", control.maximum}, {"step", 1},
            {"editable", reason.isEmpty()}, {"reason", reason}, {"resettable", control.reset >= 0},
            {"pending", m_receiveControlsPending.contains(control.id)}};
        if (m_receiveControlValues.contains(control.id)) { row.insert("value", m_receiveControlValues.value(control.id)); }
        QVariantList choices;
        for (int value = control.minimum; value <= control.maximum; ++value) {
            QString label;
            if (!control.choices.isEmpty()) { label = control.choices[value - control.minimum]; }
            else if (control.id == "apf.shift") { label = QStringLiteral("%1 Hz").arg((value - 40) * 5); }
            else if (control.id == "nr2.time") { label = QStringLiteral("%1 ms").arg((value + 1) * 2); }
            if (control.id == "nr.mode" && (m_mode == "FM" || m_mode == "DFM") && value == 2) { continue; }
            if (!label.isEmpty()) { choices.append(QVariantMap{{"value", value}, {"label", label}}); }
        }
        if (!choices.isEmpty()) { row.insert("choices", choices); }
        rows.append(row);
    }
    return {{"statusRevision", QString::number(m_receiveControlsStatusRevision)}, {"context", QStringLiteral("%1:%2").arg(m_generation).arg(m_receiveControlsRevision)},
        {"editable", ready}, {"mode", m_mode}, {"controls", rows}};
}

void KenwoodBackend::publishReceiveControls()
{
    ++m_receiveControlsStatusRevision;
    emit extensionStatus(QStringLiteral("kenwood"), QStringLiteral("receiveControls"), receiveControlsStatus());
}

bool KenwoodBackend::refreshReceiveControls(quint64 batch)
{
    const QString context = receiveControlContext();
    if (context == m_receiveControlsContext) { return false; }
    m_receiveControlsContext = context;
    ++m_receiveControlsRevision;
    m_receiveControlValues.clear();
    m_receiveControlsPending.clear();
    m_receiveControlMinimumBatches.clear();
    m_receiveFskSpacing.reset();
    m_receiveControlsMinimumBatch = batch;
    const QPointer<KenwoodBackend> guard(this);
    const quint64 generation = m_generation;
    const quint64 revision = m_receiveControlsRevision;
    publishReceiveControls();
    if (!guard || generation != m_generation || revision != m_receiveControlsRevision) { return true; }
    queryReceiveControls();
    if (guard && generation == m_generation && revision == m_receiveControlsRevision) { emit capabilitiesChanged(); }
    return true;
}

void KenwoodBackend::queryReceiveControls()
{
    if (!receiveControlsWritable()) { return; }
    m_receiveControlsMinimumBatch = m_controlIngressSequence;
    const QPointer<KenwoodBackend> guard(this);
    const quint64 generation = m_generation;
    const quint64 revision = m_receiveControlsRevision;
    // FSK APF requires menu 2-06 spacing 170 Hz; this query never changes it.
    if (m_mode == "RTTY" || m_mode == "RTTYR") {
        if (!send("EX00206") || !guard || generation != m_generation
            || revision != m_receiveControlsRevision || !receiveControlsStatus().value("editable").toBool()) { return; }
    }
    if (!m_receiveControlsInterested) {
        if (m_mode == "CW" || m_mode == "CWR") { send("AP0"); }
        return;
    }
    for (const ReceiveControl& control : controls()) {
        if (!modeReason(control, m_mode).isEmpty()) { continue; }
        if (!send(control.command) || !guard || generation != m_generation
            || revision != m_receiveControlsRevision || !receiveControlsStatus().value("editable").toBool()) { return; }
    }
}

void KenwoodBackend::adoptReceiveControl(const QByteArray& message, quint64 batch)
{
    const bool dspMonitorReport = message == "MO20" || message == "MO21";
    if (dspMonitorReport) {
        if (!isConnected() || !m_identityConfirmed || batch < m_receiveDspMonitorBatch
            || (m_receiveControlsPending.contains(QStringLiteral("monitor.dsp")) && batch <= m_receiveControlsMinimumBatch)) { return; }
    } else if (batch <= m_receiveControlsMinimumBatch || !receiveControlsStatus().value("editable").toBool()) { return; }
    if (message.startsWith("EX00206 ") && message.size() == 11 && (m_mode == "RTTY" || m_mode == "RTTYR")) {
        const QByteArray digits = message.sliced(8);
        if (digits[0] != '0' || digits[1] != '0' || digits[2] < '0' || digits[2] > '3') { return; }
        const int spacing = digits[2] - '0';
        if (m_receiveFskSpacing == spacing) { return; }
        m_receiveFskSpacing = spacing;
        m_receiveControlValues.remove(QStringLiteral("apf.enabled"));
        m_receiveControlsPending.remove(QStringLiteral("apf.enabled"));
        ++m_receiveControlsRevision;
        const QPointer<KenwoodBackend> guard(this);
        const quint64 generation = m_generation;
        const quint64 revision = m_receiveControlsRevision;
        emit capabilitiesChanged();
        if (!guard || generation != m_generation || revision != m_receiveControlsRevision) { return; }
        publishReceiveControls();
        if (guard && generation == m_generation && revision == m_receiveControlsRevision
            && receiveControlsStatus().value("editable").toBool()) { send("AP0"); }
        return;
    }
    for (const ReceiveControl& control : controls()) {
        if (!message.startsWith(control.command) || message.size() != control.command.size() + control.digits
            || (!dspMonitorReport && !modeReason(control, m_mode).isEmpty())) { continue; }
        const QByteArray digits = message.sliced(control.command.size());
        bool valid = true;
        for (char digit : digits) { valid = valid && digit >= '0' && digit <= '9'; }
        if (!valid) { return; }
        const int value = digits.toInt() + (control.id == QLatin1String("apf.enabled") ? 1 : 0);
        if (value < control.minimum || value > control.maximum
            || (control.id == "nr.mode" && value == 2 && (m_mode == "FM" || m_mode == "DFM"))) { return; }
        if (batch <= m_receiveControlMinimumBatches.value(control.id, 0)) { return; }
        const bool monitor = control.id == QLatin1String("monitor.rx") || control.id == QLatin1String("monitor.dsp");
        const QString observationKey = control.id + QStringLiteral(".observation");
        if (monitor && batch < m_receiveControlMinimumBatches.value(observationKey, 0)) { return; }
        if (monitor) { m_receiveControlMinimumBatches.insert(observationKey, batch); }
        const bool dspMonitorChanged = dspMonitorReport && m_receiveDspMonitor != std::optional<bool>(value == 1);
        if (dspMonitorReport) {
            m_receiveDspMonitor = value == 1;
            m_receiveDspMonitorBatch = batch;
            if (dspMonitorChanged) { ++m_receiveDspMonitorSerial; }
        }
        const quint64 monitorSerial = m_receiveDspMonitorSerial;
        const bool typeChanged = control.id == "nb2.type" && m_receiveControlValues.value(control.id, -1) != value;
        const bool nrChanged = control.id == "nr.mode" && m_receiveControlValues.value(control.id, -1) != value;
        if (typeChanged || nrChanged || dspMonitorChanged) { ++m_receiveControlsRevision; }
        m_receiveControlValues.insert(control.id, value);
        m_receiveControlsPending.remove(control.id);
        if (dspMonitorChanged) {
            // MO2 changes APF effectiveness (manual EN-6-9), not an AP0
            // observation. Withdraw the old APF authority without inventing Off.
            m_receiveControlValues.remove(QStringLiteral("apf.enabled"));
            m_receiveControlsPending.remove(QStringLiteral("apf.enabled"));
            m_receiveControlMinimumBatches.insert(QStringLiteral("apf.enabled"),
                std::max(m_receiveControlMinimumBatches.value(QStringLiteral("apf.enabled"), 0),
                         std::max(batch, m_controlIngressSequence)));
        }
        if (typeChanged) {
            // NL2 means effect level for Type A and pulse sensitivity for
            // Type B. A report from the old bank is not the new bank's value.
            m_receiveControlValues.remove(QStringLiteral("nb2.level"));
            m_receiveControlsPending.remove(QStringLiteral("nb2.level"));
            m_receiveControlMinimumBatches.insert(QStringLiteral("nb2.level"), batch);
        }
        const QString nrLevel = value == 1 ? QStringLiteral("nr1.level") : QStringLiteral("nr2.time");
        if (nrChanged && value != 0) {
            m_receiveControlValues.remove(nrLevel);
            m_receiveControlsPending.remove(nrLevel);
            m_receiveControlMinimumBatches.insert(nrLevel, batch);
        }
        const QPointer<KenwoodBackend> guard(this);
        const quint64 generation = m_generation;
        const quint64 revision = m_receiveControlsRevision;
        if (control.id == "apf.enabled") {
            if (m_receiveDspMonitor == std::optional<bool>(false)
                && !m_receiveControlsPending.contains(QStringLiteral("monitor.dsp"))) {
                SliceDelta delta;
                delta.apf = value == 2;
                emit sliceChanged(0, delta);
                if (!guard || generation != m_generation || revision != m_receiveControlsRevision) { return; }
            }
            emit capabilitiesChanged();
            if (!guard || generation != m_generation || revision != m_receiveControlsRevision) { return; }
        }
        if (dspMonitorChanged) {
            // Monitor state is already committed before the withdrawal callback.
            // The ordinary filter preferences do not describe its effective width.
            const quint64 filterRevision = m_filterRevision + 1;
            invalidateFilterBankAxes(false);
            if (!guard || generation != m_generation || revision != m_receiveControlsRevision
                || monitorSerial != m_receiveDspMonitorSerial || filterRevision != m_filterRevision) { return; }
        }
        publishReceiveControls();
        if (dspMonitorReport && (!guard || generation != m_generation || revision != m_receiveControlsRevision
            || monitorSerial != m_receiveDspMonitorSerial)) { return; }
        if (dspMonitorChanged && m_receiveDspMonitor == std::optional<bool>(false)) {
            queryFilter();
            if (!guard || generation != m_generation || revision != m_receiveControlsRevision
                || monitorSerial != m_receiveDspMonitorSerial) { return; }
        }
        if (dspMonitorChanged && guard && generation == m_generation && revision == m_receiveControlsRevision
            && monitorSerial == m_receiveDspMonitorSerial && receiveControlsStatus().value("editable").toBool()
            && (m_mode == QLatin1String("CW") || m_mode == QLatin1String("CWR")
                || m_mode == QLatin1String("RTTY") || m_mode == QLatin1String("RTTYR"))) {
            // Publications may consume newer ingress. Only a reply after this
            // handoff floor can requalify the dependent AP0 observation.
            m_receiveControlMinimumBatches.insert(QStringLiteral("apf.enabled"),
                std::max(m_receiveControlMinimumBatches.value(QStringLiteral("apf.enabled"), 0), m_controlIngressSequence));
            send("AP0");
            return;
        }
        if (nrChanged && value != 0 && guard && generation == m_generation && revision == m_receiveControlsRevision
            && m_receiveControlsInterested && receiveControlsStatus().value("editable").toBool()) { send(value == 1 ? "RL1" : "RL2"); }
        if (typeChanged && guard && generation == m_generation && revision == m_receiveControlsRevision
            && m_receiveControlsInterested && receiveControlsStatus().value("editable").toBool()) { send("NL2"); }
        return;
    }
}

bool KenwoodBackend::setReceiveControl(const QVariantMap& fields, bool reset)
{
    const QVariantMap status = receiveControlsStatus();
    if (fields.value("context").typeId() != QMetaType::QString || fields.value("context") != status.value("context")
        || fields.value("id").typeId() != QMetaType::QString || !status.value("editable").toBool()) { return false; }
    const QVariant value = fields.value("value");
    const int type = value.typeId();
    if (!reset && type != QMetaType::Int && type != QMetaType::UInt && type != QMetaType::LongLong
        && type != QMetaType::ULongLong && type != QMetaType::Double && type != QMetaType::Float) { return false; }
    bool valid = false;
    const double number = value.toDouble(&valid);
    if (!reset && (!valid || !std::isfinite(number) || number != std::trunc(number))) { return false; }
    const QString id = fields.value("id").toString();
    if ((id == QLatin1String("monitor.rx") || id == QLatin1String("monitor.dsp"))
        && !reset && (type != QMetaType::Int || fields.size() != 3)) { return false; }
    for (const ReceiveControl& control : controls()) {
        if (control.id != id || (reset ? control.reset < 0 : number < control.minimum || number > control.maximum)) { continue; }
        bool editable = false;
        for (const QVariant& entry : status.value("controls").toList()) {
            const QVariantMap row = entry.toMap();
            if (row.value("id") == id) { editable = row.value("editable").toBool(); }
        }
        if (!editable || (!reset && id == "nr.mode" && number == 2 && (m_mode == "FM" || m_mode == "DFM"))) { return false; }
        const int requested = reset ? control.reset : static_cast<int>(number);
        const int wireValue = id == QLatin1String("apf.enabled") ? requested - 1 : requested;
        const QByteArray command = control.command + QByteArray::number(wireValue).rightJustified(control.digits, '0');
        const QString receiveContext = m_receiveControlsContext;
        ++m_receiveControlsRevision;
        m_receiveControlsPending.insert(id, m_receiveControlsRevision);
        m_receiveControlsMinimumBatch = m_controlIngressSequence;
        const QPointer<KenwoodBackend> guard(this);
        const quint64 generation = m_generation;
        const quint64 revision = m_receiveControlsRevision;
        publishReceiveControls();
        if (id == "apf.enabled" && guard && generation == m_generation) { emit capabilitiesChanged(); }
        const auto current = [guard, generation, revision, id](bool requirePending) {
            if (!guard || generation != guard->m_generation || revision != guard->m_receiveControlsRevision
                || (requirePending && guard->m_receiveControlsPending.value(id, 0) != revision)
                || !guard->receiveControlsStatus().value("editable").toBool()) { return false; }
            if (id == "nr1.level" && (guard->m_receiveControlsPending.contains("nr.mode")
                || guard->m_receiveControlValues.value("nr.mode", -1) != 1)) { return false; }
            if (id == "nr2.time" && (guard->m_receiveControlsPending.contains("nr.mode")
                || guard->m_receiveControlValues.value("nr.mode", -1) != 2)) { return false; }
            if ((id == "nb2.level" || id == "nb2.depth" || id == "nb2.width")
                && guard->m_receiveControlsPending.contains("nb2.type")) { return false; }
            if ((id == "nb2.depth" || id == "nb2.width")
                && guard->m_receiveControlValues.value("nb2.type", -1) != 1) { return false; }
            if (id.startsWith(QLatin1String("apf."))
                && (guard->m_receiveControlsPending.contains(QStringLiteral("monitor.dsp"))
                    || guard->m_receiveDspMonitor != std::optional<bool>(false))) { return false; }
            if (id == "apf.enabled" && (guard->m_mode == "RTTY" || guard->m_mode == "RTTYR")
                && guard->m_receiveFskSpacing != std::optional<int>(0)) { return false; }
            return true;
        };
        if (!current(true)) {
            if (guard && generation == m_generation && m_receiveControlsPending.value(id, 0) == revision) {
                m_receiveControlsPending.remove(id);
                publishReceiveControls();
                if (id == "apf.enabled" && guard && generation == m_generation) { emit capabilitiesChanged(); }
            }
            return false;
        }
        if (!send(command)) { return false; }
        // A synchronous transport callback can retire this RX context. Never
        // continue the old transaction after that callback has returned.
        if (!current(false)) {
            // A synchronous, matching variant readback completes this request
            // and starts its own dependent query in the new descriptor epoch.
            const bool observed = !reset && (id == "nr.mode" || id == "nb2.type" || id == "monitor.dsp") && guard
                && generation == m_generation && receiveContext == m_receiveControlsContext
                && receiveControlsStatus().value("editable").toBool()
                && !m_receiveControlsPending.contains(id) && m_receiveControlValues.value(id, -1) == requested;
            if (observed && id == QLatin1String("monitor.dsp")) {
                return send(control.command) && guard && generation == m_generation;
            }
            return observed;
        }
        return send(control.command) && guard && generation == m_generation;
    }
    return false;
}
} // namespace AetherSDR::kenwood
