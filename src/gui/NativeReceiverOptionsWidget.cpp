#include "NativeReceiverOptionsWidget.h"
#include "ControlAvailabilityRegistry.h"
#include "GuardedSlider.h"
#include "models/RadioModel.h"

#include <QAccessible>
#include <QAccessibleValueChangeEvent>
#include <QComboBox>
#include <QGridLayout>
#include <QLabel>
#include <QList>
#include <QMetaType>
#include <QPushButton>
#include <QSet>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QStringList>
#include <QVariantList>
#include <QVBoxLayout>

namespace AetherSDR {
namespace {
const QStringList kIds{"speakerMute", "signalBlanking", "busy"};
const QStringList kVerbs{"receiver.options.get", "receiver.options.read", "receiver.options.set", "receiver.options.cancel"};
bool text(const QVariant& v, int maximum = 512, bool empty = true)
{
    return v.typeId() == QMetaType::QString && v.toString().size() <= maximum && (empty || !v.toString().isEmpty());
}
bool sequence(const QVariant& v, bool empty = false, bool positive = false)
{
    if (!text(v, 20, empty)) { return false; }
    if (empty && v.toString().isEmpty()) { return true; }
    bool ok = false;
    const quint64 n = v.toString().toULongLong(&ok);
    return ok && QString::number(n) == v.toString() && (!positive || n > 0);
}
bool flag(const QVariant& v) { return v.typeId() == QMetaType::Bool; }
bool map(const QVariant& v) { return v.typeId() == QMetaType::QVariantMap; }
bool count(const QVariant& v) { return v.typeId() == QMetaType::Int && v.toInt() >= 0 && v.toInt() <= 4; }
bool keys(const QVariantMap& value, const QStringList& required, const QStringList& optional = {})
{
    for (const QString& key : required) { if (!value.contains(key)) { return false; } }
    for (auto it = value.cbegin(); it != value.cend(); ++it) {
        if (!required.contains(it.key()) && !optional.contains(it.key())) { return false; }
    }
    return true;
}
QString nativeNamespace(const RadioCapabilities& caps)
{
    for (const QString& ns : caps.extensionNamespaces) {
        const QVariantMap entry = caps.extensions.value(ns).toMap();
        const QVariant version = entry.value("nativeReceiverOptionsVersion");
        if (version.typeId() != QMetaType::Int || version.toInt() != 1
            || entry.value("nativeReceiverOptionsVerbs").typeId() != QMetaType::QStringList) { continue; }
        bool complete = true;
        for (const QString& verb : kVerbs) { complete = complete && entry.value("nativeReceiverOptionsVerbs").toStringList().contains(verb); }
        if (complete) { return ns; }
    }
    return {};
}
bool validStatus(const QVariantMap& status)
{
    if (!keys(status, {"statusRevision", "context", "connected", "reason", "rows", "refresh", "lastWrite", "notice", "recording"})
        || !sequence(status.value("statusRevision")) || !text(status.value("context"), 512, false)
        || !flag(status.value("connected")) || !text(status.value("reason"), 2048)
        || !text(status.value("notice"), 4096) || status.value("rows").typeId() != QMetaType::QVariantList
        || status.value("rows").toList().size() != 3 || !map(status.value("refresh")) || !map(status.value("lastWrite"))) { return false; }
    QSet<QString> ids;
    bool anyEditable = false;
    bool anyPending = false;
    for (const QVariant& raw : status.value("rows").toList()) {
        if (!map(raw)) { return false; }
        const QVariantMap row = raw.toMap();
        const QString id = row.value("id").toString();
        if (!keys(row, {"id", "label", "known", "revision", "editable", "readonly", "pending", "reason", "ageMs"}, {"value"})
            || !text(row.value("id")) || !kIds.contains(id) || ids.contains(id)
            || !text(row.value("label"), 512, false) || !sequence(row.value("revision"))
            || !flag(row.value("known")) || !flag(row.value("editable")) || !flag(row.value("readonly"))
            || !flag(row.value("pending")) || !text(row.value("reason"), 2048)
            || row.value("ageMs").typeId() != QMetaType::LongLong || row.value("ageMs").toLongLong() < -1
            || (row.contains("value") && !flag(row.value("value")))
            || row.value("readonly").toBool() != (id == "busy")) { return false; }
        const bool known = row.value("known").toBool();
        const bool editable = row.value("editable").toBool();
        const bool pending = row.value("pending").toBool();
        if ((known && (!row.contains("value") || !status.value("connected").toBool() || row.value("ageMs").toLongLong() < 0))
            || (id == "busy" && (editable || pending || (known && row.value("ageMs").toLongLong() > 1500)))
            || (editable && (!known || pending || !status.value("connected").toBool()))) { return false; }
        ids.insert(id);
        anyEditable = anyEditable || editable;
        anyPending = anyPending || pending;
    }
    if (!map(status.value("recording"))) { return false; }
    const QVariantMap recording = status.value("recording").toMap();
    if (!keys(recording, {"known", "revision", "reason"}, {"state", "progress"})
        || !flag(recording.value("known")) || !sequence(recording.value("revision")) || !text(recording.value("reason"), 2048)
        || recording.contains("state") != recording.contains("progress")) { return false; }
    if (recording.contains("state") && (recording.value("state").typeId() != QMetaType::Int
        || !QList<int>{0, 1, 3, 4, 5, 6, 7}.contains(recording.value("state").toInt())
        || recording.value("progress").typeId() != QMetaType::Int || recording.value("progress").toInt() < 0
        || recording.value("progress").toInt() > 100)) { return false; }
    if (recording.value("known").toBool() && (!recording.contains("state") || !status.value("connected").toBool())) { return false; }
    const QVariantMap refresh = status.value("refresh").toMap();
    if (!keys(refresh, {"token", "busy", "total", "completed", "observed", "failed", "pendingKey", "reason"})
        || !sequence(refresh.value("token"), true, true) || !flag(refresh.value("busy"))
        || !text(refresh.value("pendingKey"), 32) || !QStringList{"", "QS", "BK", "BY", "RE"}.contains(refresh.value("pendingKey").toString())
        || !text(refresh.value("reason"), 2048)) { return false; }
    for (const QString& key : {QString("total"), QString("completed"), QString("observed"), QString("failed")}) {
        if (!count(refresh.value(key))) { return false; }
    }
    if (refresh.value("completed").toInt() != refresh.value("observed").toInt() + refresh.value("failed").toInt()
        || refresh.value("completed").toInt() > refresh.value("total").toInt()
        || (refresh.value("busy").toBool() && (refresh.value("token").toString().isEmpty() || !status.value("connected").toBool()))
        || (anyEditable && (anyPending || refresh.value("busy").toBool()))) { return false; }
    const QVariantMap write = status.value("lastWrite").toMap();
    if (!keys(write, {"token", "id", "state", "reason"}) || !sequence(write.value("token"), true, true)
        || !text(write.value("id")) || !QStringList{"", "speakerMute", "signalBlanking"}.contains(write.value("id").toString())
        || !text(write.value("state")) || !QStringList{"none", "pending", "sent_unverified", "observed_consistent", "indeterminate", "rejected"}.contains(write.value("state").toString())
        || !text(write.value("reason"), 2048)) { return false; }
    return write.value("state") == "none"
        ? write.value("token").toString().isEmpty() && write.value("id").toString().isEmpty()
        : !write.value("token").toString().isEmpty() && !write.value("id").toString().isEmpty();
}
quint64 nextRequest() { static quint64 request = 0x52584F5000000000ULL; return ++request; }
QString fallbackLabel(const QString& id)
{
    if (id == "speakerMute") { return NativeReceiverOptionsWidget::tr("Physical speaker mute"); }
    if (id == "signalBlanking") { return NativeReceiverOptionsWidget::tr("Received signal blanking"); }
    return NativeReceiverOptionsWidget::tr("Last observed BUSY");
}
}

NativeReceiverOptionsWidget::NativeReceiverOptionsWidget(RadioModel& model, QWidget* parent)
    : QWidget(parent), m_model(model), m_availability(new ControlAvailabilityRegistry(model, this)),
      m_read(new QPushButton(tr("Read receiver options"), this)), m_cancel(new QPushButton(tr("Cancel reads"), this)),
      m_recording(new QLabel(this)), m_notice(new QLabel(this)), m_progress(new QLabel(this)), m_write(new QLabel(this)), m_status(new QLabel(this))
{
    setObjectName("nativeReceiverOptionsSettings");
    auto* layout = new QVBoxLayout(this);
    auto* explanation = new QLabel(tr("These are radio receiver controls, separate from computer audio mute and noise blankers. Choose a draft and Apply explicitly. Last observed BUSY is an indicator, not a transmission control. Read to refresh; opening or closing this page does not change the radio."), this);
    explanation->setTextFormat(Qt::PlainText);
    explanation->setWordWrap(true);
    layout->addWidget(explanation);
    auto* grid = new QGridLayout;
    layout->addLayout(grid);
    int index = 0;
    for (const QString& id : kIds) {
        Row row;
        row.observed = new QLabel(this);
        row.observed->setObjectName("receiverOptions_" + id + "Observed");
        row.observed->setAccessibleName(fallbackLabel(id));
        row.observed->setTextFormat(Qt::PlainText);
        row.observed->setWordWrap(true);
        row.observed->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
        grid->addWidget(row.observed, index, 0);
        if (id != "busy") {
            row.draft = new GuardedComboBox(this);
            row.draft->setObjectName("receiverOptions_" + id + "Draft");
            row.draft->setAccessibleName(tr("%1 draft").arg(fallbackLabel(id)));
            row.draft->addItem(tr("Off"), false);
            row.draft->addItem(tr("On"), true);
            row.draft->setCurrentIndex(-1);
            row.apply = new QPushButton(tr("Apply"), this);
            row.apply->setObjectName("receiverOptions_" + id + "Apply");
            row.apply->setAccessibleName(tr("Apply %1").arg(fallbackLabel(id)));
            row.apply->setAutoDefault(false);
            grid->addWidget(row.draft, index, 1);
            grid->addWidget(row.apply, index, 2);
            connect(row.draft, &QComboBox::activated, this, [this, id](int choice) { editDraft(id, choice); });
            connect(row.apply, &QPushButton::clicked, this, [this, id] {
                if (!available(id, true)) { return; }
                const Row owner = m_rows.value(id);
                submit("receiver.options.set", {{"context", owner.context}, {"id", id}, {"revision", owner.revision}, {"value", owner.value}});
            });
        }
        m_rows.insert(id, row);
        if (row.draft) {
            for (QWidget* control : {static_cast<QWidget*>(row.draft), static_cast<QWidget*>(row.apply)}) {
                m_availability->registerWidget(control, tr("Connect to a radio supporting native receiver options."),
                    [this, id, control](bool connected, const RadioCapabilities& caps) {
                        return connected && !nativeNamespace(caps).isEmpty() && available(id, control == m_rows.value(id).apply);
                    }, {}, false);
            }
        }
        ++index;
    }
    m_read->setObjectName("receiverOptionsRead");
    m_cancel->setObjectName("receiverOptionsCancel");
    m_read->setAccessibleName(tr("Read receiver options"));
    m_cancel->setAccessibleName(tr("Cancel receiver option reads"));
    m_read->setAutoDefault(false);
    m_cancel->setAutoDefault(false);
    grid->addWidget(m_read, index, 0, 1, 2);
    grid->addWidget(m_cancel, index, 2);
    for (QPushButton* button : {m_read, m_cancel}) {
        m_availability->registerWidget(button, tr("Connect to a radio supporting native receiver options."),
            [this, button](bool connected, const RadioCapabilities& caps) {
                if (!connected || nativeNamespace(caps).isEmpty() || !m_requests.isEmpty()) { return false; }
                if (m_confirmed.isEmpty()) { return button == m_read; }
                const bool reading = m_confirmed.value("refresh").toMap().value("busy").toBool();
                return button == m_read ? !reading : reading;
            }, {}, false);
    }
    const std::array<QLabel*, 5> labels{m_recording, m_notice, m_progress, m_write, m_status};
    const std::array<QString, 5> names{"receiverOptionsRecording", "receiverOptionsNotice", "receiverOptionsProgress", "receiverOptionsWriteStatus", "receiverOptionsStatus"};
    for (int i = 0; i < 5; ++i) {
        labels[i]->setObjectName(names[i]);
        labels[i]->setAccessibleName(names[i]);
        labels[i]->setTextFormat(Qt::PlainText);
        labels[i]->setWordWrap(true);
        labels[i]->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
        layout->addWidget(labels[i]);
    }
    m_recording->setAccessibleName(tr("Last reported native recording/playback"));
    layout->addStretch();
    connect(m_read, &QPushButton::clicked, this, &NativeReceiverOptionsWidget::read);
    connect(m_cancel, &QPushButton::clicked, this, [this] {
        submit("receiver.options.cancel", {{"context", m_confirmed.value("context")}, {"token", m_confirmed.value("refresh").toMap().value("token")}});
    });
    connect(&model, &RadioModel::connectionStateChanged, this, &NativeReceiverOptionsWidget::bindBackend);
    connect(&model, &RadioModel::capabilitiesChanged, this, &NativeReceiverOptionsWidget::bindBackend);
    bindBackend();
}

NativeReceiverOptionsWidget::~NativeReceiverOptionsWidget()
{
    for (QObject* child : findChildren<QObject*>()) { QObject::disconnect(child, nullptr, this, nullptr); }
    QObject::disconnect(&m_model, nullptr, this, nullptr);
    QObject::disconnect(&m_model, nullptr, m_availability, nullptr);
    for (const QMetaObject::Connection& connection : m_connections) { QObject::disconnect(connection); }
}
QVariantMap NativeReceiverOptionsWidget::descriptor(const QString& id) const
{
    for (const QVariant& raw : m_confirmed.value("rows").toList()) {
        if (raw.toMap().value("id") == id) { return raw.toMap(); }
    }
    return {};
}
bool NativeReceiverOptionsWidget::available(const QString& id, bool apply) const
{
    const QVariantMap field = descriptor(id);
    if (!m_requests.isEmpty() || !field.value("known").toBool() || !field.value("editable").toBool()
        || field.value("readonly").toBool() || field.value("pending").toBool()) { return false; }
    if (!apply) { return true; }
    const Row row = m_rows.value(id);
    return row.seeded && !row.retired && row.context == m_confirmed.value("context").toString()
        && row.revision == field.value("revision").toString();
}
void NativeReceiverOptionsWidget::editDraft(const QString& id, int index)
{
    Row& row = m_rows[id];
    if (!available(id) || !row.draft || index < 0 || index >= row.draft->count()
        || !flag(row.draft->itemData(index))) { return; }
    row.seeded = row.dirty = true;
    row.retired = false;
    row.value = row.draft->itemData(index).toBool();
    row.context = m_confirmed.value("context").toString();
    row.revision = descriptor(id).value("revision").toString();
    refreshAvailability();
}
void NativeReceiverOptionsWidget::read()
{
    if (m_confirmed.isEmpty()) { m_readAfterGet = true; submit("receiver.options.get"); return; }
    submit("receiver.options.read", {{"context", m_confirmed.value("context")}});
}
void NativeReceiverOptionsWidget::bindBackend()
{
    IRadioBackend* backend = m_model.isConnected() ? m_model.backend() : nullptr;
    const QString ns = backend ? nativeNamespace(m_model.backendCapabilities()) : QString{};
    if (backend == m_backend && ns == m_namespace) { render(); return; }
    const quint64 generation = ++m_bindingGeneration;
    const RadioControlSession session = m_model.controlSession();
    for (const QMetaObject::Connection& connection : m_connections) { disconnect(connection); }
    m_backend = backend;
    m_namespace = ns;
    m_confirmed.clear();
    m_requests.clear();
    m_statusRevision = 0;
    ++m_statusSerial;
    m_readAfterGet = false;
    for (Row& row : m_rows) { row.seeded = row.dirty = row.retired = false; row.context.clear(); row.revision.clear(); }
    const QPointer<NativeReceiverOptionsWidget> alive(this);
    m_status->clear();
    if (!alive || generation != m_bindingGeneration) { return; }
    render();
    if (!alive || generation != m_bindingGeneration || !backend || ns.isEmpty()) { return; }
    const QPointer<IRadioBackend> source(backend);
    const auto current = [this, alive, source, generation, session] {
        return alive && source && generation == m_bindingGeneration && source == m_backend
            && source == m_model.backend() && m_model.isConnected() && m_model.controlSessionCurrent(session);
    };
    m_connections[0] = connect(backend, &IRadioBackend::extensionStatus, this,
        [this, current, ns](const QString& space, const QString& kind, const QVariantMap& status) {
            if (current() && space == ns && kind == "receiverOptions") { acceptStatus(status); }
        });
    m_connections[1] = connect(backend, &IRadioBackend::extensionResult, this,
        [this, current](quint64 request, const QVariant& result) {
            if (!current() || !m_requests.contains(request)) { return; }
            const QString verb = m_requests.take(request);
            const QPointer<NativeReceiverOptionsWidget> alive(this);
            if (verb == "receiver.options.get") {
                if (m_getSerial == m_statusSerial) { acceptStatus(map(result) ? result.toMap() : QVariantMap{}); }
                else { render(); }
                if (!alive || !current()) { return; }
                if (m_readAfterGet) {
                    m_readAfterGet = false;
                    if (!m_confirmed.isEmpty() && !m_confirmed.value("refresh").toMap().value("busy").toBool()) {
                        submit("receiver.options.read", {{"context", m_confirmed.value("context")}});
                    }
                }
                return;
            }
            const QVariantMap receipt = result.toMap();
            bool valid = map(result);
            if (verb == "receiver.options.read") {
                valid = valid && keys(receipt, {"token"}) && sequence(receipt.value("token"), false, true);
            } else if (verb == "receiver.options.cancel") {
                valid = valid && keys(receipt, {"cancelled"}) && flag(receipt.value("cancelled")) && receipt.value("cancelled").toBool();
            } else {
                valid = valid && keys(receipt, {"sent", "verified", "token"}) && flag(receipt.value("sent"))
                    && receipt.value("sent").toBool() && flag(receipt.value("verified")) && !receipt.value("verified").toBool()
                    && sequence(receipt.value("token"), false, true);
            }
            m_status->setText(!valid ? tr("Invalid request receipt; no confirmation inferred.")
                : verb == "receiver.options.set" ? tr("Request sent; see Last request for the current radio observation.")
                : verb == "receiver.options.cancel" ? tr("Local read verification cancelled. No radio setting was restored.")
                : tr("Read requested; values change only after native observations."));
            if (alive && current()) { render(); }
        });
    m_connections[2] = connect(backend, &IRadioBackend::extensionError, this,
        [this, current](quint64 request, const QString& reason) {
            if (!current() || !m_requests.remove(request)) { return; }
            m_readAfterGet = false;
            const QPointer<NativeReceiverOptionsWidget> alive(this);
            m_status->setText(reason);
            if (alive && current()) { render(); }
        });
    submit("receiver.options.get");
}
void NativeReceiverOptionsWidget::acceptStatus(const QVariantMap& status)
{
    const bool hasRevision = sequence(status.value("statusRevision"));
    const quint64 revision = hasRevision ? status.value("statusRevision").toString().toULongLong() : 0;
    if (hasRevision && revision < m_statusRevision) { return; }
    ++m_statusSerial;
    if (validStatus(status)) { m_statusRevision = revision; m_confirmed = status; }
    else {
        m_confirmed.clear();
        for (Row& row : m_rows) { row.retired = true; }
    }
    render();
}
void NativeReceiverOptionsWidget::render()
{
    const QPointer<NativeReceiverOptionsWidget> alive(this);
    const quint64 generation = m_bindingGeneration;
    const quint64 serial = m_statusSerial;
    const auto current = [this, alive, generation, serial] {
        return alive && generation == m_bindingGeneration && serial == m_statusSerial;
    };
    for (const QString& id : kIds) {
        Row& row = m_rows[id];
        const QVariantMap field = descriptor(id);
        const bool known = field.value("known").toBool();
        const QString context = m_confirmed.value("context").toString();
        if (!known) { row.retired = true; }
        else if (!row.seeded || !row.dirty) {
            row.seeded = true;
            row.retired = false;
            row.value = field.value("value").toBool();
            row.context = context;
            row.revision = field.value("revision").toString();
        } else if (row.context != context || row.revision != field.value("revision").toString()) { row.retired = true; }
        if (row.draft) {
            const QPointer<QComboBox> editor(row.draft);
            QSignalBlocker blocker(editor);
            editor->setCurrentIndex(row.seeded ? editor->findData(row.value) : -1);
            if (!editor) { blocker.dismiss(); }
            if (!editor || !current()) { return; }
        }
        const QString value = !field.contains("value") ? tr("Unknown")
            : known ? field.value("value").toBool() ? tr("On") : tr("Off")
            : tr("Unknown; last observed %1").arg(field.value("value").toBool() ? tr("On") : tr("Off"));
        const QString label = id == "busy" ? fallbackLabel(id) : field.value("label", fallbackLabel(id)).toString();
        row.observed->setText(tr("%1: %2. %3").arg(label, value, field.value("reason").toString()));
        if (!current()) { return; }
        QAccessibleValueChangeEvent event(row.observed, value);
        QAccessible::updateAccessibility(&event);
        if (!current()) { return; }
        if (id == "busy") {
            row.observed->setAccessibleDescription(tr("Read-only radio indicator. Freshness is an application presentation policy; stale values remain unknown until refreshed. %1").arg(field.value("reason").toString()));
            if (!current()) { return; }
        }
    }
    const QVariantMap recording = m_confirmed.value("recording").toMap();
    QString recordingText = tr("Last reported native recording/playback: Unknown. %1").arg(recording.value("reason").toString());
    if (recording.contains("state")) {
        const QMap<int, QString> states{{0, tr("Stopped")}, {1, tr("Manual recording")}, {3, tr("Quick playback")},
            {4, tr("Recording pause/resume status")}, {5, tr("Playback pause/resume status")},
            {6, tr("Recording failure")}, {7, tr("Playback failure")}};
        const QString progression = recording.value("progress").toInt() == 0 ? tr("no playback progression reported")
            : tr("reported playback progression %1%").arg(recording.value("progress").toInt());
        recordingText = tr("Last reported native recording/playback: %1%2; %3. %4")
            .arg(recording.value("known").toBool() ? QString{} : tr("Unknown; last observed "),
                states.value(recording.value("state").toInt()), progression,
                recording.value("known").toBool() ? tr("Read receiver options to refresh.") : recording.value("reason").toString());
    }
    if (m_recording->text() != recordingText) {
        m_recording->setText(recordingText);
        if (!current()) { return; }
        QAccessibleValueChangeEvent recordingEvent(m_recording, recordingText);
        QAccessible::updateAccessibility(&recordingEvent);
        if (!current()) { return; }
    }
    m_notice->setText(m_confirmed.isEmpty()
        ? tr("Read current receiver observations. Last observed BUSY is read-only; no initial or restored value is assumed.")
        : m_confirmed.value("notice").toString());
    if (!current()) { return; }
    const QVariantMap refresh = m_confirmed.value("refresh").toMap();
    m_progress->setText(m_confirmed.isEmpty() ? tr("Read progress: Unknown")
        : tr("Read progress %1/%2; observed %3, failed %4. %5").arg(refresh.value("completed").toInt())
            .arg(refresh.value("total").toInt()).arg(refresh.value("observed").toInt())
            .arg(refresh.value("failed").toInt()).arg(refresh.value("reason").toString()));
    if (!current()) { return; }
    const QVariantMap write = m_confirmed.value("lastWrite").toMap();
    const QMap<QString, QString> outcomes{
        {"pending", tr("Waiting to send")}, {"sent_unverified", tr("Waiting for radio readback")},
        {"observed_consistent", tr("Radio readback matches the request")},
        {"indeterminate", tr("Outcome unknown; read the radio state again")},
        {"rejected", tr("Request not sent")}};
    const QString state = write.value("state").toString();
    m_write->setText(state == "none" ? tr("Last request: None")
        : tr("Last request: %1 — %2").arg(write.value("id").toString().isEmpty()
            ? tr("Unknown") : fallbackLabel(write.value("id").toString()), outcomes.value(state, tr("Unknown"))));
    if (current()) { refreshAvailability(); }
}
void NativeReceiverOptionsWidget::refreshAvailability()
{
    const QPointer<NativeReceiverOptionsWidget> alive(this);
    const quint64 generation = m_bindingGeneration;
    const quint64 serial = m_statusSerial;
    const auto current = [this, alive, generation, serial] {
        return alive && generation == m_bindingGeneration && serial == m_statusSerial;
    };
    m_availability->refreshEngaged();
    if (!current()) { return; }
    QList<QWidget*> controls{m_read, m_cancel};
    for (const Row& row : m_rows) { if (row.draft) { controls << row.draft << row.apply; } }
    for (QWidget* control : controls) {
        if (control->isEnabled()) { continue; }
        QString reason;
        if (!m_model.isConnected()) { reason = tr("Connect to a radio supporting native receiver options."); }
        else if (nativeNamespace(m_model.backendCapabilities()).isEmpty()) { reason = tr("This radio does not support native receiver options."); }
        else if (m_confirmed.isEmpty()) { reason = tr("Read receiver options to obtain a valid current radio description."); }
        else {
            for (auto it = m_rows.cbegin(); it != m_rows.cend(); ++it) {
                if (control == it.value().draft || control == it.value().apply) { reason = descriptor(it.key()).value("reason").toString(); }
            }
        }
        if (reason.isEmpty()) {
            reason = !m_requests.isEmpty() ? tr("Waiting for the current native request.")
                : control == m_cancel ? tr("No native read is running.")
                : control == m_read ? tr("Cancel the current read before starting another.")
                : tr("Select the draft again to bind it to the current native observation.");
        }
        const QPointer<QWidget> target(control);
        target->setToolTip(reason);
        if (!target || !current()) { return; }
        target->setAccessibleDescription(reason);
        if (!current()) { return; }
    }
}
void NativeReceiverOptionsWidget::submit(const QString& verb, const QVariantMap& arguments)
{
    if (!m_backend || m_backend != m_model.backend() || !m_model.isConnected() || m_namespace.isEmpty() || !m_requests.isEmpty()) { return; }
    const quint64 request = nextRequest();
    const quint64 generation = m_bindingGeneration;
    const RadioControlSession session = m_model.controlSession();
    m_requests.insert(request, verb);
    if (verb == "receiver.options.get") { m_getSerial = m_statusSerial; }
    const quint64 serial = m_statusSerial;
    const QPointer<NativeReceiverOptionsWidget> alive(this);
    render();
    if (!alive || generation != m_bindingGeneration || !m_requests.contains(request) || !m_model.controlSessionCurrent(session)) { return; }
    if (verb != "receiver.options.get" && serial != m_statusSerial) {
        m_requests.remove(request);
        render();
        return;
    }
    m_model.invokeBackendExtension(m_namespace, verb, request, arguments);
}
}
