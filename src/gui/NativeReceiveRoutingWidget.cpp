#include "NativeReceiveRoutingWidget.h"

#include "ControlAvailabilityRegistry.h"
#include "models/RadioModel.h"

#include <QAccessible>
#include <QAccessibleValueChangeEvent>
#include <QCheckBox>
#include <QLabel>
#include <QMetaType>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStringList>
#include <QVBoxLayout>

namespace AetherSDR {
namespace {
const QStringList kFields{QStringLiteral("rxAntenna"), QStringLiteral("externalOutput")};
QString routingNamespace(const RadioCapabilities& caps)
{
    for (const QString& ns : caps.extensionNamespaces) {
        const QVariant version = caps.extensions.value(ns).toMap().value("nativeReceiveRoutingVersion");
        if (version.typeId() == QMetaType::Int && version.toInt() == 1) { return ns; }
    }
    return {};
}
bool textValid(const QVariant& value, bool empty = false, int maximum = 512)
{
    return value.typeId() == QMetaType::QString && value.toString().size() <= maximum
        && (empty || !value.toString().isEmpty());
}
bool revisionValid(const QVariant& value, quint64& revision)
{
    if (value.typeId() != QMetaType::QString || value.toString().size() > 20) { return false; }
    bool valid = false;
    revision = value.toString().toULongLong(&valid);
    return valid && QString::number(revision) == value.toString();
}
bool statusValid(const QVariantMap& status)
{
    if (!textValid(status.value("context"), true) || !textValid(status.value("mode"), true)
        || !textValid(status.value("reason"), true, 2048) || status.value("editable").typeId() != QMetaType::Bool
        || status.value("operations").typeId() != QMetaType::QVariantMap) { return false; }
    const QVariantMap operations = status.value("operations").toMap();
    if (operations.size() != 2) { return false; }
    for (const QString& field : kFields) {
        if (status.contains(field) && status.value(field).typeId() != QMetaType::Bool) { return false; }
        if (operations.value(field).typeId() != QMetaType::QVariantMap) { return false; }
        const QVariantMap operation = operations.value(field).toMap();
        if (operation.value("editable").typeId() != QMetaType::Bool || operation.value("pending").typeId() != QMetaType::Bool
            || !textValid(operation.value("reason"), true, 2048)) { return false; }
        if (operation.value("editable").toBool() && (!status.value("editable").toBool()
            || !textValid(status.value("context")) || !status.contains(field) || operation.value("pending").toBool())) { return false; }
    }
    return true;
}
quint64 nextRequest()
{
    static quint64 sequence = 0x5258524f00000000ULL;
    return ++sequence;
}
}

NativeReceiveRoutingWidget::NativeReceiveRoutingWidget(RadioModel& model, QWidget* parent)
    : QWidget(parent), m_model(model), m_availability(new ControlAvailabilityRegistry(model, this)),
      m_read(new QPushButton(tr("Read radio receive routing"), this)), m_context(new QLabel(this)), m_status(new QLabel(this))
{
    setObjectName("nativeReceiveRoutingSettings");
    auto* layout = new QVBoxLayout(this);
    auto* explanation = new QLabel(tr("These controls change the radio's receive antenna routing. Main ANT 1/2 selection and drive output are retained. "
        "External receiver antenna output splits the received signal and reduces receive gain by approximately 3 dB. "
        "The radio stores main antenna, RX ANT and drive settings in antenna-band memory. "
        "External receiver output is stored separately for HF and the combined 50/70 MHz range. "
        "Unknown values remain unavailable until radio readback; requested values are not confirmation."), this);
    explanation->setTextFormat(Qt::PlainText); explanation->setWordWrap(true); layout->addWidget(explanation);
    m_context->setObjectName("nativeReceiveRoutingContext"); m_context->setAccessibleName(tr("Confirmed receive routing context"));
    m_status->setObjectName("nativeReceiveRoutingStatus"); m_status->setAccessibleName(tr("Receive routing request status"));
    for (QLabel* label : {m_context, m_status}) { label->setTextFormat(Qt::PlainText); label->setWordWrap(true); }
    layout->addWidget(m_context);
    const QStringList labels{tr("Radio RX ANT input"), tr("External receiver antenna output")};
    for (int index = 0; index < 2; ++index) {
        const QString field = kFields[index];
        QCheckBox* control = new QCheckBox(labels[index], this); m_controls[index] = control;
        control->setObjectName("nativeReceiveRouting" + field); control->setAccessibleName(labels[index]);
        layout->addWidget(control);
        m_availability->registerWidget(control, tr("Requires a current confirmed and editable receive routing value; unknown, pending or retired values cannot be changed."),
            [this, field](bool connected, const RadioCapabilities& caps) {
                return connected && !routingNamespace(caps).isEmpty() && available(field);
            }, [this, field] { return m_confirmed.contains(field) && m_confirmed.value(field).toBool(); }, false);
        connect(control, &QCheckBox::clicked, this, [this, field](bool enabled) {
            const QVariantMap arguments{{"context", m_confirmed.value("context")}, {"value", enabled}};
            const QPointer<NativeReceiveRoutingWidget> alive(this); render();
            if (alive) { submit("receive.routing." + field + ".set", field, arguments); }
        });
    }
    m_read->setObjectName("nativeReceiveRoutingRead"); m_read->setAccessibleName(m_read->text()); m_read->setAutoDefault(false);
    m_availability->registerWidget(m_read, tr("Requires a connected radio reporting native receive routing control."),
        [this](bool connected, const RadioCapabilities& caps) {
            return connected && !routingNamespace(caps).isEmpty() && m_requests.isEmpty();
        }, {}, false);
    connect(m_read, &QPushButton::clicked, this, [this] { submit("receive.routing.get", {}); });
    layout->addWidget(m_read); layout->addWidget(m_status); layout->addStretch();
    connect(&model, &RadioModel::connectionStateChanged, this, &NativeReceiveRoutingWidget::bindBackend);
    connect(&model, &RadioModel::capabilitiesChanged, this, &NativeReceiveRoutingWidget::bindBackend);
    bindBackend();
}

bool NativeReceiveRoutingWidget::available(const QString& operation) const
{
    const QVariantMap state = m_confirmed.value("operations").toMap().value(operation).toMap();
    return m_requests.isEmpty() && m_confirmed.value("editable").toBool()
        && state.value("editable").toBool() && !state.value("pending").toBool();
}

void NativeReceiveRoutingWidget::bindBackend()
{
    IRadioBackend* backend = m_model.isConnected() ? m_model.backend() : nullptr;
    const QString ns = m_model.isConnected() ? routingNamespace(m_model.backendCapabilities()) : QString{};
    if (backend == m_backend && ns == m_namespace && m_receiveSession == m_model.receiveControlSession()) { render(); return; }
    const quint64 generation = ++m_bindingGeneration;
    m_receiveSession = m_model.receiveControlSession(); const RadioControlSession session = m_model.controlSession();
    disconnect(m_statusConnection); disconnect(m_resultConnection); disconnect(m_errorConnection);
    m_backend = backend; m_namespace = ns; m_confirmed.clear(); m_requests.clear(); m_statusRevision = 0;
    const QPointer<NativeReceiveRoutingWidget> alive(this); render();
    if (!alive || generation != m_bindingGeneration || !backend || ns.isEmpty()) { return; }
    const QPointer<IRadioBackend> source(backend);
    const auto current = [this, source, generation, session] {
        return source && generation == m_bindingGeneration && source == m_backend && source == m_model.backend()
            && m_model.isConnected() && m_model.controlSessionCurrent(session);
    };
    m_statusConnection = connect(backend, &IRadioBackend::extensionStatus, this,
        [this, current, ns](const QString& space, const QString& kind, const QVariantMap& status) {
            if (current() && space == ns && kind == QLatin1String("receiveRouting")) { acceptStatus(status); }
        });
    m_resultConnection = connect(backend, &IRadioBackend::extensionResult, this,
        [this, current](quint64 id, const QVariant& result) {
            if (!current() || !m_requests.contains(id)) { return; }
            const QString verb = m_requests.take(id);
            if (verb == QLatin1String("receive.routing.get")) {
                if (m_getSerial == m_statusSerial) { acceptStatus(result.toMap()); } else { render(); }
                return;
            }
            m_status->setText(result.typeId() == QMetaType::Bool && result.toBool()
                ? tr("Request sent. Routing awaits radio readback.") : tr("Invalid request result; routing remains unconfirmed."));
            render();
        });
    m_errorConnection = connect(backend, &IRadioBackend::extensionError, this,
        [this, current](quint64 id, const QString& error) {
            if (current() && m_requests.remove(id)) { m_status->setText(error); render(); }
        });
    submit("receive.routing.get", {});
}

void NativeReceiveRoutingWidget::acceptStatus(const QVariantMap& status)
{
    quint64 revision = 0;
    const bool ordered = revisionValid(status.value("statusRevision"), revision);
    if (ordered && revision < m_statusRevision) { return; }
    ++m_statusSerial;
    if (!ordered || !statusValid(status)) {
        m_confirmed.clear(); m_status->setText(tr("Invalid radio receive routing description. Read radio receive routing to retry."));
    } else { m_statusRevision = revision; m_confirmed = status; }
    render();
}

void NativeReceiveRoutingWidget::render()
{
    const QPointer<NativeReceiveRoutingWidget> alive(this);
    const quint64 generation = m_bindingGeneration; const quint64 serial = m_statusSerial;
    const QVariantMap snapshot = m_confirmed;
    const QString mode = snapshot.value("mode").toString();
    m_context->setText(tr("Confirmed receive mode: %1. %2").arg(mode.isEmpty() ? tr("Unknown") : mode, snapshot.value("reason").toString()));
    for (int index = 0; index < 2; ++index) {
        const QString field = kFields[index]; QCheckBox* control = m_controls[index];
        const bool known = snapshot.contains(field); const Qt::CheckState state = !known ? Qt::PartiallyChecked
            : snapshot.value(field).toBool() ? Qt::Checked : Qt::Unchecked;
        const bool changed = control->checkState() != state;
        {
            const QSignalBlocker blocker(control); control->setTristate(!known); control->setCheckState(state);
        }
        if (changed) {
            QAccessibleValueChangeEvent event(control, known ? QVariant(snapshot.value(field).toBool()) : QVariant(tr("Unknown")));
            QAccessible::updateAccessibility(&event);
            if (!alive || generation != m_bindingGeneration || serial != m_statusSerial) { return; }
        }
    }
    m_availability->refreshEngaged();
}

void NativeReceiveRoutingWidget::submit(const QString& verb, const QString& operation, const QVariantMap& arguments)
{
    if (!m_backend || m_backend != m_model.backend() || !m_model.isConnected() || m_namespace.isEmpty()
        || !m_requests.isEmpty() || (!operation.isEmpty() && !available(operation))) { return; }
    const quint64 request = nextRequest(); m_requests.insert(request, verb);
    if (verb == QLatin1String("receive.routing.get")) { m_getSerial = m_statusSerial; }
    const quint64 generation = m_bindingGeneration; const QPointer<NativeReceiveRoutingWidget> alive(this);
    render();
    if (!alive || generation != m_bindingGeneration || !m_requests.contains(request)) { return; }
    m_model.invokeBackendExtension(m_namespace, verb, request, arguments);
}
} // namespace AetherSDR
